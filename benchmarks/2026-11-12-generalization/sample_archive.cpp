// Sample by member-name hash, independently of compressed size or timing.
#include "../../archive.h"
#include "../../platform.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>

int main(int argc, char **argv) {
  if (argc != 4) {
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  slippack::Bytes packed{std::istreambuf_iterator<char>(input), {}};
  std::map<std::string, slippack::DecodedFile> chosen;
  std::mutex mutex;
  auto count = std::stoul(argv[3]);
  slippack::DecodeOptions options;
  options.threads = 4;
  options.wave_bytes = 256ull << 20;
  auto result = slippack::decode_archive(packed, options, [&](slippack::DecodedFile file) {
    if (std::filesystem::path(file.name).extension() != ".slp") {
      return;
    }
    auto hash =
        slippack::sha1({reinterpret_cast<const uint8_t *>(file.name.data()), file.name.size()});
    std::lock_guard lock(mutex);
    chosen.emplace(hash + ":" + file.name, std::move(file));
    if (chosen.size() > count) {
      chosen.erase(std::prev(chosen.end()));
    }
  });
  std::filesystem::create_directories(argv[2]);
  for (const auto &[key, file] : chosen) {
    auto path = std::filesystem::path(argv[2]) / (key.substr(0, 40) + ".slp");
    if (std::filesystem::exists(path)) {
      throw std::runtime_error("sample output already exists");
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(file.bytes.data()), file.bytes.size());
    if (!out) {
      throw std::runtime_error("sample write failed");
    }
  }
  std::cout << result.entries.size() << " members; " << chosen.size() << " sampled\n";
}
