// Command-line adapter. The archive engine is independent of paths and output.
#include "archive.h"
#include "file_io.h"
#include "record_codec.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace slippack;
namespace {
[[noreturn]] void usage() {
  fail("usage:\n"
       "  slippack c [-o out.slpc] [-l level] [-T threads] [-v] [--overwrite] "
       "<file.slp...|@listfile>\n"
       "  slippack l archive.slpc                    (original size and name per file)\n"
       "  slippack x archive.slpc -d outdir [-T threads] [-v] [--overwrite]\n"
       "             [--strip-components N | --flatten] [member...|@listfile]\n"
       "  slippack t archive.slpc [-T threads] [-v] [member...|@listfile]\n"
       "  slippack hash archive.slpc [-T threads] [member...|@listfile]\n"
       "                                             (sha1 per file, shasum format)\n"
       "  slippack selftest\n"
       "  Members are stored names as printed by l; without any, every file is processed.\n"
       "  -v, --verbose: c reports compressed bytes by section; x and t print each file's\n"
       "                 size and path as it completes.\n"
       "  Progress is reported on a terminal; --progress and --no-progress override that.\n"
       "  Default output: <input-stem>.slpc beside one input; archive.slpc for multiple inputs.\n"
       "  Decode limits: --max-archive-mib N --max-file-mib N --max-output-mib N\n"
       "                 --max-frame-mib N --max-pool-mib N --max-metadata-mib N --wave-mib N");
}
uint64_t number(const std::string &value) {
  uint64_t result = 0;
  auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  require(error == std::errc{} && end == value.data() + value.size(), "invalid numeric option");
  return result;
}
// Completed files and original bytes on stderr. A terminal gets one line
// redrawn in place; other destinations get a complete line once per second.
class Progress {
  const bool enabled_, terminal_;
  const char *verb_;
  const uint64_t total_;
  const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  std::atomic<uint64_t> files_{0}, bytes_{0};
  std::atomic<int64_t> due_{0};
  std::mutex mutex_;
  bool drawn_ = false;

public:
  Progress(bool enabled, const char *verb, uint64_t total)
      : enabled_(enabled), terminal_(isatty(STDERR_FILENO)), verb_(verb), total_(total) {}
  ~Progress() { clear(); }
  void add(uint64_t bytes) {
    uint64_t files = ++files_, done = bytes_ += bytes;
    if (!enabled_) {
      return;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start_)
                       .count();
    auto due = due_.load();
    if (elapsed < due || !due_.compare_exchange_strong(due, elapsed + (terminal_ ? 100 : 1000))) {
      return;
    }
    std::lock_guard lock(mutex_);
    fprintf(stderr, "%sslippack: %s %llu/%llu files, %.1f MB%s", terminal_ ? "\r\033[K" : "", verb_,
            (unsigned long long)files, (unsigned long long)total_, done / 1e6,
            terminal_ ? "" : "\n");
    fflush(stderr);
    drawn_ = true;
  }
  // Erases the in-place line so the summary or an error starts a clean line.
  void clear() {
    std::lock_guard lock(mutex_);
    if (drawn_ && terminal_) {
      fputs("\r\033[K", stderr);
      fflush(stderr);
    }
    drawn_ = false;
  }
};
// The extraction path for a stored name after dropping leading directories.
std::string output_name(const std::string &stored, uint64_t strip, bool flatten) {
  auto name = archive_name(stored);
  if (flatten) {
    return name.substr(name.rfind('/') + 1);
  }
  for (uint64_t i = 0; i < strip; ++i) {
    auto slash = name.find('/');
    if (slash == name.npos) {
      fail("too few path components for --strip-components: " + stored);
    }
    name = name.substr(slash + 1);
  }
  return name;
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
  bool progress = isatty(STDERR_FILENO), progressChosen = false, flatten = false, verbose = false;
  uint64_t strip = 0;
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
      require(!out.empty(), "output path must not be empty");
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
    } else if (argument == "-v" || argument == "--verbose") {
      encode.collect_stats = verbose = true;
    } else if (argument == "--progress") {
      progress = progressChosen = true;
    } else if (argument == "--no-progress") {
      progress = false;
      progressChosen = true;
    } else if (argument == "--flatten") {
      flatten = true;
    } else if (argument == "--strip-components") {
      strip = number(next());
      require(strip >= 1 && strip <= 4096, "--strip-components must be between 1 and 4096");
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
  if (std::getenv("SLIPPACK_TIMING")) {
    encode.on_phase = decode.on_phase = [](std::string_view phase, double seconds) {
      fprintf(stderr, "  [%.*s] %.3fs\n", int(phase.size()), phase.data(), seconds);
    };
  }
  // Per-file lines replace the in-place progress line unless it is requested.
  if (verbose && !progressChosen && (mode == "x" || mode == "t")) {
    progress = false;
  }
  require((!flatten && !strip) || mode == "x",
          "--strip-components and --flatten apply to extraction");
  require(!flatten || !strip, "--strip-components and --flatten are mutually exclusive");
  if (mode == "c") {
    if (paths.empty()) {
      usage();
    }
    if (out.empty()) {
      out = paths.size() == 1 ? paths.front() : fs::path("archive");
      out.replace_extension(".slpc");
      for (const auto &path : paths) {
        std::error_code error;
        require(!fs::equivalent(out, path, error),
                "default output is an input file; choose a different filename with -o");
      }
    }
    std::vector<ArchiveInput> inputs;
    Progress report(progress, "read", paths.size());
    for (const auto &path : paths) {
      inputs.push_back({source_name(path), [path, &report] {
                          auto bytes = read_file(path);
                          report.add(bytes.size());
                          return bytes;
                        }});
    }
    auto result = encode_archive(inputs, encode);
    report.clear();
    write_file(out, result.bytes, overwrite);
    fprintf(stderr, "slippack: wrote %s: %zu files, %.1f MB -> %.1f MB (%.2f%%)\n",
            out.string().c_str(), inputs.size(), result.original_bytes / 1e6,
            result.bytes.size() / 1e6,
            result.original_bytes ? 100.0 * result.bytes.size() / result.original_bytes : 0.0);
    if (encode.collect_stats) {
      print_stats(result.statistics);
    }
  } else if (mode == "l") {
    if (paths.size() != 1) {
      usage();
    }
    MappedFile archive(paths.front(), decode.limits.archive_bytes);
    auto result = list_archive(archive.bytes(), decode.limits);
    for (const auto &entry : result.entries) {
      printf("%llu  %s\n", (unsigned long long)entry.size, entry.name.c_str());
    }
    fprintf(stderr, "slippack: listed %zu files, %.1f MB\n", result.entries.size(),
            result.original_bytes / 1e6);
  } else if (mode == "x" || mode == "t" || mode == "hash") {
    if (paths.empty() || (mode == "x" && directory.empty())) {
      usage();
    }
    for (size_t i = 1; i < paths.size(); ++i) {
      decode.members.push_back(paths[i].string());
    }
    // Mapping avoids a second in-memory copy of the archive, and a selection
    // touches only a small part of it.
    MappedFile archive(paths.front(), decode.limits.archive_bytes);
    auto bytes = archive.bytes();
    // The directory gives the selection's size for progress and lets renamed
    // output paths be checked for collisions before any file is written.
    std::unordered_map<std::string, std::string> renamed;
    uint64_t selected = 0;
    if (progress || flatten || strip) {
      std::set<std::string> wanted, outputs;
      for (const auto &member : decode.members) {
        wanted.insert(archive_name(member));
      }
      for (const auto &entry : list_archive(bytes, decode.limits).entries) {
        if (!wanted.empty() && !wanted.contains(archive_name(entry.name))) {
          continue;
        }
        ++selected;
        if (flatten || strip) {
          auto name = output_name(entry.name, strip, flatten);
          if (!outputs.insert(name).second) {
            fail("extraction paths collide: " + name);
          }
          renamed.emplace(entry.name, std::move(name));
        }
      }
      for (const auto &name : outputs) {
        for (auto slash = name.find('/'); slash != name.npos; slash = name.find('/', slash + 1)) {
          if (outputs.contains(name.substr(0, slash))) {
            fail("extraction paths collide: " + name);
          }
        }
      }
    }
    Progress report(progress,
                    mode == "x"   ? "extracted"
                    : mode == "t" ? "tested"
                                  : "hashed",
                    selected);
    std::once_flag initialize;
    std::unique_ptr<ExtractionWriter> writer;
    std::mutex printing;
    // Files complete concurrently, so verbose lines follow completion order.
    auto completed = [&](uint64_t size, const std::string &name) {
      if (verbose) {
        std::lock_guard lock(printing);
        report.clear();
        printf("%llu  %s\n", (unsigned long long)size, name.c_str());
      }
      report.add(size);
    };
    FileSink sink;
    if (mode == "x") {
      sink = [&](DecodedFile file) {
        std::call_once(initialize,
                       [&] { writer = std::make_unique<ExtractionWriter>(directory, overwrite); });
        auto size = file.bytes.size();
        if (!renamed.empty()) {
          file.name = renamed.at(file.name);
        }
        auto name = file.name;
        writer->submit(std::move(file));
        completed(size, name);
      };
    } else if (mode == "t" && (progress || verbose)) {
      sink = [&](DecodedFile file) { completed(file.bytes.size(), file.name); };
    } else if (progress) {
      sink = [&](DecodedFile file) { report.add(file.bytes.size()); };
    }
    decode.compute_hashes = mode == "hash";
    auto result = decode_archive(bytes, decode, std::move(sink));
    if (writer) {
      writer->finish();
    }
    report.clear();
    for (const auto &entry : result.entries) {
      if (decode.compute_hashes) {
        printf("%s  %s\n", entry.sha1.c_str(), entry.name.c_str());
      }
    }
    fprintf(stderr, "slippack: %s %zu files\n",
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
    fprintf(stderr, "slippack: %s\n", error.what());
    return 1;
  }
}
