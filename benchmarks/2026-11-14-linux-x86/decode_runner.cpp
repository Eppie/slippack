// Decode without first running the encoder: allocator state from discarded
// compression trials otherwise changes this benchmark's memory behavior.
#include "archive.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <unordered_map>

using namespace slippack;
static Bytes read_bytes(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw Error("cannot open " + path.string());
  }
  auto length = file.tellg();
  if (length < 0) {
    throw Error("cannot size input");
  }
  Bytes bytes(static_cast<size_t>(length));
  file.seekg(0);
  if (length && !file.read(reinterpret_cast<char *>(bytes.data()), length)) {
    throw Error("cannot read input");
  }
  return bytes;
}
int main(int argc, char **argv) try {
  if (argc != 3) {
    throw Error("usage: decoder manifest reference-directory");
  }
  std::ifstream manifest(argv[1]);
  if (!manifest) {
    throw Error("cannot open manifest");
  }
  std::vector<Bytes> inputs;
  std::unordered_map<std::string, size_t> indexes;
  std::string path;
  uint64_t raw = 0;
  while (std::getline(manifest, path)) {
    auto name = std::filesystem::path(path).filename().string();
    if (!indexes.emplace(name, inputs.size()).second) {
      throw Error("duplicate input name");
    }
    inputs.push_back(read_bytes(path));
    raw += inputs.back().size();
  }
  std::cout << "{\"ready\":true,\"files\":" << inputs.size() << ",\"raw_bytes\":" << raw << "}"
            << std::endl;
  std::map<std::string, std::vector<Bytes>> archives;
  std::string layout;
  while (std::cin >> layout) {
    if (layout != "separate" && layout != "grouped") {
      throw Error("bad layout");
    }
    auto &packets = archives[layout];
    if (packets.empty()) {
      size_t count = layout == "grouped" ? 1 : inputs.size();
      for (size_t i = 0; i < count; ++i) {
        packets.push_back(
            read_bytes(std::filesystem::path(argv[2]) / layout / (std::to_string(i) + ".slpc")));
      }
    }
    std::vector<OutputBytes> restored(inputs.size());
    std::vector<unsigned> seen(inputs.size());
    DecodeOptions options;
    options.threads = 1;
    auto start = std::chrono::steady_clock::now();
    for (const auto &packet : packets) {
      decode_archive(packet, options, [&](DecodedFile file) {
        auto it = indexes.find(file.name);
        if (it == indexes.end() || ++seen[it->second] != 1) {
          throw Error("bad output name");
        }
        restored[it->second] = std::move(file.bytes);
      });
    }
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (seen[i] != 1 ||
          !std::equal(inputs[i].begin(), inputs[i].end(), restored[i].begin(), restored[i].end())) {
        throw Error("replay mismatch");
      }
    }
    std::cout << std::setprecision(12) << "{\"decode_seconds\":" << seconds
              << ",\"files_verified\":" << inputs.size() << "}" << std::endl;
  }
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
