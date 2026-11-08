#include "../custom_codec.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using namespace slpz_custom;
int main(int argc, char** argv) {
  std::mt19937 random(716018);
  size_t checks = 0;
  std::array<size_t, 5> packet_modes{};
  for (unsigned n : {0, 1, 2, 3, 7, 15, 31, 255, 1024, 4096, 16384, 100000}) {
    for (unsigned pattern = 0; pattern < 8; ++pattern) {
      Bytes input(n);
      for (unsigned i = 0; i < n; ++i) {
        input[i] = pattern == 0   ? 0
                   : pattern == 1 ? i % 17
                   : pattern == 2 ? random() % 4
                   : pattern == 3 ? random()
                   : pattern == 4 ? i / 137
                   : pattern == 5 ? (i % 101 < 98 ? 0 : random())
                   : pattern == 6 ? ((i / 1024) % 2 ? i % 19 : random())
                                  : uint8_t(i * 41);
      }
      for (auto profile : {Generic, Controller, FloatLow, Counter, Rng}) {
        auto packed = encode(input, profile);
        ++packet_modes.at(packed[0]);
        require(decode(packed, n, profile) == input, "codec roundtrip mismatch");
        ++checks;
        if (argc > 1 && n <= 65535) {
          std::filesystem::create_directories(argv[1]);
          auto path = std::filesystem::path(argv[1]) / std::to_string(checks);
          std::ofstream file(path, std::ios::binary);
          const uint8_t prefix[4] = {uint8_t(n), uint8_t(n >> 8), 0, uint8_t(profile)};
          file.write(reinterpret_cast<const char*>(prefix), 4);
          file.write(reinterpret_cast<const char*>(packed.data()), packed.size());
        }
        // Truncation must fail rather than overread or silently return a prefix.
        for (size_t cut : {size_t(0), packed.size() / 2, packed.size() - 1}) {
          try {
            (void)decode(View(packed).first(cut), n, profile);
            throw std::logic_error("accepted truncation");
          } catch (const Error&) {
          }
          ++checks;
        }
      }
      if (n) {
        Bytes planes, profiles;
        for (uint8_t p = 0; p < ProfileCount; ++p) {
          append(planes, input);
          profiles.push_back(p);
        }
        auto packed = encode_planes(planes, n, profiles);
        require(decode_planes(packed, narrow(planes.size())) == planes, "plane roundtrip mismatch");
        ++checks;
      }
    }
  }
  // Deliberately different field distributions must select and correctly
  // decode semantic probability tables rather than merely accepting labels.
  Bytes field_data, field_contexts, profiles;
  for (uint8_t p = 0; p < ProfileCount; ++p) {
    profiles.push_back(p);
    for (unsigned i = 0; i < 8192; ++i) {
      field_data.push_back(uint8_t(p * 32 + random() % 32));
      field_contexts.push_back(p);
    }
  }
  Bytes encoded_channel;
  channel(encoded_channel, field_data, field_contexts);
  Reader framing{encoded_channel};
  unsigned packed_size = framing.var();
  require(framing.var() == field_data.size(), "semantic channel size");
  require(framing.take(packed_size)[0] == 2, "semantic model did not win");
  Reader decode_channel{encoded_channel};
  require(unchannel(decode_channel, field_data.size(), field_contexts) == field_data,
          "semantic model roundtrip");
  auto field_planes = encode_planes(field_data, 8192, profiles);
  require(decode_planes(field_planes, narrow(field_data.size())) == field_data,
          "distinct field distributions roundtrip");
  checks += 2;
  for (auto count : packet_modes) require(count != 0, "packet mode not exercised");
  // Decoding corrupt data must stay bounded; accepted mutations may describe
  // different valid streams, so byte equality is not an oracle for this test.
  Bytes seed(8192);
  for (size_t i = 0; i < seed.size(); ++i) seed[i] = uint8_t(random() % 16);
  auto valid = encode(seed);
  for (unsigned i = 0; i < 1000; ++i) {
    auto mutated = valid;
    mutated[random() % mutated.size()] ^= uint8_t(1u << (random() % 8));
    try {
      (void)decode(mutated, narrow(seed.size()));
    } catch (const Error&) {
    }
    ++checks;
  }
  // Varints and models are hostile-input boundaries, not trusted metadata.
  for (Bytes malformed : {Bytes{2, 0, 255, 255, 255, 255, 127}, Bytes{3, 0}, Bytes{99}}) {
    try {
      (void)decode(malformed, 16);
      throw std::logic_error("accepted invalid header");
    } catch (const Error&) {
    }
    ++checks;
  }
  std::printf("custom codec: %zu checks passed\n", checks);
}
