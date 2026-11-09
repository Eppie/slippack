#include "../custom_codec.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using namespace slpz_custom;
int main(int argc, char** argv) {
  std::mt19937 random(716018);
  size_t checks = 0;
  std::array<size_t, 6> packet_modes{};
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
        if (argc > 1 && planes.size() <= 65535) {
          std::filesystem::create_directories(argv[1]);
          std::ofstream file(std::filesystem::path(argv[1]) / std::to_string(checks),
                             std::ios::binary);
          uint8_t prefix[4] = {uint8_t(planes.size()), uint8_t(planes.size() >> 8), 1, 0};
          file.write(reinterpret_cast<const char*>(prefix), 4);
          file.write(reinterpret_cast<const char*>(packed.data()), packed.size());
        }
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
  require(framing.take(packed_size)[0] == 4, "context model did not win");
  Reader decode_channel{encoded_channel};
  require(unchannel(decode_channel, field_data.size(), field_contexts) == field_data,
          "semantic model roundtrip");
  // The version-10 semantic channel remains readable.
  auto legacy_semantic = ans_candidate(field_data, field_contexts);
  Bytes legacy_framed;
  varint(legacy_framed, narrow(legacy_semantic.size()));
  varint(legacy_framed, narrow(field_data.size()));
  append(legacy_framed, legacy_semantic);
  Reader legacy_reader{legacy_framed};
  require(unchannel(legacy_reader, field_data.size(), field_contexts) == field_data,
          "legacy semantic channel");
  legacy_reader.done();
  ++checks;
  // Force the v15 presence path, including every short nibble/byte boundary.
  // Its selection heuristic must not determine decoder coverage.
  for (unsigned count :
       {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 127u, 1025u, 4097u}) {
    for (unsigned pattern = 0; pattern < 5; ++pattern) {
      size_t width = (count + 7) / 8;
      Bytes masks(width * 8, 0), refs(8),
          labels{FloatHigh, FloatLow, Controller, FloatLow, Discrete, FloatLow, Generic, FloatLow};
      for (unsigned j = 0; j < 8; ++j) {
        refs[j] = j ? 1 + random() % j : 0;
        for (unsigned i = 0; i < count; ++i) {
          bool bit = pattern == 0   ? false
                     : pattern == 1 ? true
                     : pattern == 2 ? bool(random() & 1)
                     : pattern == 3 ? i % 37 < 4
                                    : ((i / 7 + j) % 9 < 4);
          if (bit) masks[j * width + i / 8] |= 1u << (i % 8);
        }
      }
      auto packed = encode_presence4(masks, count, labels, refs);
      Reader reader{packed};
      require(decode_presence4(reader, count, labels, refs) == masks,
              "four-bit presence roundtrip");
      reader.done();
      ++checks;
    }
  }
  // Related planes share exception locations, including odd lengths and
  // references more than one plane back. Check the mask coder directly so
  // coverage does not depend on a cost heuristic selecting it.
  for (unsigned count : {1u, 3u, 7u, 9u, 31u, 127u, 1000u, 4097u}) {
    Bytes data(count * 8),
        labels{FloatHigh, FloatLow, Controller, FloatLow, Discrete, FloatLow, Generic, FloatLow};
    for (unsigned j = 0; j < 8; ++j)
      for (unsigned i = 0; i < count; ++i) {
        bool present = (i % 37 < 11) || (i % 101 == j);
        data[j * count + i] = present ? uint8_t(1 + random() % 255) : 0;
      }
    auto packed = masked_encode(data, count, labels, 8);
    Reader r{packed};
    require(masked_decode(r, count, labels) == data, "shared mask roundtrip");
    r.done();
    ++checks;
    for (size_t cut : {size_t(0), packed.size() / 2, packed.size() - 1}) {
      try {
        Reader bad{View(packed).first(cut)};
        (void)masked_decode(bad, count, labels);
        throw std::logic_error("accepted mask truncation");
      } catch (const Error&) {
      }
      ++checks;
    }
  }
  // Exhaust all eight-lane masks, including a nonzero dominant value.
  for (unsigned bits = 0; bits < 256; ++bits) {
    Bytes plane(8, 93), mask(1, 0), values;
    for (unsigned k = 0; k < 8; ++k)
      if (bits & (1u << k)) plane[k] = uint8_t(k + 17);
    extract_exceptions(plane, 93, mask, values);
    require(mask[0] == bits, "SIMD mask extraction");
    require(values.size() == size_t(std::popcount(bits)), "SIMD compact count");
    values.resize(values.size() + 8);
    Bytes restored(8), base{93, 0};
    restore_exceptions(mask, values, 8, base, restored);
    require(restored == plane, "SIMD scatter");
    ++checks;
  }
  // Every lookup in the product model must return a positive-frequency
  // symbol whose cumulative interval contains that slot.
  for (unsigned rep = 0; rep < 128; ++rep) {
    std::array<uint16_t, 256> p;
    for (auto& x : p) x = random() % 4097;
    PresenceTable m;
    m.build(random() % 512, p);
    for (unsigned slot = 0; slot < 4096; ++slot) {
      unsigned symbol = m.symbol(slot);
      require(m.freq[symbol] && slot >= m.start[symbol] && slot < m.start[symbol] + m.freq[symbol],
              "presence CDF");
    }
    ++checks;
  }
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
