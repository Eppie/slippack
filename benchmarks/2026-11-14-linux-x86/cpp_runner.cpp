// One codec library per executable. RAM-only timings; exact archive and replay
// comparisons run outside timers. Baseline packets are persisted for comparison.
#include "archive.h"
#include "platform.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace slippack;
using Clock = std::chrono::steady_clock;

static Bytes read_bytes(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw Error("cannot open " + path.string());
  }
  const auto length = file.tellg();
  if (length < 0) {
    throw Error("cannot size input");
  }
  Bytes result(static_cast<size_t>(length));
  file.seekg(0);
  if (length && !file.read(reinterpret_cast<char *>(result.data()), length)) {
    throw Error("cannot read input");
  }
  return result;
}

int main(int argc, char **argv) try {
  if (argc != 4) {
    throw Error("usage: runner manifest reference-directory write|check");
  }
  const bool write_reference = std::string(argv[3]) == "write";
  const std::filesystem::path reference_root(argv[2]);
  std::ifstream manifest(argv[1]);
  if (!manifest) {
    throw Error("cannot open manifest");
  }
  std::vector<Bytes> inputs;
  std::vector<std::string> names;
  std::unordered_map<std::string, size_t> indexes;
  uint64_t raw = 0;
  std::string path;
  while (std::getline(manifest, path)) {
    if (path.empty()) {
      continue;
    }
    auto name = std::filesystem::path(path).filename().string();
    if (!indexes.emplace(name, inputs.size()).second) {
      throw Error("duplicate name");
    }
    inputs.push_back(read_bytes(path));
    names.push_back(std::move(name));
    raw += inputs.back().size();
  }
  std::vector<ArchiveInput> sources;
  for (size_t i = 0; i < inputs.size(); ++i) {
    sources.push_back({names[i], [&, i] { return inputs[i]; }});
  }
  std::cout << "{\"ready\":true,\"files\":" << inputs.size() << ",\"raw_bytes\":" << raw << "}"
            << std::endl;

  std::unordered_map<std::string, std::vector<Bytes>> references;
  std::string grouping;
  while (std::cin >> grouping) {
    if (grouping != "separate" && grouping != "grouped") {
      throw Error("bad grouping");
    }
    const bool grouped = grouping == "grouped";
    const size_t count = grouped ? 1 : inputs.size();
    auto &expected = references[grouping];
    const auto reference_dir = reference_root / grouping;
    if (expected.empty() && !write_reference) {
      for (size_t i = 0; i < count; ++i) {
        expected.push_back(read_bytes(reference_dir / (std::to_string(i) + ".slpc")));
      }
    }
    std::vector<Bytes> compressed(count);
    std::vector<OutputBytes> restored(inputs.size());
    std::vector<unsigned> seen(inputs.size());
    EncodeOptions encode_options;
    encode_options.level = 3;
    encode_options.threads = 1;
    DecodeOptions decode_options;
    decode_options.threads = 1;

    auto start = Clock::now();
    if (grouped) {
      compressed[0] = encode_archive(sources, encode_options).bytes;
    } else {
      for (size_t i = 0; i < inputs.size(); ++i) {
        compressed[i] = encode_archive(std::span(&sources[i], 1), encode_options).bytes;
      }
    }
    double encode_seconds = std::chrono::duration<double>(Clock::now() - start).count();

    start = Clock::now();
    for (const auto &archive : compressed) {
      decode_archive(archive, decode_options, [&](DecodedFile file) {
        auto it = indexes.find(file.name);
        if (it == indexes.end() || ++seen[it->second] != 1) {
          throw Error("bad decoded name");
        }
        restored[it->second] = std::move(file.bytes);
      });
    }
    double decode_seconds = std::chrono::duration<double>(Clock::now() - start).count();

    for (size_t i = 0; i < inputs.size(); ++i) {
      if (seen[i] != 1 ||
          !std::equal(inputs[i].begin(), inputs[i].end(), restored[i].begin(), restored[i].end())) {
        throw Error("replay byte mismatch: " + names[i]);
      }
    }
    if (expected.empty()) {
      std::filesystem::create_directories(reference_dir);
      for (size_t i = 0; i < count; ++i) {
        auto target = reference_dir / (std::to_string(i) + ".slpc");
        if (std::filesystem::exists(target)) {
          throw Error("reference already exists");
        }
        std::ofstream file(target, std::ios::binary);
        file.write(reinterpret_cast<const char *>(compressed[i].data()), compressed[i].size());
        if (!file) {
          throw Error("cannot write reference");
        }
      }
      expected = compressed;
    }
    const bool archive_bytes_identical = compressed == expected;
    uint64_t total = 0;
    for (const auto &archive : compressed) {
      total += archive.size();
    }
    std::cout << std::setprecision(12) << "{\"encode_seconds\":" << encode_seconds
              << ",\"decode_seconds\":" << decode_seconds << ",\"compressed_bytes\":" << total
              << ",\"archive_bytes_identical\":" << (archive_bytes_identical ? "true" : "false")
              << ",\"files_verified\":" << inputs.size() << ",\"archives\":[";
    for (size_t i = 0; i < count; ++i) {
      if (i) {
        std::cout << ',';
      }
      std::cout << "{\"bytes\":" << compressed[i].size()
                << ",\"sha1\":" << std::quoted(sha1(compressed[i])) << '}';
    }
    std::cout << "]}" << std::endl;
  }
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
