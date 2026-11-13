#include "../custom_codec.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using namespace slippack_custom;
static size_t check_presence_histograms() {
  std::mt19937 rng(894257);
  size_t checks = 0;
  // Every bit count through two full four-symbol groups exercises their short
  // tails, reference boundaries, and state rotations between planes.
  for (unsigned count :
       {0u,  1u,   2u,   3u,   4u,   5u,   6u,   7u,    8u,    9u,    10u,  11u, 12u, 13u,
        14u, 15u,  16u,  17u,  18u,  19u,  20u,  21u,   22u,   23u,   24u,  25u, 26u, 27u,
        28u, 29u,  30u,  31u,  32u,  33u,  60u,  63u,   64u,   65u,   67u,  68u, 71u, 72u,
        73u, 127u, 128u, 129u, 191u, 192u, 193u, 1023u, 1024u, 1025u, 4097u}) {
    for (unsigned planes : {1u, 2u, 5u, 17u}) {
      for (unsigned pattern = 0; pattern < 10; ++pattern) {
        size_t width = (count + 7) / 8;
        Bytes masks(width * planes), profiles(planes), refs(planes);
        for (unsigned j = 0; j < planes; ++j) {
          profiles[j] = rng() % ProfileCount;
          refs[j] = j && pattern % 2 ? 1 + rng() % std::min(j, 8u) : 0;
          for (size_t k = 0; k < width; ++k) {
            unsigned value = pattern < 2   ? 0
                             : pattern < 4 ? 255
                             : pattern < 6 ? ((k / 8) & 1 ? 0 : 255)
                             : pattern < 8 ? (rng() % 20 ? 0 : rng())
                                           : rng();
            masks[j * width + k] = uint8_t(value);
          }
          if (count % 8) {
            masks[j * width + width - 1] &= (1u << (count % 8)) - 1;
          }
        }
        // Independent bit-by-bit counting includes the final nibble's zero padding.
        std::array<std::array<uint32_t, 2>, 256> hist{};
        auto bit = [&](unsigned plane, int64_t position) -> unsigned {
          return position >= 0 && uint64_t(position) < count
                     ? (masks[plane * width + size_t(position) / 8] >> (position % 8)) & 1
                     : 0;
        };
        for (unsigned j = 0; j < planes; ++j) {
          unsigned history = 0, cls = profiles[j] == FloatLow ? 128 : 0;
          for (unsigned i = 0; i < (count + 3) / 4 * 4; ++i) {
            unsigned above = refs[j]
                                 ? bit(j - refs[j], int64_t(i) - 1) | (bit(j - refs[j], i) << 1) |
                                       (bit(j - refs[j], int64_t(i) + 1) << 2)
                                 : 0;
            unsigned value = bit(j, i);
            ++hist[cls + history + above * 16][value];
            history = (history * 2 + value) & 15;
          }
        }
        auto packed = encode_presence4(masks, count, profiles, refs);
        Reader header{packed};
        unsigned mode = header.byte(), split = mode & 1, coding = mode >> 1, nc = 128u << split;
        require(mode <= 5, "presence header mode");
        View tags = coding ? header.take(nc / (coding == 1 ? 4 : 8)) : View{};
        for (unsigned c = 0; c < nc; ++c) {
          unsigned actual;
          if (!coding) {
            actual = header.var();
          } else if (coding == 2) {
            actual = (tags[c / 8] >> (c % 8)) & 1 ? header.var() : 4096;
          } else {
            unsigned tag = (tags[c / 4] >> (2 * (c % 4))) & 3;
            actual = tag == 0 ? 4096 : tag == 1 ? 0 : tag == 2 ? 4095 : header.var();
          }
          uint32_t zero = hist[c][0] + (split ? 0 : hist[c + 128][0]);
          uint32_t one = hist[c][1] + (split ? 0 : hist[c + 128][1]);
          unsigned expected = zero + one ? uint64_t(zero) * 4096 / (zero + one) : 4096;
          if (zero && one) {
            expected = std::clamp(expected, 1u, 4095u);
          }
          require(actual == expected, "presence bit-count oracle");
        }
        Reader r{packed};
        require(decode_presence4(r, count, profiles, refs) == masks,
                "uniform presence reconstruction");
        r.done();
        ++checks;
      }
    }
  }
  return checks;
}

struct ScratchRelocationProbe {
  static inline unsigned relocations = 0;
  ScratchRelocationProbe() = default;
  ScratchRelocationProbe(const ScratchRelocationProbe &) { ++relocations; }
  ScratchRelocationProbe(ScratchRelocationProbe &&) noexcept { ++relocations; }
};

static void check_scratch_leases() {
  {
    Scratch<ScratchRelocationProbe> first(4);
  }
  const size_t larger = Scratch<ScratchRelocationProbe>::cache().capacity() + 1;
  {
    Scratch<ScratchRelocationProbe> grown(larger);
  }
  require(ScratchRelocationProbe::relocations == 0, "relocated stale scratch entries");

  uint32_t *outer_pointer;
  {
    Scratch<uint32_t> outer(64);
    outer_pointer = outer.data.data();
    std::fill(outer.data.begin(), outer.data.end(), 4321);
    {
      Scratch<uint32_t> inner(32);
      require(inner.data.data() != outer_pointer, "nested scratch alias");
      std::fill(inner.data.begin(), inner.data.end(), 1234);
    }
    for (auto v : outer.data) {
      require(v == 4321, "nested scratch overwrite");
    }
  }
  {
    Scratch<uint32_t> reused(32);
    require(reused.data.data() == outer_pointer, "scratch not retained");
  }
  try {
    Scratch<uint32_t> temporary(64);
    throw Error("scratch unwind");
  } catch (const Error &) {
  }
  require(Scratch<uint32_t>::cache().capacity() == 64, "scratch lost during unwind");
  {
    Scratch<uint32_t> large((1u << 20) / sizeof(uint32_t) + 1);
  }
  require(Scratch<uint32_t>::cache().capacity() * sizeof(uint32_t) <= (1u << 20),
          "scratch retention cap");
}

static size_t check_constant_values() {

  unsigned cases = 0;
  for (unsigned count : {0u, 1u, 2u, 3u, 15u, 16u, 17u, 1024u, 4097u}) {
    for (unsigned planes : {1u, 2u}) {
      Bytes input(size_t(count) * planes), profiles(planes);
      for (unsigned j = 0; j < planes; ++j) {
        profiles[j] = j ? FloatLow : Generic;
        for (unsigned i = 0; i < count; ++i) {
          input[size_t(j) * count + i] = i % 2;
        }
      }
      auto packed = masked_encode(input, count, profiles, LzOptions{2, false, true, 1, false});
      Reader r{packed};
      require(masked_decode(r, count, profiles) == input, "constant exceptions roundtrip");
      r.done();
      if (count >= 2) {
        Reader shape{packed};
        shape.take(profiles.size() * 2);
        shape.byte();
        shape.take(shape.var());
        require(shape.byte() == 1 && shape.var() == 2, "constant exception channel selection");
        require(shape.var() == size_t(count / 2) * planes && shape.byte() == 3 && shape.byte() == 1,
                "constant exception framing");
        shape.done();
      }
      ++cases;
    }
  }
  try {
    Bytes input{0, 1}, labels{ProfileCount};
    (void)masked_encode(input, 2, labels, LzOptions{2, false, true, 1, false});
    throw std::logic_error("accepted invalid constant profile");
  } catch (const Error &) {
    ++cases;
  }
  return cases;
}

static size_t check_mask_boundaries() {
  size_t checks = 0;
  std::mt19937 random(193560);
  for (unsigned count : {0u, 1u, 7u, 1023u, 1024u, 1025u}) {
    for (unsigned planes : {3u, 4u}) {
      for (auto profile : {FloatLow, Controller}) {
        for (unsigned pattern = 0; pattern < 2; ++pattern) {
          Bytes input(size_t(count) * planes), profiles(planes, profile);
          for (unsigned j = 0; j < planes; ++j) {
            for (unsigned i = 0; i < count; ++i) {
              if ((i + j % 2) % 7 == 0) {
                input[size_t(j) * count + i] = pattern ? uint8_t(1 + random() % 255) : 1;
              }
            }
          }
          auto packed = masked_encode(input, count, profiles, LzOptions{2, false, true, 2, false});
          Reader reader{packed};
          require(masked_decode(reader, count, profiles) == input, "mask boundary roundtrip");
          reader.done();
          ++checks;
          if (count == 1024 && planes == 4 && profile == FloatLow && pattern) {
            for (size_t cut :
                 {size_t(0), size_t(1), size_t(2), packed.size() / 2, packed.size() - 1}) {
              try {
                Reader truncated{View(packed).first(cut)};
                (void)masked_decode(truncated, count, profiles);
                throw std::logic_error("accepted truncated mask packet");
              } catch (const Error &) {
              }
              ++checks;
            }
          }
        }
      }
    }
  }
  return checks;
}
int main(int argc, char **argv) {
  check_scratch_leases();
  std::mt19937 random(716018);
  size_t checks = 0;
  std::array<size_t, 6> packet_modes{};
  auto check_statistics = [&](View input) {
    std::array<uint32_t, 256> expected{}, actual;
    actual.fill(UINT32_MAX); // The helper must overwrite every output bin.
    uint32_t runs = 0;
    for (size_t i = 0; i < input.size(); ++i) {
      ++expected[input[i]];
      runs += i == 0 || input[i] != input[i - 1];
    }
    require(histogram_runs(input, actual) == runs && actual == expected,
            "byte histogram/run oracle");
    auto maximum = *std::max_element(expected.begin(), expected.end());
    bool special = !input.empty() &&
                   (input.size() - maximum <= input.size() / 128 || runs <= input.size() / 128);
    require(sparse_or_few_runs(input) == special, "special plane trial oracle");
    checks += 2;
  };
  // Exercise threshold changes, block tails, first-byte outliers, and a
  // candidate prefix whose majority differs from the full plane's majority.
  for (size_t n : {1u, 7u, 8u, 9u, 63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u, 1023u, 1024u,
                   1025u, 8193u}) {
    size_t limit = n / 128;
    for (uint8_t base : {0u, 37u, 255u}) {
      for (size_t exceptions : {limit, limit + 1, 2 * limit + 1}) {
        Bytes input(n, base);
        for (size_t i = 0; i < exceptions; ++i) {
          input[i] = base ^ 127;
        }
        check_statistics(input);
        std::shuffle(input.begin(), input.end(), random);
        check_statistics(input);
      }
      for (size_t runs : {std::max<size_t>(1, limit), limit + 1}) {
        Bytes input(n);
        for (size_t i = 0; i < n; ++i) {
          input[i] = uint8_t(base + i * runs / n);
        }
        check_statistics(input);
      }
    }
  }
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
      check_statistics(input);
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
          file.write(reinterpret_cast<const char *>(prefix), 4);
          file.write(reinterpret_cast<const char *>(packed.data()), packed.size());
        }
        // Truncation must fail rather than overread or silently return a prefix.
        for (size_t cut : {size_t(0), packed.size() / 2, packed.size() - 1}) {
          try {
            (void)decode(View(packed).first(cut), n, profile);
            throw std::logic_error("accepted truncation");
          } catch (const Error &) {
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
        for (LzOptions options :
             {LzOptions{2, false, false}, LzOptions{2, false, true}, LzOptions{2, true, true},
              LzOptions{2, false, true, 1, false}, LzOptions{2, false, true, 2, false},
              LzOptions{4, true, true}, LzOptions{24, true, false}}) {
          auto fast_packed = encode_planes(planes, n, profiles, options);
          require(decode_planes(fast_packed, narrow(planes.size())) == planes,
                  "fast plane roundtrip");
          ++checks;
        }
        if (argc > 1 && planes.size() <= 65535) {
          std::filesystem::create_directories(argv[1]);
          std::ofstream file(std::filesystem::path(argv[1]) / std::to_string(checks),
                             std::ios::binary);
          uint8_t prefix[4] = {uint8_t(planes.size()), uint8_t(planes.size() >> 8), 1, 0};
          file.write(reinterpret_cast<const char *>(prefix), 4);
          file.write(reinterpret_cast<const char *>(packed.data()), packed.size());
        }
      }
    }
  }
  // Force the tree's leaf limit with occupied and empty history buckets.
  // Terminal leaves must remain complete when further splitting stops.
  for (unsigned pattern = 0; pattern < 3; ++pattern) {
    Bytes input;
    input.reserve(262144);
    for (unsigned repeat = 0; repeat < 512; ++repeat) {
      for (unsigned previous = 0; previous < 256; ++previous) {
        input.push_back(uint8_t(previous));
        input.push_back(uint8_t(pattern == 0   ? previous ^ 90
                                : pattern == 1 ? previous * 37
                                               : previous));
      }
    }
    auto encoded = context_encode(input, {});
    Reader shape{encoded};
    require(shape.byte() == 1 && shape.byte() == 16, "full context tree fixture");
    Reader reader{encoded};
    require(context_decode_precision<10>(reader, input.size(), {}, true) == input,
            "terminal context leaf reconstruction");
    reader.done();
    ++checks;
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
  require(framing.take(packed_size)[0] == 8, "context model did not win");
  Reader decode_channel{encoded_channel};
  require(unchannel(decode_channel, field_data.size(), field_contexts) == field_data,
          "semantic model roundtrip");
  // Force the self-described semantic ANS channel as well as the context tree.
  auto semantic_candidate = ans_candidate(field_data, field_contexts);
  Bytes semantic_framed;
  varint(semantic_framed, narrow(semantic_candidate.size()));
  varint(semantic_framed, narrow(field_data.size()));
  append(semantic_framed, semantic_candidate);
  Reader semantic_reader{semantic_framed};
  require(unchannel(semantic_reader, field_data.size(), field_contexts) == field_data,
          "semantic channel");
  semantic_reader.done();
  ++checks;
  // Force four-bit presence coding, including every short nibble/byte boundary.
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
          if (bit) {
            masks[j * width + i / 8] |= 1u << (i % 8);
          }
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
    for (unsigned j = 0; j < 8; ++j) {
      for (unsigned i = 0; i < count; ++i) {
        bool present = (i % 37 < 11) || (i % 101 == j);
        data[j * count + i] = present ? uint8_t(1 + random() % 255) : 0;
      }
    }
    auto packed = masked_encode(data, count, labels, {});
    Reader r{packed};
    require(masked_decode(r, count, labels) == data, "shared mask roundtrip");
    r.done();
    ++checks;
    for (size_t cut : {size_t(0), packed.size() / 2, packed.size() - 1}) {
      try {
        Reader bad{View(packed).first(cut)};
        (void)masked_decode(bad, count, labels);
        throw std::logic_error("accepted mask truncation");
      } catch (const Error &) {
      }
      ++checks;
    }
  }
  // Exhaust all eight-lane masks, including a nonzero dominant value.
  for (unsigned bits = 0; bits < 256; ++bits) {
    Bytes plane(8, 93), mask(1, 0), values;
    for (unsigned k = 0; k < 8; ++k) {
      if (bits & (1u << k)) {
        plane[k] = uint8_t(k + 17);
      }
    }
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
    for (auto &x : p) {
      x = random() % 4097;
    }
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
  for (auto count : packet_modes) {
    require(count != 0, "packet mode not exercised");
  }
  // Decoding corrupt data must stay bounded; accepted mutations may describe
  // different valid streams, so byte equality is not an oracle for this test.
  Bytes seed(8192);
  for (size_t i = 0; i < seed.size(); ++i) {
    seed[i] = uint8_t(random() % 16);
  }
  auto valid = encode(seed);
  for (unsigned i = 0; i < 1000; ++i) {
    auto mutated = valid;
    mutated[random() % mutated.size()] ^= uint8_t(1u << (random() % 8));
    try {
      (void)decode(mutated, narrow(seed.size()));
    } catch (const Error &) {
    }
    ++checks;
  }
  // Varints and models are hostile-input boundaries, not trusted metadata.
  for (Bytes malformed : {Bytes{2, 0, 255, 255, 255, 255, 127}, Bytes{3, 0}, Bytes{99}}) {
    try {
      (void)decode(malformed, 16);
      throw std::logic_error("accepted invalid header");
    } catch (const Error &) {
    }
    ++checks;
  }
  checks += check_constant_values();
  checks += check_mask_boundaries();
  checks += check_presence_histograms();
  std::printf("custom codec: %zu checks passed\n", checks);
}
