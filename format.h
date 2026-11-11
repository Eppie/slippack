#pragma once
#include "binary_io.h"
#include <array>
#include <atomic>

namespace slippack {
constexpr uint32_t kMagic = 0x43504C53; // "SLPC" little-endian
constexpr uint8_t kVersion = 1;
constexpr uint8_t kCustom = 6;    // tagged custom frame codec ID
constexpr uint8_t kModeRaw = 0;   // whole file = one custom frame
constexpr uint8_t kModeSplit = 1; // parsed event streams

constexpr size_t kSlpHeaderLen = 15; // "{U\x03raw[$U#l" + u32 BE raw length
constexpr char kSlpMagic[] = "{U\x03raw[$U#l";

enum : uint8_t { T_PLAIN = 0, T_SUB = 1, T_XOR = 2 };

// Internal counters are owned by one encoder invocation.
struct Stats {
  std::atomic<uint64_t> byCmd[256]{};
  std::atomic<uint64_t> order{0}, suffix{0}, tail{0}, selector{0}, misc{0};
  std::atomic<uint64_t> seeds{0}, seedHit{0}, seedMiss{0};
};

} // namespace slippack
