#pragma once

// SLIPPACK custom payload codec. No compression-library dependency.
// Four interleaved byte-rANS states, semantic literal contexts, explicit
// constant/sparse/run modes, and a bounded lazy LZ parser.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
#if defined(__aarch64__) && !defined(SLIPPACK_CUSTOM_SCALAR)
#include <arm_neon.h>
#endif

#include "binary_io.h"

namespace slippack_custom {
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;
enum Profile : uint8_t {
  Generic,
  Controller,
  FloatHigh,
  FloatLow,
  Rng,
  RngContinuation,
  Counter,
  Discrete,
  ProfileCount
};
using slippack::append;
using slippack::Error;
using slippack::narrow;
using slippack::Reader;
using slippack::require;
using slippack::u32;
using slippack::varint;

// Search depth, lookahead, and literal prediction are separate encoder choices.
struct LzOptions {
  unsigned depth = 8;
  bool lazy = true;
  bool predict = true;
  unsigned recent_limit = 4;
  bool short_matches = true;
};

// Modes 8/9/10 have a fixed ten-bit probability scale.
constexpr unsigned EncoderBits = 10;
static_assert(EncoderBits == 10 && std::has_single_bit(unsigned(ProfileCount)));

// A lease takes the entire cache so nested calls cannot alias live scratch.
// Retain at most 1 MiB per type and thread, discarding larger one-off buffers.
template <class T> struct Scratch {
  using Storage = std::vector<T, slippack::OutputAllocator<T>>;
  static Storage &cache() {
    static thread_local Storage value;
    return value;
  }
  Storage data;
  explicit Scratch(size_t size) : data(std::move(cache())) {
    // The old lease may have left entries uninitialized. Keep its allocation,
    // but never relocate those entries if the new request needs more capacity.
    data.clear();
    data.resize(size);
  }
  Scratch(const Scratch &) = delete;
  Scratch &operator=(const Scratch &) = delete;
  ~Scratch() {
    auto &saved = cache();
    if (data.capacity() * sizeof(T) <= (1u << 20) && data.capacity() > saved.capacity()) {
      saved = std::move(data);
    }
  }
  T &operator[](size_t i) { return data[i]; }
  const T &operator[](size_t i) const { return data[i]; }
};
struct Model {
  std::array<uint32_t, 256> hist{};
  std::array<uint16_t, 256> freq{}, start{};
  void normalize(unsigned precision = 12) {
    const int scale = 1 << precision;
    uint64_t total = 0;
    for (auto n : hist) {
      total += n;
    }
    if (!total) {
      freq[0] = scale;
      return;
    }
    int sum = 0;
    for (unsigned s = 0; s < 256; ++s) {
      if (hist[s]) {
        // Nearest rounding avoids systematically moving every fractional
        // frequency into the most common symbol during sum correction.
        freq[s] = uint16_t(std::max<uint64_t>(1, (uint64_t(hist[s]) * scale + total / 2) / total));
        sum += freq[s];
      }
    }
    while (sum != scale) {
      auto it = std::max_element(freq.begin(), freq.end());
      if (sum < scale) {
        *it += uint16_t(scale - sum);
        sum = scale;
      } else {
        int d = std::min(sum - scale, int(*it) - 1);
        require(d > 0, "custom normalization");
        *it -= d;
        sum -= d;
      }
    }
    unsigned at = 0;
    for (unsigned s = 0; s < 256; ++s) {
      start[s] = uint16_t(at);
      at += freq[s];
    }
  }
};

// Compact alphabets, an implied first frequency, and Rice-coded frequencies.
inline void write_compact_model(Bytes &out, const std::array<uint16_t, 256> &freq,
                                unsigned scale = 4096) {
  std::array<uint8_t, 256> symbols;
  unsigned used = 0;
  for (unsigned b = 0; b < 256; ++b) {
    if (freq[b]) {
      symbols[used++] = uint8_t(b);
    }
  }
  require(used > 0, "compact alphabet");
  varint(out, used > 32 ? 0 : used);
  if (used > 32) {
    for (unsigned byte = 0; byte < 32; ++byte) {
      unsigned bits = 0;
      for (unsigned bit = 0; bit < 8; ++bit) {
        bits |= unsigned(freq[byte * 8 + bit] != 0) << bit;
      }
      out.push_back(uint8_t(bits));
    }
  } else {
    out.insert(out.end(), symbols.begin(), symbols.begin() + used);
  }
  if (used == 1) {
    return;
  }
  unsigned mean = (scale - freq[symbols[0]]) / (used - 1);
  unsigned k = std::bit_width(mean) > 1 ? std::bit_width(mean) - 2 : 0;
  unsigned bits[2] = {}, raw = 0;
  for (unsigned i = 1; i < used; ++i) {
    unsigned f = freq[symbols[i]], v = f - 1;
    bits[0] += (v >> k) + 1 + k;
    bits[1] += (v >> (k + 1)) + 2 + k;
    raw += f < 128 ? 1 : 2;
  }
  unsigned which = bits[1] < bits[0];
  k += which;
  bool rice = (bits[which] + 7) / 8 < raw;
  out.push_back(uint8_t(rice ? k : 12));
  if (!rice) {
    for (unsigned i = 1; i < used; ++i) {
      varint(out, freq[symbols[i]]);
    }
    return;
  }
  uint64_t pending = 0;
  unsigned count = 0;
  auto put = [&](uint32_t value, unsigned n) {
    pending |= uint64_t(value) << count;
    count += n;
    while (count >= 8) {
      out.push_back(uint8_t(pending));
      pending >>= 8;
      count -= 8;
    }
  };
  for (unsigned i = 1; i < used; ++i) {
    unsigned v = freq[symbols[i]] - 1, q = v >> k;
    while (q >= 32) {
      put(0, 32);
      q -= 32;
    }
    put(1u << q, q + 1);
    put(v & ((1u << k) - 1), k);
  }
  if (count) {
    out.push_back(uint8_t(pending));
  }
}
inline void read_compact_model(Reader &r, std::array<uint16_t, 256> &freq, unsigned scale = 4096) {
  freq.fill(0);
  std::array<uint8_t, 256> symbols;
  unsigned used = r.var();
  if (!used) {
    auto alphabet = r.take(32);
    for (unsigned b = 0; b < 256; ++b) {
      if ((alphabet[b / 8] >> (b % 8)) & 1) {
        symbols[used++] = uint8_t(b);
      }
    }
    require(used, "compact alphabet");
  } else {
    require(used <= 256, "compact alphabet");
    for (unsigned i = 0; i < used; ++i) {
      symbols[i] = r.byte();
      require(!i || symbols[i] > symbols[i - 1], "compact symbol order");
    }
  }
  if (used == 1) {
    freq[symbols[0]] = scale;
    return;
  }
  unsigned k = r.byte();
  require(k <= 12, "compact frequency coding");
  unsigned pending = 0, count = 0, sum = 0;
  auto get = [&](unsigned n) {
    while (count < n) {
      pending |= unsigned(r.byte()) << count;
      count += 8;
    }
    unsigned value = pending & ((1u << n) - 1);
    pending >>= n;
    count -= n;
    return value;
  };
  for (unsigned i = 1; i < used; ++i) {
    unsigned f;
    if (k == 12) {
      f = r.var();
    } else {
      unsigned q = 0;
      while (!pending) {
        q += count;
        require(q <= ((scale - 1) >> k), "compact unary frequency");
        pending = r.byte();
        count = 8;
      }
      unsigned zeros = std::countr_zero(pending);
      q += zeros;
      require(q <= ((scale - 1) >> k), "compact unary frequency");
      pending >>= zeros + 1;
      count -= zeros + 1;
      f = (q << k) + get(k) + 1;
    }
    require(f > 0 && f < scale && sum + f < scale, "compact frequency sum");
    sum += f;
    freq[symbols[i]] = uint16_t(f);
  }
  require(!pending, "compact frequency padding");
  freq[symbols[0]] = uint16_t(scale - sum);
}

} // namespace slippack_custom
