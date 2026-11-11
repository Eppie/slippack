#pragma once

// SLPZ custom payload codec. No compression-library dependency.
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
#if defined(__aarch64__) && !defined(SLPZ_CUSTOM_SCALAR)
#include <arm_neon.h>
#endif

#include "binary_io.h"

namespace slpz_custom {
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
using slpz::append;
using slpz::Error;
using slpz::narrow;
using slpz::Reader;
using slpz::require;
using slpz::u32;
using slpz::varint;

struct Model {
  std::array<uint32_t, 256> hist{};
  std::array<uint16_t, 256> freq{}, start{};
  void normalize() {
    uint64_t total = 0;
    for (auto n : hist) {
      total += n;
    }
    if (!total) {
      freq[0] = 4096;
      return;
    }
    int sum = 0;
    for (unsigned s = 0; s < 256; ++s) {
      if (hist[s]) {
        // Nearest rounding avoids systematically moving every fractional
        // frequency into the most common symbol during sum correction.
        freq[s] = uint16_t(std::max<uint64_t>(1, (uint64_t(hist[s]) * 4096 + total / 2) / total));
        sum += freq[s];
      }
    }
    while (sum != 4096) {
      auto it = std::max_element(freq.begin(), freq.end());
      if (sum < 4096) {
        *it += uint16_t(4096 - sum);
        sum = 4096;
      } else {
        int d = std::min(sum - 4096, int(*it) - 1);
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

} // namespace slpz_custom
