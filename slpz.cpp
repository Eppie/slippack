// Command-line adapter. The archive engine is independent of paths and output.
#include "archive.h"
#include "file_io.h"
#include "record_codec.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using namespace slpz;
namespace {
[[noreturn]] void usage() {
  fail("usage:\n"
       "  slpz c -o out.slpz [-l level] [-T threads] [-v] [--overwrite] <file.slp...|@listfile>\n"
       "  slpz x archive.slpz -d outdir [-T threads] [--overwrite]\n"
       "  slpz t archive.slpz [-T threads]\n"
       "  slpz hash archive.slpz [-T threads]   (sha1 per file, shasum format)\n"
       "  slpz selftest\n"
       "  Decode limits: --max-archive-mib N --max-file-mib N --max-output-mib N\n"
       "                 --max-frame-mib N --max-pool-mib N --max-metadata-mib N --wave-mib N");
}
uint64_t number(const std::string &value) {
  uint64_t result = 0;
  auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  require(error == std::errc{} && end == value.data() + value.size(), "invalid numeric option");
  return result;
}
void print_stats(const Statistics &stats) {
  fprintf(stderr, "  compressed bytes by section:\n");
  for (unsigned c = 0; c < stats.by_command.size(); ++c) {
    if (stats.by_command[c]) {
      fprintf(stderr, "    cmd 0x%02x : %10llu\n", c, (unsigned long long)stats.by_command[c]);
    }
  }
  for (auto [label, value] : {std::pair{"order", stats.order},
                              {"selector", stats.selector},
                              {"metadata", stats.suffix},
                              {"tail", stats.tail},
                              {"misc", stats.misc}}) {
    fprintf(stderr, "    %-8s : %10llu\n", label, (unsigned long long)value);
  }
  fprintf(stderr, "    seeds    : %10llu  (folded %llu, exceptions %llu)\n",
          (unsigned long long)stats.seeds, (unsigned long long)stats.seed_hits,
          (unsigned long long)stats.seed_misses);
}
int run(int argc, char **argv) {
  if (argc < 2) {
    usage();
  }
  std::string mode = argv[1];
  if (mode == "selftest") {
    return cmd_selftest();
  }
  if (argc < 3) {
    usage();
  }
  fs::path out, directory;
  EncodeOptions encode;
  DecodeOptions decode;
  encode.threads = decode.threads = int(std::clamp(std::thread::hardware_concurrency(), 1u, 256u));
  Overwrite overwrite = Overwrite::Refuse;
  std::vector<fs::path> paths;
  bool positional = false;
  for (int i = 2; i < argc; ++i) {
    std::string argument = argv[i];
    auto next = [&]() -> std::string {
      if (++i >= argc) {
        usage();
      }
      return argv[i];
    };
    auto mib = [&] {
      auto n = number(next());
      require(n <= (UINT64_MAX >> 20), "size limit overflow");
      return n << 20;
    };
    if (positional) {
      paths.emplace_back(argument);
    } else if (argument == "--") {
      positional = true;
    } else if (argument == "-o") {
      out = next();
    } else if (argument == "-d") {
      directory = next();
    } else if (argument == "-l") {
      auto n = number(next());
      require(n >= 1 && n <= 12, "level must be between 1 and 12");
      encode.level = int(n);
    } else if (argument == "-T") {
      auto n = number(next());
      require(n >= 1 && n <= 256, "threads must be between 1 and 256");
      encode.threads = decode.threads = int(n);
    } else if (argument == "--overwrite") {
      overwrite = Overwrite::Replace;
    } else if (argument == "-v") {
      encode.collect_stats = true;
    } else if (argument == "--codec") {
      require(next() == "custom", "only the custom codec is supported");
    } else if (argument == "--max-archive-mib") {
      decode.limits.archive_bytes = mib();
    } else if (argument == "--max-file-mib") {
      decode.limits.file_bytes = mib();
    } else if (argument == "--max-output-mib") {
      decode.limits.total_bytes = mib();
    } else if (argument == "--max-frame-mib") {
      decode.limits.frame_bytes = mib();
    } else if (argument == "--max-pool-mib") {
      decode.limits.pool_bytes = mib();
    } else if (argument == "--max-metadata-mib") {
      decode.limits.metadata_bytes = mib();
    } else if (argument == "--wave-mib") {
      decode.wave_bytes = mib();
    } else if (!argument.empty() && argument[0] == '@') {
      std::ifstream list(argument.substr(1));
      if (!list) {
        fail("cannot open list file " + argument.substr(1));
      }
      for (std::string line; std::getline(list, line);) {
        if (!line.empty()) {
          paths.emplace_back(line);
        }
      }
      if (list.bad()) {
        fail("cannot read list file " + argument.substr(1));
      }
    } else if (!argument.empty() && argument[0] == '-') {
      fail("unknown option " + argument);
    } else {
      paths.emplace_back(argument);
    }
  }
  if (std::getenv("SLPZ_TIMING")) {
    encode.on_phase = decode.on_phase = [](std::string_view phase, double seconds) {
      fprintf(stderr, "  [%.*s] %.3fs\n", int(phase.size()), phase.data(), seconds);
    };
  }
  if (mode == "c") {
    if (out.empty() || paths.empty()) {
      usage();
    }
    std::vector<ArchiveInput> inputs;
    for (const auto &path : paths) {
      inputs.push_back({source_name(path), [path] { return read_file(path); }});
    }
    auto result = encode_archive(inputs, encode);
    write_file(out, result.bytes, overwrite);
    fprintf(stderr, "slpz: %zu files, %.1f MB -> %.1f MB (%.2f%%)\n", inputs.size(),
            result.original_bytes / 1e6, result.bytes.size() / 1e6,
            result.original_bytes ? 100.0 * result.bytes.size() / result.original_bytes : 0.0);
    if (encode.collect_stats) {
      print_stats(result.statistics);
    }
  } else if (mode == "x" || mode == "t" || mode == "hash") {
    if (paths.size() != 1 || (mode == "x" && directory.empty())) {
      usage();
    }
    auto bytes = read_file(paths.front(), decode.limits.archive_bytes);
    std::once_flag initialize;
    std::unique_ptr<ExtractionWriter> writer;
    FileSink sink;
    if (mode == "x") {
      sink = [&](DecodedFile file) {
        std::call_once(initialize,
                       [&] { writer = std::make_unique<ExtractionWriter>(directory, overwrite); });
        writer->submit(std::move(file));
      };
    }
    decode.compute_hashes = mode == "hash";
    auto result = decode_archive(bytes, decode, std::move(sink));
    if (writer) {
      writer->finish();
    }
    for (const auto &entry : result.entries) {
      if (decode.compute_hashes) {
        printf("%s  %s\n", entry.sha1.c_str(), entry.name.c_str());
      }
    }
    fprintf(stderr, "slpz: %s %zu files\n",
            mode == "x"   ? "extracted"
            : mode == "t" ? "tested"
                          : "hashed",
            result.entries.size());
  } else {
    usage();
  }
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    fprintf(stderr, "slpz: %s\n", error.what());
    return 1;
  }
}
