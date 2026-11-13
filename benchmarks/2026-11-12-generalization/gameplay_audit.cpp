// Link against the frozen baseline library; no candidate encoding is performed.
#include "platform.h"
#include "slp.h"
#include <fstream>
#include <iostream>
#include <limits>

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  std::ifstream manifest(argv[1]);
  if (!manifest) {
    return 2;
  }
  size_t index = 0;
  for (std::string path; std::getline(manifest, path); ++index) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
      return 2;
    }
    auto length = file.tellg();
    if (length < 0 || uint64_t(length) > std::numeric_limits<uint32_t>::max()) {
      return 2;
    }
    slippack::Bytes bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) {
      return 2;
    }
    slippack::ParsedSlp parsed;
    bool usable = slippack::parse_slp(bytes, parsed) && !parsed.streams.empty();
    std::cout << "{\"index\":" << index << ",\"usable\":" << (usable ? "true" : "false");
    if (usable) {
      size_t end = parsed.tail.data() - bytes.data();
      if (end < slippack::kSlpHeaderLen || end > bytes.size()) {
        return 2;
      }
      auto events = std::span<const uint8_t>(bytes).subspan(slippack::kSlpHeaderLen,
                                                            end - slippack::kSlpHeaderLen);
      std::cout << ",\"offset\":" << slippack::kSlpHeaderLen << ",\"event_bytes\":" << events.size()
                << ",\"event_sha1\":\"" << slippack::sha1(events) << '"';
    }
    std::cout << "}\n";
  }
}
