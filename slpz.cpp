// slpz — Slippi (.slp) replay-specific solid-archive compressor.
//
// Idea: a .slp file is a UBJSON wrapper around an event stream of fixed-size
// records (sizes self-declared by the 0x35 Event Payloads event). Records of
// the same event type are highly similar frame to frame, so we:
//   1. split the event stream into one buffer per command byte (and per
//      player for pre/post-frame events),
//   2. transpose each buffer so byte-plane k of every record is contiguous
//      (same idea as Blosc shuffle — puts slowly-changing bytes together),
//   3. per plane, pick plain / subtract-delta / xor-delta by an order-0
//      entropy probe (sub suits counters, xor suits the BE floats),
//   4. compress semantic plane packets with the custom rANS/LZ codec.
// A one-byte-per-event order stream (itself extremely compressible) lets us
// reconstruct the exact interleaving. Anything that doesn't parse — truncated
// events, unknown commands, non-.slp input — falls back to a custom frame of the
// remaining bytes, so output is byte-identical for arbitrary input.
//
// Hot paths (transpose, delta, scans) are NEON-vectorized on Apple Silicon and
// use scalar fallbacks on other architectures; `slpz selftest` checks both.
//
// Build: make (see Makefile). Tested on macOS/Apple Silicon and Linux/x86-64.

#if defined(__aarch64__)
#include <arm_neon.h>
#define SLPZ_NEON 1
#else
#define SLPZ_NEON 0
#endif
#include <fcntl.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <mach/mach.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <CommonCrypto/CommonDigest.h>
#pragma clang diagnostic pop
#else
#include <malloc.h>
#include <openssl/sha.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "custom_codec.h"

namespace fs = std::filesystem;

// ── container constants ─────────────────────────────────────────────────────
constexpr uint32_t kMagic = 0x5A504C53;  // "SLPZ" little-endian
constexpr uint8_t kVersion = 1;
constexpr uint8_t kOriginalVersion = 15;  // identical payload before renumbering
constexpr uint8_t kCustom = 6;  // tagged custom frame codec ID
constexpr uint8_t kModeRaw = 0;    // whole file = one custom frame
constexpr uint8_t kModeSplit = 1;  // parsed event streams

constexpr size_t kSlpHeaderLen = 15;  // "{U\x03raw[$U#l" + u32 BE raw length
constexpr char kSlpMagic[] = "{U\x03raw[$U#l";

enum : uint8_t { T_PLAIN = 0, T_SUB = 1, T_XOR = 2 };

// Decode destinations are filled completely before observation. Avoid a
// redundant zero pass while retaining normal vector lifetime and ownership.
template <class T>
struct OutputAllocator : std::allocator<T> {
  using value_type = T;
  OutputAllocator() = default;
  template <class U>
  OutputAllocator(const OutputAllocator<U>&) noexcept {}
  template <class U>
  void construct(U* p) {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... Args>
    requires(sizeof...(Args) > 0)
  void construct(U* p, Args&&... args) {
    std::construct_at(p, std::forward<Args>(args)...);
  }
  template <class U>
  bool operator==(const OutputAllocator<U>&) const noexcept {
    return true;
  }
};
using OutputBytes = std::vector<uint8_t, OutputAllocator<uint8_t>>;

// ── little helpers ──────────────────────────────────────────────────────────
struct Buf : std::vector<uint8_t> {
  void put_u8(uint8_t v) { push_back(v); }
  void put_u32(uint32_t v) { insert(end(), (uint8_t*)&v, (uint8_t*)&v + 4); }
  void put_u64(uint64_t v) { insert(end(), (uint8_t*)&v, (uint8_t*)&v + 8); }
  void put_bytes(const void* p, size_t n) {
    insert(end(), (const uint8_t*)p, (const uint8_t*)p + n);
  }
};

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool fail = false;
  uint8_t u8() { return avail(1) ? *p++ : (fail = true, 0); }
  uint32_t u32() {
    if (!avail(4)) { fail = true; return 0; }
    uint32_t v; memcpy(&v, p, 4); p += 4; return v;
  }
  uint64_t u64() {
    if (!avail(8)) { fail = true; return 0; }
    uint64_t v; memcpy(&v, p, 8); p += 8; return v;
  }
  std::span<const uint8_t> bytes(size_t n) {
    if (!avail(n)) { fail = true; return {}; }
    auto s = std::span<const uint8_t>(p, n); p += n; return s;
  }
  bool avail(size_t n) const { return !fail && (size_t)(end - p) >= n; }
};

static uint32_t be32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t* p) { return (uint16_t)p[0] << 8 | p[1]; }

[[noreturn]] static void die(const std::string& msg) {
  fprintf(stderr, "slpz: %s\n", msg.c_str());
  exit(1);
}

#ifdef SLPZ_FUZZ
// In-memory filesystem for the fuzz round-trip harness: read_file/write_file
// hit this map instead of disk, so the round-trip does zero I/O — no disk
// bandwidth, no syscalls, and no temp files or dirs to clean up. Thread-safe
// because cmd_compress/cmd_extract touch files from worker/writer threads.
#include <map>
static std::map<std::string, std::vector<uint8_t>> g_memfs;
static std::mutex g_memfs_mu;
static bool memfs_get(const fs::path& p, std::vector<uint8_t>& out) {
  std::lock_guard<std::mutex> lk(g_memfs_mu);
  auto it = g_memfs.find(p.generic_string());
  if (it == g_memfs.end()) return false;
  out = it->second;
  return true;
}
static void memfs_put(const fs::path& p, std::span<const uint8_t> d) {
  std::lock_guard<std::mutex> lk(g_memfs_mu);
  g_memfs[p.generic_string()].assign(d.begin(), d.end());
}
static void memfs_clear() {
  std::lock_guard<std::mutex> lk(g_memfs_mu);
  g_memfs.clear();
}
#endif

static std::vector<uint8_t> read_file(const fs::path& path) {
#ifdef SLPZ_FUZZ
  std::vector<uint8_t> mv;
  if (memfs_get(path, mv)) return mv;
  die("cannot open " + path.string());
#endif
  std::ifstream f(path, std::ios::binary);
  if (!f) die("cannot open " + path.string());
  f.seekg(0, std::ios::end);
  std::vector<uint8_t> data((size_t)f.tellg());
  f.seekg(0);
  f.read((char*)data.data(), (std::streamsize)data.size());
  if (!f) die("short read on " + path.string());
  return data;
}

static void write_file(const fs::path& path, std::span<const uint8_t> data) {
#ifdef SLPZ_FUZZ
  memfs_put(path, data);
  return;
#endif
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) die("cannot create " + path.string());
  size_t at = 0;
  while (at < data.size()) {
    // macOS write() rejects requests > INT_MAX with EINVAL
    size_t want = std::min(data.size() - at, (size_t)1 << 30);
    ssize_t n = ::write(fd, data.data() + at, want);
    if (n <= 0) { ::close(fd); die("short write on " + path.string()); }
    at += (size_t)n;
  }
  ::close(fd);
}

// Custom frames carry their semantic profile in the payload.
// Levels 6-8 retain the eight-candidate size option.
static unsigned custom_depth(int level) {
  return level >= 9 ? 24 : level >= 6 ? 8 : level >= 3 ? 4 : 2;
}
static size_t custom_frame(std::span<const uint8_t> in, Buf& out,
                           slpz_custom::Profile profile = slpz_custom::Generic, int level = 3) {
  try {
    auto packed = slpz_custom::encode(in, profile, custom_depth(level));
    out.put_u32((uint32_t)packed.size() + 1);
    out.put_u32((uint32_t)in.size());
    out.put_u8(profile);
    out.put_bytes(packed.data(), packed.size());
    return packed.size() + 1;
  } catch (const slpz_custom::Error& e) {
    die(e.what());
  }
}
static void custom_planes_frame(std::span<const uint8_t> in, uint32_t count,
                                std::span<const uint8_t> profiles, int level, Buf& out) {
  try {
    auto packed = slpz_custom::encode_planes(in, count, profiles,
                                             custom_depth(level));
    out.put_u32((uint32_t)packed.size());
    out.put_u32((uint32_t)in.size());
    out.put_bytes(packed.data(), packed.size());
  } catch (const slpz_custom::Error& e) {
    die(e.what());
  }
}
static std::vector<uint8_t> custom_unframe(Reader& r, bool planes = false) {
  uint32_t comp = r.u32(), orig = r.u32();
  auto packed = r.bytes(comp);
  if (r.fail) die("corrupt custom frame");
  try {
    if (planes) return slpz_custom::decode_planes(packed, orig);
    if (packed.empty() || packed[0] >= slpz_custom::ProfileCount) die("corrupt custom profile");
    return slpz_custom::decode(packed.subspan(1), orig, slpz_custom::Profile(packed[0]));
  } catch (const slpz_custom::Error& e) {
    die(e.what());
  }
}
static uint8_t field_profile(uint8_t cmd, uint32_t offset, bool dictionary) {
  using namespace slpz_custom;
  if (dictionary) return cmd == 0x37 && offset >= 24 ? Controller : Discrete;
  if ((cmd == 0x37 || cmd == 0x38 || cmd == 0x3A || cmd == 0x3C) && offset < 4) return Counter;
  if (cmd == 0x37) {
    if (offset >= 24 && offset < 59) return Controller;
    if (offset >= 63) return Controller;
    for (uint32_t start : {12u, 16u, 20u, 59u})
      if (offset >= start && offset < start + 4) return offset - start < 2 ? FloatHigh : FloatLow;
    if (offset >= 6 && offset < 10) return Rng;
  } else if (cmd == 0x38) {
    for (uint32_t start :
         {9u, 13u, 17u, 21u, 25u, 33u, 42u, 52u, 56u, 60u, 64u, 68u, 72u, 76u, 80u})
      if (offset >= start && offset < start + 4) return offset - start < 2 ? FloatHigh : FloatLow;
  } else if (cmd == 0x3A && offset >= 4 && offset < 8)
    return Rng;
  return Generic;
}

// ── stats (-v) ──────────────────────────────────────────────────────────────
struct Stats {
  std::atomic<uint64_t> byCmd[256]{};
  std::atomic<uint64_t> order{0}, suffix{0}, tail{0}, selector{0}, misc{0};
  std::atomic<uint64_t> seeds{0}, seedHit{0}, seedMiss{0};
  bool enabled = false;
};
static Stats g_stats;

// ── Melee RNG (MSVC LCG). Pre-frame events carry the live seed; consecutive
// events differ by a small number of engine RNG calls, so we store the step
// count instead of 32 raw bits. ──────────────────────────────────────────────
constexpr uint32_t kSeedCap = 4096;  // max steps to search
static inline uint32_t lcg_next(uint32_t s) { return s * 214013u + 2531011u; }

// seed field location per command: pre-frame (0x37) at offset 6,
// frame start (0x3A) at offset 4; 0 = command carries no seed
static uint32_t seed_offset_for(uint8_t cmd, uint32_t recSize) {
  if (cmd == 0x37 && recSize >= 10) return 6;
  if (cmd == 0x3A && recSize >= 8) return 4;
  return 0;
}

struct LcgTables {
  std::array<uint16_t, 8192> phase{};
  struct Jump { uint32_t multiply, add; };
  std::array<Jump, kSeedCap + 1> jumps{};
  LcgTables() {
    uint32_t seed = 0;
    for (unsigned i = 0; i < phase.size(); ++i) {
      phase[seed & 8191] = uint16_t(i);
      seed = lcg_next(seed);
    }
    jumps[0] = {1, 0};
    for (unsigned k = 1; k < jumps.size(); ++k)
      jumps[k] = {jumps[k - 1].multiply * 214013u, lcg_next(jumps[k - 1].add)};
  }
};
static const LcgTables& lcg_tables() {
  static const LcgTables tables;
  return tables;
}
static uint32_t lcg_advance(uint32_t seed, uint32_t steps) {
  if (steps > kSeedCap) die("corrupt archive (seed step count)");
  if (!steps) return seed;
  const auto& jump = lcg_tables().jumps[steps];
  return jump.multiply * seed + jump.add;
}
// Low 13 bits visit all 8192 states, so they identify at most one candidate
// in the 0..4096 search window. Verify the full state before accepting it.
static uint32_t lcg_distance(uint32_t a, uint32_t b) {
  if (a == b) return 0;
  const auto& table = lcg_tables();
  uint32_t k = (uint32_t(table.phase[b & 8191]) - table.phase[a & 8191]) & 8191;
  if (k > kSeedCap) return UINT32_MAX;
  const auto& jump = table.jumps[k];
  return jump.multiply * a + jump.add == b ? k : UINT32_MAX;
}

// Small side streams carry a codec tag before their custom frame.
static void small_compress(std::span<const uint8_t> in, int level, Buf& blob,
                           slpz_custom::Profile profile = slpz_custom::Generic) {
  blob.put_u8(kCustom);
  custom_frame(in, blob, profile, level);
}

static std::vector<uint8_t> small_decompress(Reader& r) {
  if (r.u8() != kCustom || r.fail) die("corrupt archive (custom codec tag)");
  return custom_unframe(r);
}

// ── NEON kernels ────────────────────────────────────────────────────────────
// 16x16 byte tile transpose: dst row k = column k of the 16 src rows.
template<unsigned Rows = 16, unsigned Cols = 16>
static inline void tile16(const uint8_t* src, size_t sStride,
                          uint8_t* dst, size_t dStride) {
#if SLPZ_NEON
  uint8x16_t x[16];
  for (unsigned k = 0; k < 16; ++k) {
    if (k >= Rows) x[k] = vdupq_n_u8(0);
    else if constexpr (Cols == 16) x[k] = vld1q_u8(src + size_t(k) * sStride);
    else {
      uint8_t row[16] = {};
      memcpy(row, src + size_t(k) * sStride, Cols);
      x[k] = vld1q_u8(row);
    }
  }
  for (int k = 0; k < 16; k += 2) {
    uint8x16_t lo = vtrn1q_u8(x[k], x[k + 1]), hi = vtrn2q_u8(x[k], x[k + 1]);
    x[k] = lo; x[k + 1] = hi;
  }
  for (int k = 0; k < 16; k++)
    if (!(k & 2)) {
      uint16x8_t a = vreinterpretq_u16_u8(x[k]), b = vreinterpretq_u16_u8(x[k | 2]);
      x[k] = vreinterpretq_u8_u16(vtrn1q_u16(a, b));
      x[k | 2] = vreinterpretq_u8_u16(vtrn2q_u16(a, b));
    }
  for (int k = 0; k < 16; k++)
    if (!(k & 4)) {
      uint32x4_t a = vreinterpretq_u32_u8(x[k]), b = vreinterpretq_u32_u8(x[k | 4]);
      x[k] = vreinterpretq_u8_u32(vtrn1q_u32(a, b));
      x[k | 4] = vreinterpretq_u8_u32(vtrn2q_u32(a, b));
    }
  for (int k = 0; k < 16; k++)
    if (!(k & 8)) {
      uint64x2_t a = vreinterpretq_u64_u8(x[k]), b = vreinterpretq_u64_u8(x[k | 8]);
      x[k] = vreinterpretq_u8_u64(vtrn1q_u64(a, b));
      x[k | 8] = vreinterpretq_u8_u64(vtrn2q_u64(a, b));
    }
  for (unsigned k = 0; k < Cols; ++k) {
    if constexpr (Rows == 16) vst1q_u8(dst + size_t(k) * dStride, x[k]);
    else {
      uint8_t row[16];
      vst1q_u8(row, x[k]);
      memcpy(dst + size_t(k) * dStride, row, Rows);
    }
  }
#else
  for (size_t i = 0; i < Rows; i++)
    for (size_t j = 0; j < Cols; j++)
      dst[j * dStride + i] = src[i * sStride + j];
#endif
}

// records (count × size) → planes (size × count)
static void transpose_fwd(std::span<const uint8_t> recs, uint32_t count,
                          uint32_t size, Buf& out) {
  out.resize(recs.size());
  const uint8_t* in = recs.data();
  uint32_t iTiles = count & ~15u, jTiles = size & ~15u;
  for (uint32_t j = 0; j < jTiles; j += 16)
    for (uint32_t i = 0; i < iTiles; i += 16)
      tile16(in + (size_t)i * size + j, size, out.data() + (size_t)j * count + i, count);
  // Keep incomplete record-width tiles in the vector path, without reading
  // or writing outside the short edge of the source/destination.
  auto tail = [&]<unsigned W>() {
    for (unsigned i = 0; i < iTiles; i += 16)
      tile16<16, W>(in + size_t(i) * size + jTiles, size,
                     out.data() + size_t(jTiles) * count + i, count);
  };
  switch (size - jTiles) {
    case 1: tail.operator()<1>(); break;
    case 2: tail.operator()<2>(); break;
    case 3: tail.operator()<3>(); break;
    case 4: tail.operator()<4>(); break;
    case 5: tail.operator()<5>(); break;
    case 6: tail.operator()<6>(); break;
    case 7: tail.operator()<7>(); break;
    case 8: tail.operator()<8>(); break;
    case 9: tail.operator()<9>(); break;
    case 10: tail.operator()<10>(); break;
    case 11: tail.operator()<11>(); break;
    case 12: tail.operator()<12>(); break;
    case 13: tail.operator()<13>(); break;
    case 14: tail.operator()<14>(); break;
    case 15: tail.operator()<15>(); break;
  }
  for (uint32_t j = jTiles; j < size; j++)
    for (uint32_t i = iTiles; i < count; i++)
      out[(size_t)j * count + i] = in[(size_t)i * size + j];
  for (uint32_t j = 0; j < jTiles; j++)
    for (uint32_t i = iTiles; i < count; i++)
      out[(size_t)j * count + i] = in[(size_t)i * size + j];
}

// planes (size × count) → records (count × size)
static void transpose_rev(std::span<const uint8_t> planes, uint32_t count,
                          uint32_t size, uint8_t* out) {
  const uint8_t* in = planes.data();
  uint32_t iTiles = count & ~15u, jTiles = size & ~15u;
  for (uint32_t i = 0; i < iTiles; i += 16)
    for (uint32_t j = 0; j < jTiles; j += 16)
      tile16(in + (size_t)j * count + i, count, out + (size_t)i * size + j, size);
  // Keep incomplete record-width tiles in the vector path, without reading
  // or writing outside the short edge of the source/destination.
  auto tail = [&]<unsigned W>() {
    for (unsigned i = 0; i < iTiles; i += 16)
      tile16<W, 16>(in + size_t(jTiles) * count + i, count,
                     out + size_t(i) * size + jTiles, size);
  };
  switch (size - jTiles) {
    case 1: tail.operator()<1>(); break;
    case 2: tail.operator()<2>(); break;
    case 3: tail.operator()<3>(); break;
    case 4: tail.operator()<4>(); break;
    case 5: tail.operator()<5>(); break;
    case 6: tail.operator()<6>(); break;
    case 7: tail.operator()<7>(); break;
    case 8: tail.operator()<8>(); break;
    case 9: tail.operator()<9>(); break;
    case 10: tail.operator()<10>(); break;
    case 11: tail.operator()<11>(); break;
    case 12: tail.operator()<12>(); break;
    case 13: tail.operator()<13>(); break;
    case 14: tail.operator()<14>(); break;
    case 15: tail.operator()<15>(); break;
  }
  for (uint32_t j = jTiles; j < size; j++)
    for (uint32_t i = iTiles; i < count; i++)
      out[(size_t)i * size + j] = in[(size_t)j * count + i];
  for (uint32_t j = 0; j < jTiles; j++)
    for (uint32_t i = iTiles; i < count; i++)
      out[(size_t)i * size + j] = in[(size_t)j * count + i];
}

// in-place p[i] -= p[i-1] (or ^=), processed back-to-front so reads see
// original values; vector blocks stay 16 behind the last write.
template <bool XOR>
static void delta_fwd(uint8_t* p, size_t n) {
  if (n < 2) return;
#if SLPZ_NEON
  size_t pos = n;
  while (pos >= 17) {
    pos -= 16;
    uint8x16_t v = vld1q_u8(p + pos), vp = vld1q_u8(p + pos - 1);
    vst1q_u8(p + pos, XOR ? veorq_u8(v, vp) : vsubq_u8(v, vp));
  }
  for (size_t i = pos; i-- > 1;)
    p[i] = XOR ? (uint8_t)(p[i] ^ p[i - 1]) : (uint8_t)(p[i] - p[i - 1]);
#else
  for (size_t i = n; i-- > 1;)
    p[i] = XOR ? (uint8_t)(p[i] ^ p[i - 1]) : (uint8_t)(p[i] - p[i - 1]);
#endif
}

// inclusive scan undoing delta_fwd (Hillis–Steele within a vector + carry)
template <bool XOR>
static void scan_rev(uint8_t* p, size_t n) {
#if SLPZ_NEON
  const uint8x16_t zero = vdupq_n_u8(0);
  uint8x16_t carry = zero;
  auto combine = [](uint8x16_t a, uint8x16_t b) {
    return XOR ? veorq_u8(a, b) : vaddq_u8(a, b);
  };
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    uint8x16_t v = vld1q_u8(p + i);
    v = combine(v, vextq_u8(zero, v, 15));
    v = combine(v, vextq_u8(zero, v, 14));
    v = combine(v, vextq_u8(zero, v, 12));
    v = combine(v, vextq_u8(zero, v, 8));
    v = combine(v, carry);
    carry = vdupq_laneq_u8(v, 15);
    vst1q_u8(p + i, v);
  }
  uint8_t c = vgetq_lane_u8(carry, 15);
  for (; i < n; i++) {
    c = XOR ? (uint8_t)(p[i] ^ c) : (uint8_t)(p[i] + c);
    p[i] = c;
  }
#else
  for (size_t i = 1; i < n; i++)
    p[i] = XOR ? (uint8_t)(p[i] ^ p[i - 1]) : (uint8_t)(p[i] + p[i - 1]);
#endif
}

// ── per-plane transform choice ──────────────────────────────────────────────
// Order-0 entropy over contiguous sample windows decides plain vs sub vs xor.
// Contiguous windows keep the delta candidates honest (true neighbours) and
// the memory access pattern cache-friendly.
static const float* xlog2x_lut() {
  constexpr uint32_t N = 1 << 15;
  static float* lut = [] {
    float* t = new float[N + 1];
    t[0] = 0.f;
    for (uint32_t c = 1; c <= N; c++) t[c] = (float)c * std::log2f((float)c);
    return t;
  }();
  return lut;
}

// Return the transform with the lowest estimated order-0 entropy.
static uint8_t choose_transform(const uint8_t* plane, uint32_t count) {
  // Constant planes have an explicit compact representation.
  if (slpz_custom::is_constant({plane, count})) return T_PLAIN;

  uint32_t hist[3][256] = {};
  uint32_t m = 0;
  constexpr uint32_t kWin = 256;
  uint32_t starts[3] = {0, count > 2 * kWin ? count / 2 : count,
                        count > 3 * kWin ? count - kWin : count};
  for (uint32_t s : starts) {
    if (s >= count) continue;
    uint32_t n = count - s < kWin ? count - s : kWin;
    const uint8_t* p = plane + s;
    uint8_t prev = 0;
    for (uint32_t k = 0; k < n; k++) {
      uint8_t b = p[k];
      hist[0][b]++;
      hist[1][(uint8_t)(b - prev)]++;
      hist[2][b ^ prev]++;
      prev = b;
    }
    m += n;
  }
  const float* lut = xlog2x_lut();
  float score[3];
  for (int t = 0; t < 3; t++) {
    float s = 0;
    for (uint32_t c = 0; c < 256; c++) s += lut[hist[t][c]];
    score[t] = s;  // maximize Σ c·log2 c  ⇔  minimize entropy
  }
  const float eps = 0.01f * (float)m;
  uint8_t t;
  if (score[0] + eps >= score[1] && score[0] + eps >= score[2]) t = T_PLAIN;
  else if (score[1] + eps / 2 >= score[2]) t = T_SUB;
  else t = T_XOR;
  return t;
}

// ── field dictionarization ──────────────────────────────────────────────────
// Melee analog fields are quantized floats: a 4-byte column often takes few
// distinct values. Byte-plane splitting scatters that structure over 4
// planes; replacing the field with a 1-byte dictionary index recovers it
// (and shrinks the entropy-stage input ~40%).
struct U32Set {
  static constexpr uint32_t N = 1024;  // ≤256 live entries → low load factor
  uint32_t keys[N];
  uint8_t idx[N];
  uint8_t used[N] = {};
  uint32_t n = 0, lastKey = 0;
  int lastId = -1;
  int find_or_insert(uint32_t k) {
    if (lastId >= 0 && lastKey == k) return lastId;
    lastKey = k;
    lastId = -1;
    uint32_t h = (k * 2654435761u) >> 22;
    for (;; h = (h + 1) & (N - 1)) {
      if (!used[h]) {
        if (n >= 256) return -1;
        used[h] = 1; keys[h] = k; idx[h] = (uint8_t)n++;
        return lastId = idx[h];
      }
      if (keys[h] == k) return lastId = idx[h];
    }
  }
};

struct DictField {
  uint16_t offset;
  uint8_t width;               // 2 or 4 bytes
  std::vector<uint32_t> vals;  // LE-packed field bytes; sorted by original byte order
  Buf idx;                     // index plane (count bytes)
};

static bool implicit_dictionary(const DictField& f) {
  return f.vals.size() == 1;
}

// Sort by the original big-endian byte sequence, then remap record indexes.
// Adjacent dictionary entries and temporal index differences become smaller.
static void sort_dictionaries(std::vector<DictField>& fields) {
  for (auto& f : fields) {
    if (f.vals.size() < 2) continue;
    std::array<uint16_t, 256> order;
    std::array<uint32_t, 256> keys;
    std::array<uint8_t, 256> remap;
    for (size_t i = 0; i < f.vals.size(); ++i) {
      order[i] = uint16_t(i);
      keys[i] = 0;
      for (unsigned b = 0; b < f.width; ++b)
        keys[i] = (keys[i] << 8) | uint8_t(f.vals[i] >> (8 * b));
    }
    if (std::is_sorted(keys.begin(), keys.begin() + f.vals.size())) continue;
    std::sort(order.begin(), order.begin() + f.vals.size(),
              [&](unsigned a, unsigned b) { return keys[a] < keys[b]; });
    auto old = f.vals;
    for (size_t i = 0; i < f.vals.size(); ++i) {
      f.vals[i] = old[order[i]];
      remap[order[i]] = uint8_t(i);
    }
    for (auto& id : f.idx) id = remap[id];
  }
}

// Values are shuffled per field. Modes 2/3 predict the previous byte in each
// plane; mode 4 predicts the previous whole big-endian field before shuffling.
static Buf dictionary_forward(const std::vector<DictField>& fields, uint8_t transform) {
  Buf result;
  size_t bytes = 0;
  for (const auto& f : fields) bytes += f.vals.size() * f.width;
  result.reserve(bytes);
  for (const auto& f : fields) {
    std::array<uint32_t, 256> deltas;
    if (transform == 4) {
      uint32_t previous = 0;
      for (size_t i = 0; i < f.vals.size(); ++i) {
        uint32_t value = 0;
        for (unsigned b = 0; b < f.width; ++b)
          value = (value << 8) | uint8_t(f.vals[i] >> (8 * b));
        deltas[i] = value - previous;
        previous = value;
      }
    }
    for (unsigned b = 0; b < f.width; ++b) {
      uint8_t previous = 0;
      for (size_t i = 0; i < f.vals.size(); ++i) {
        uint8_t value = uint8_t(f.vals[i] >> (8 * b));
        uint8_t encoded = transform == 2 ? value ^ previous
                          : transform == 3 ? uint8_t(value - previous)
                          : transform == 4 ? uint8_t(deltas[i] >> (8 * (f.width - 1 - b)))
                                           : value;
        result.put_u8(encoded);
        previous = value;
      }
    }
  }
  return result;
}

static void dictionary_reverse(std::span<const uint8_t> data, uint8_t transform,
                               std::vector<DictField>& fields) {
  size_t at = 0;
  for (auto& f : fields) {
    for (unsigned b = 0; b < f.width; ++b) {
      uint8_t previous = 0;
      for (auto& v : f.vals) {
        uint8_t value = data[at++];
        if (transform == 2) value ^= previous;
        else if (transform == 3) value += previous;
        reinterpret_cast<uint8_t*>(&v)[b] = value;
        previous = value;
      }
    }
    if (transform == 4) {
      uint32_t previous = 0;
      for (auto& v : f.vals) {
        uint32_t delta = 0;
        for (unsigned b = 0; b < f.width; ++b) delta = (delta << 8) | uint8_t(v >> (8 * b));
        uint32_t value = previous + delta;
        previous = value;
        v = 0;
        for (unsigned b = 0; b < f.width; ++b)
          v |= uint32_t(uint8_t(value >> (8 * (f.width - 1 - b)))) << (8 * b);
      }
    }
  }
}

// Transpose, dictionarize complete fields, and select a reversible byte
// transform per plane. Layout 2 stores the semantic custom plane packet.
static void pack_records(std::span<const uint8_t> recs, uint32_t count, uint32_t size,
                         int level, Buf& blob, std::atomic<uint64_t>* stat,
                         uint8_t cmd = 0) {
  size_t blobAt = blob.size();
  Buf planes;
  transpose_fwd(recs, count, size, planes);

  std::vector<DictField> fields;
  std::vector<uint8_t> widthAt(size, 0);  // field width at start offset
  if (count >= 64) {
    std::vector<uint8_t> covered(size, 0), constant(size, 0);
    std::vector<unsigned> alphabets(size, 0);
    constexpr unsigned limit = 128;
    for (unsigned o = 0; o < size; ++o)
      constant[o] = slpz_custom::is_constant({planes.data() + size_t(o) * count, count});
    Buf idxPlane;
    // Preserve complete semantic fields before considering sliding windows.
    std::vector<std::pair<unsigned, unsigned>> trials;
    trials.reserve(size_t(size) * 2 + 16);
    if (cmd == 0x37)
      for (unsigned o : {12u, 16u, 20u, 24u, 28u, 32u, 36u, 40u, 50u, 54u, 59u})
        if (o + 4 <= size) trials.emplace_back(4, o);
    if (cmd == 0x38)
      for (unsigned o : {9u, 13u, 17u, 21u, 25u, 33u, 42u, 52u, 56u, 60u, 64u, 68u, 72u, 76u, 80u})
        if (o + 4 <= size) trials.emplace_back(4, o);
    for (unsigned width : {4u, 2u})
      for (unsigned w = 0; w + width <= size; ++w) trials.emplace_back(width, w);
    for (auto [width, w] : trials) {
      bool free = true;
      for (uint32_t b = 0; b < width; b++) free = free && !covered[w + b];
      if (!free) continue;
      unsigned changing = 0, changingOffset = 0;
      for (unsigned b = 0; b < width; ++b)
        if (!constant[w + b]) {
          ++changing;
          changingOffset = w + b;
        }
      // A large alphabet in one varying byte gains little from a dictionary:
      // explicit constant planes already represent its fixed neighbours.
      if (changing == 1) {
        if (!alphabets[changingOffset]) {
          std::array<bool, 256> seen{};
          unsigned distinct = 0;
          for (unsigned i = 0; i < count; ++i) {
            uint8_t v = planes[size_t(changingOffset) * count + i];
            if (!seen[v]) {
              seen[v] = true;
              if (++distinct == limit) break;
            }
          }
          alphabets[changingOffset] = distinct;
        }
        if (alphabets[changingOffset] >= limit) continue;
      }
      U32Set set;
      idxPlane.clear();
      if (changing) idxPlane.resize(count);
      bool ok = true;
      if (changing == 0) {
        uint32_t v = 0;
        memcpy(&v, recs.data() + w, width);
        set.find_or_insert(v);  // every index is already zero
      } else if (changing == 1) {
        std::array<int16_t, 256> ids;
        ids.fill(-1);
        auto* plane = planes.data() + size_t(changingOffset) * count;
        for (uint32_t i = 0; i < count; ++i) {
          uint8_t b = plane[i];
          if (ids[b] < 0) {
            uint32_t v = 0;
            memcpy(&v, recs.data() + size_t(i) * size + w, width);
            ids[b] = set.find_or_insert(v);
          }
          idxPlane[i] = uint8_t(ids[b]);
        }
      } else {
        for (uint32_t i = 0; i < count; i++) {
          uint32_t v = 0;
          if (width == 4)
            memcpy(&v, recs.data() + size_t(i) * size + w, 4);
          else {
            uint16_t u;
            memcpy(&u, recs.data() + size_t(i) * size + w, 2);
            v = u;
          }
          int id = set.find_or_insert(v);
          if (id < 0) {
            ok = false;
            break;
          }
          idxPlane[i] = (uint8_t)id;
        }
      }
      // The field count ships as one byte, so 255 is a hard ceiling. Records
      // wide enough to exceed it exist (cmd 0x10 is 516 bytes = up to 258
      // two-byte fields), and whether they do depends on the pooled record
      // count, not the record itself: acceptance loosens as `count` grows, so
      // a corpus large enough to pool many 0x10 records tips over. The 4-byte
      // pass runs first, so capping here keeps the highest-value fields.
      if (ok && fields.size() < 255 && set.n * width <= (size_t)count * (width / 2)) {
        DictField f{(uint16_t)w, uint8_t(width), {}, {}};
        f.vals.resize(set.n);
        for (uint32_t h = 0; h < U32Set::N; h++)
          if (set.used[h]) f.vals[set.idx[h]] = set.keys[h];
        f.idx = idxPlane;
        fields.push_back(std::move(f));
        widthAt[w] = width;
        for (uint32_t b = 0; b < width; b++) covered[w + b] = 1;
      }
    }
    std::sort(fields.begin(), fields.end(),
              [](const DictField& a, const DictField& b) { return a.offset < b.offset; });
  }

  sort_dictionaries(fields);

  uint32_t nOut = size;
  for (auto& f : fields) nOut -= f.width - 1 + implicit_dictionary(f);
  Buf out;
  out.reserve((size_t)nOut * count);
  {
    size_t fi = 0;
    for (uint32_t o = 0; o < size;) {
      if (widthAt[o]) {
        if (!implicit_dictionary(fields[fi])) out.put_bytes(fields[fi].idx.data(), count);
        o += fields[fi].width;
        fi++;
      } else {
        out.put_bytes(planes.data() + (size_t)o * count, count);
        o += 1;
      }
    }
  }

  if (fields.size() > 255) die("internal: dict field count overflow");
  // Dictionary descriptors remain directly scannable; only value bytes are
  // compressed. The first width byte marks the new representation.
  Buf dictionary;
  uint8_t dictionaryTransform = 0;
  size_t dictionaryBytes = 0;
  for (const auto& f : fields) dictionaryBytes += f.vals.size() * f.width;
  if (dictionaryBytes >= 128) {
    Buf candidates[4];
    unsigned best = 0;
    double cost = std::numeric_limits<double>::infinity();
    for (unsigned t = 0; t < 4; ++t) {
      candidates[t] = dictionary_forward(fields, uint8_t(t + 1));
      std::array<uint32_t, 256> hist{};
      for (auto b : candidates[t]) ++hist[b];
      double c = slpz_custom::context_cost(hist);
      if (c < cost) { cost = c; best = t; }
    }
    small_compress(candidates[best], std::min(level, 3), dictionary, slpz_custom::Discrete);
    if (dictionary.size() < dictionaryBytes) dictionaryTransform = uint8_t(best + 1);
  }
  blob.put_u8(uint8_t(fields.size()));
  for (size_t j = 0; j < fields.size(); ++j) {
    const auto& f = fields[j];
    uint8_t tag = j == 0 && dictionaryTransform ? uint8_t(128 | (dictionaryTransform << 4)) : 0;
    blob.put_u8(f.width | tag);
    blob.put_u8(uint8_t(f.offset));
    blob.put_u8(uint8_t(f.offset >> 8));
    blob.put_u8(uint8_t(f.vals.size() - 1));
    if (!dictionaryTransform)
      for (uint32_t v : f.vals) blob.put_bytes(&v, f.width);
  }
  if (dictionaryTransform) blob.put_bytes(dictionary.data(), dictionary.size());
  std::vector<uint8_t> profiles;
  for (uint32_t offset = 0; offset < size;) {
    bool constant = false;
    if (widthAt[offset])
      for (auto& f : fields)
        if (f.offset == offset) {
          constant = implicit_dictionary(f);
          break;
        }
    if (!constant) profiles.push_back(field_profile(cmd, offset, widthAt[offset] != 0));
    offset += widthAt[offset] ? widthAt[offset] : 1;
  }
  for (uint32_t j = 0; j < nOut; j++) {
    uint8_t* pl = out.data() + (size_t)j * count;
    uint8_t t = count >= 8 ? choose_transform(pl, count) : T_PLAIN;
    if (t == T_SUB) delta_fwd<false>(pl, count);
    else if (t == T_XOR) delta_fwd<true>(pl, count);
    blob.put_u8(t);
  }
  blob.put_u8(2);
  custom_planes_frame(out, count, profiles, level, blob);
  if (g_stats.enabled && stat) *stat += blob.size() - blobAt;
}

// Validate the index plane once, then expand at a fixed field width. Small
// alphabets fit in NEON lookup tables; large ones use direct scalar gathers.
template<unsigned W>
static void expand_dictionary(const DictField& f, const uint8_t* indexes,
                              uint32_t count, uint8_t* output) {
  if (implicit_dictionary(f)) {
    for (unsigned b = 0; b < W; ++b)
      memset(output + size_t(b) * count, uint8_t(f.vals[0] >> (8 * b)), count);
    return;
  }
  unsigned maximum = 0, i = 0;
#if SLPZ_NEON && !defined(SLPZ_CUSTOM_SCALAR)
  auto maxes = vdupq_n_u8(0);
  for (; i + 16 <= count; i += 16) maxes = vmaxq_u8(maxes, vld1q_u8(indexes + i));
  maximum = vmaxvq_u8(maxes);
#endif
  for (; i < count; ++i) maximum = std::max(maximum, unsigned(indexes[i]));
  if (maximum >= f.vals.size()) die("corrupt archive (dict index)");

  i = 0;
#if SLPZ_NEON && !defined(SLPZ_CUSTOM_SCALAR)
  if (f.vals.size() <= 64) {
    uint8_t tables[W][64] = {};
    for (unsigned v = 0; v < f.vals.size(); ++v)
      for (unsigned b = 0; b < W; ++b) tables[b][v] = uint8_t(f.vals[v] >> (8 * b));
    if (f.vals.size() <= 16) {
      uint8x16_t table[W];
      for (unsigned b = 0; b < W; ++b) table[b] = vld1q_u8(tables[b]);
      for (; i + 16 <= count; i += 16) {
        auto ids = vld1q_u8(indexes + i);
        for (unsigned b = 0; b < W; ++b)
          vst1q_u8(output + size_t(b) * count + i, vqtbl1q_u8(table[b], ids));
      }
    } else {
      uint8x16x4_t table[W];
      for (unsigned b = 0; b < W; ++b) table[b] = vld1q_u8_x4(tables[b]);
      for (; i + 16 <= count; i += 16) {
        auto ids = vld1q_u8(indexes + i);
        for (unsigned b = 0; b < W; ++b)
          vst1q_u8(output + size_t(b) * count + i, vqtbl4q_u8(table[b], ids));
      }
    }
  }
  // Gather full values, then deinterleave with NEON rather than storing
  // every expanded byte separately for palettes larger than 64 entries.
  if constexpr (std::endian::native == std::endian::little) {
    for (; i + 16 <= count; i += 16) {
      if constexpr (W == 4) {
        uint32_t values[16];
        for (unsigned k = 0; k < 16; ++k) values[k] = f.vals[indexes[i + k]];
        auto columns = vld4q_u8(reinterpret_cast<const uint8_t*>(values));
        for (unsigned b = 0; b < 4; ++b) vst1q_u8(output + size_t(b) * count + i, columns.val[b]);
      } else {
        uint16_t values[16];
        for (unsigned k = 0; k < 16; ++k) values[k] = uint16_t(f.vals[indexes[i + k]]);
        auto columns = vld2q_u8(reinterpret_cast<const uint8_t*>(values));
        for (unsigned b = 0; b < 2; ++b) vst1q_u8(output + size_t(b) * count + i, columns.val[b]);
      }
    }
  }
#endif
  for (; i < count; ++i) {
    uint32_t value = f.vals[indexes[i]];
    for (unsigned b = 0; b < W; ++b)
      output[size_t(b) * count + i] = uint8_t(value >> (8 * b));
  }
}

template <class RecordBuffer>
static void unpack_records(Reader& r, uint32_t count, uint32_t size,
                           RecordBuffer& records) {
  uint8_t nFields = r.u8();
  std::vector<DictField> fields(nFields);
  uint32_t nOut = size;
  uint8_t dictionaryTransform = 0;
  uint32_t dictionaryBytes = 0;
  std::vector<bool> covered(size, false);
  for (size_t j = 0; j < fields.size(); ++j) {
    auto& f = fields[j];
    f.width = r.u8();
    if (j == 0 && (f.width & 128)) {
      dictionaryTransform = (f.width >> 4) & 7;
      if (!dictionaryTransform || dictionaryTransform > 4) die("corrupt dictionary transform");
      f.width &= 15;
    }
    f.offset = r.u8();
    f.offset |= uint16_t(r.u8()) << 8;
    uint32_t nVals = uint32_t(r.u8()) + 1;
    if ((f.width != 2 && f.width != 4) || f.offset + f.width > size || r.fail)
      die("corrupt archive (dict field)");
    for (unsigned b = 0; b < f.width; ++b) {
      if (covered[f.offset + b]) die("corrupt archive (dict overlap)");
      covered[f.offset + b] = true;
    }
    f.vals.resize(nVals);
    dictionaryBytes += nVals * f.width;
    if (!dictionaryTransform) {
      auto raw = r.bytes(nVals * f.width);
      if (r.fail) die("corrupt archive (dict field)");
      for (uint32_t v = 0; v < nVals; ++v)
        memcpy(&f.vals[v], raw.data() + size_t(v) * f.width, f.width);
    }
    if (nOut < f.width) die("corrupt archive (dict count)");
    nOut -= f.width - 1 + implicit_dictionary(f);
  }
  if (dictionaryTransform) {
    Reader check = r;
    check.u8();  // codec
    check.u32();  // packed size
    if (check.u32() != dictionaryBytes || check.fail) die("corrupt dictionary frame size");
    auto data = small_decompress(r);
    if (data.size() != dictionaryBytes) die("corrupt dictionary output size");
    dictionary_reverse(data, dictionaryTransform, fields);
  }
  auto sel = r.bytes(nOut);
  uint8_t layout = r.u8();
  if (r.fail) die("corrupt archive (plane selectors)");

  if (layout != 2) die("corrupt archive (layout id)");
  auto out = custom_unframe(r, true);
  if (out.size() != (size_t)count * nOut) die("corrupt archive (stream size)");

  for (uint32_t j = 0; j < nOut; j++) {
    uint8_t* pl = out.data() + (size_t)j * count;
    uint8_t t = sel[j] & 3;
    if (t == T_SUB) scan_rev<false>(pl, count);
    else if (t == T_XOR) scan_rev<true>(pl, count);
  }

  // Expand and transpose one cache-sized block, avoiding a full-sized plane
  // allocation and its extra memory traffic.
  std::vector<const DictField*> fieldAt(size, nullptr);
  for (auto& f : fields) fieldAt[f.offset] = &f;
  static const unsigned block = 512;
  OutputBytes planes(size_t(std::min(block, count)) * size);
  records.resize(size_t(count) * size);
  for (unsigned at = 0; at < count; at += block) {
    unsigned take = std::min(block, count - at);
    size_t srcAt = 0;
    for (unsigned o = 0; o < size;) {
      bool constant = fieldAt[o] && implicit_dictionary(*fieldAt[o]);
      if (!constant && srcAt + count > out.size()) die("corrupt archive (plane underrun)");
      const uint8_t* pl = constant ? nullptr : out.data() + srcAt + at;
      auto* dest = planes.data() + size_t(o) * take;
      if (fieldAt[o]) {
        const auto& f = *fieldAt[o];
        if (f.width == 4)
          expand_dictionary<4>(f, pl, take, dest);
        else
          expand_dictionary<2>(f, pl, take, dest);
        o += f.width;
      } else {
        memcpy(dest, pl, take);
        ++o;
      }
      if (!constant) srcAt += count;
    }
    transpose_rev({planes.data(), size_t(take) * size}, take, size,
                  records.data() + size_t(at) * size);
  }
}

// ── .slp event-stream model ─────────────────────────────────────────────────
struct EventStream {
  uint8_t cmd = 0;
  uint32_t recSize = 0;  // payload bytes per record (command byte excluded)
  Buf records;           // concatenated payloads
  uint32_t count = 0;
};

struct ParsedSlp {
  std::span<const uint8_t> prefix, suffix, tail;
  std::span<const uint8_t> payloadsEvent;  // 0x35 payload, verbatim
  Buf order;                               // command byte per event
  std::vector<EventStream> streams;
};

static bool parse_slp(std::span<const uint8_t> file, ParsedSlp& out) {
  if (file.size() < kSlpHeaderLen + 2) return false;
  if (memcmp(file.data(), kSlpMagic, 11) != 0) return false;
  uint32_t rawLen = be32(file.data() + 11);

  size_t rawStart = kSlpHeaderLen;
  size_t rawEnd;  // exclusive
  if (rawLen != 0 && rawStart + rawLen <= file.size()) {
    rawEnd = rawStart + rawLen;
  } else {
    rawEnd = file.size();  // in-progress replay: events run to EOF
  }
  out.prefix = file.subspan(0, rawStart);
  out.suffix = file.subspan(rawEnd);

  const uint8_t* p = file.data() + rawStart;
  const uint8_t* end = file.data() + rawEnd;
  if (p >= end || *p != 0x35) return false;
  if (p + 2 > end) return false;
  uint8_t epSize = p[1];  // includes the size byte itself
  if (epSize < 1 || (epSize - 1) % 3 != 0 || p + 1 + epSize > end) return false;
  out.payloadsEvent = std::span<const uint8_t>(p + 1, epSize);

  int32_t sizeOf[256];
  for (auto& s : sizeOf) s = -1;
  int16_t idxOf[256];
  for (auto& i : idxOf) i = -1;
  for (const uint8_t* t = p + 2; t < p + 1 + epSize; t += 3) {
    uint8_t cmd = t[0];
    int32_t sz = be16(t + 1);
    // corrupt files can redeclare a command with a different size; the stream
    // would keep the first recSize while records append at the last → OOB
    if (sizeOf[cmd] >= 0 && sizeOf[cmd] != sz) return false;
    sizeOf[cmd] = sz;
    if (idxOf[cmd] < 0) {
      idxOf[cmd] = (int16_t)out.streams.size();
      out.streams.push_back({cmd, (uint32_t)sz, {}, 0});
    }
  }
  p += 1 + epSize;

  {
    uint32_t events = 0;
    for (const uint8_t* q = p; q < end;) {
      uint8_t cmd = *q;
      int32_t sz = sizeOf[cmd];
      if (sz < 0 || size_t(end - q) <= size_t(sz)) break;
      ++out.streams[size_t(idxOf[cmd])].count;
      ++events;
      q += 1 + sz;
    }
    out.order.reserve(events);
    for (auto& st : out.streams) {
      st.records.reserve(size_t(st.count) * st.recSize);
      st.count = 0;
    }
  }
  while (p < end) {
    uint8_t cmd = *p;
    int32_t sz = sizeOf[cmd];
    if (sz < 0 || p + 1 + sz > end) break;  // rest becomes tail
    auto& st = out.streams[(size_t)idxOf[cmd]];
    st.records.put_bytes(p + 1, (size_t)sz);
    st.count++;
    out.order.put_u8(cmd);
    p += 1 + sz;
  }
  out.tail = std::span<const uint8_t>(p, (size_t)(end - p));
  // The original declaration table is already preserved in payloadsEvent.
  // An unused declaration needs no stream packet, selectors, or plane models.
  // Keep zero-width streams when actual command-only events were observed.
  std::erase_if(out.streams, [](const EventStream& st) { return st.count == 0; });
  return true;
}

// Pre/post-frame events (0x37/0x38) carry (playerIndex, isFollower) at
// payload offsets 4-5; item updates (0x3B) carry the item typeID u16 there.
// Splitting on those two bytes makes plane deltas coherent per entity.
static bool splittable(uint8_t cmd, uint32_t recSize) {
  return (cmd == 0x37 || cmd == 0x38 || cmd == 0x3B) && recSize >= 6;
}

// ── seed folding (0x37) ─────────────────────────────────────────────────────
static void put_varint(Buf& b, uint32_t v) {
  while (v >= 0x80) { b.put_u8((uint8_t)(v | 0x80)); v >>= 7; }
  b.put_u8((uint8_t)v);
}

static uint32_t get_varint(const std::vector<uint8_t>& b, size_t& pos, bool& fail) {
  uint32_t v = 0;
  for (int sh = 0; sh < 35; sh += 7) {
    if (pos >= b.size()) { fail = true; return 0; }
    uint8_t c = b[pos++];
    v |= (uint32_t)(c & 0x7F) << sh;
    if (!(c & 0x80)) return v;
  }
  fail = true;
  return 0;
}

constexpr uint32_t kSeedException = kSeedCap + 1;

// Replace each record's seed with LCG-steps-from-previous; zero the seed
// bytes in place. Returns false (leaving records untouched) when the file's
// seeds don't follow the Melee LCG.
// Distances are computed in global event order (the RNG is global), but the
// varint stream is EMITTED grouped by entity: per-player step counts are far
// more periodic than the interleaved sequence (measured -21%).
static bool fold_seeds(EventStream& st, uint32_t seedOff, Buf& seedStream,
                       const uint8_t* groupSel) {
  if (st.count < 2) return false;
  auto seed_at = [&](uint32_t i) {
    return be32(st.records.data() + (size_t)i * st.recSize + seedOff);
  };
  uint32_t probes = st.count - 1 < 32 ? st.count - 1 : 32, hits = 0;
  for (uint32_t i = 0; i < probes; i++)
    if (lcg_distance(seed_at(i), seed_at(i + 1)) != UINT32_MAX) hits++;
  if (hits * 4 < probes * 3) return false;

  std::vector<uint32_t> ks(st.count), raw(st.count);
  uint32_t prev = 0;
  uint8_t maxG = 0;
  for (uint32_t i = 0; i < st.count; i++) {
    uint8_t* rec = st.records.data() + (size_t)i * st.recSize;
    uint32_t seed = be32(rec + seedOff);
    ks[i] = i == 0 ? UINT32_MAX : lcg_distance(prev, seed);
    raw[i] = seed;
    if (g_stats.enabled) (ks[i] == UINT32_MAX ? g_stats.seedMiss : g_stats.seedHit)++;
    memset(rec + seedOff, 0, 4);
    prev = seed;
    if (groupSel && groupSel[i] > maxG) maxG = groupSel[i];
  }
  for (uint32_t g = 0; g <= (groupSel ? (uint32_t)maxG : 0u); g++) {
    for (uint32_t i = 0; i < st.count; i++) {
      if (groupSel && groupSel[i] != g) continue;
      if (ks[i] == UINT32_MAX) {
        put_varint(seedStream, kSeedException);
        seedStream.put_u32(raw[i]);  // LE raw
      } else {
        put_varint(seedStream, ks[i]);
      }
    }
  }
  return true;
}

// ── cross-stream prediction: post-frame(N) ≈ pre-frame(N+1) ─────────────────
// Melee carries end-of-frame state into the next frame's pre-frame event for
// the same entity: actionState/x/y/facing (pre 10..24 ↔ post 7..21) and
// percent (pre 59..63 ↔ post 21..25) are byte-identical ~99% of the time.
// XORing each pre-frame record against the entity's previous post-frame
// record zeroes those 18 bytes; measured -59% on the 0x37 stream.
constexpr uint32_t kPreA = 10, kPostA = 7, kLenA = 14;
constexpr uint32_t kPreB = 59, kPostB = 21, kLenB = 4;

static bool cross_pred_eligible(uint32_t preSize, uint32_t preCount,
                                uint32_t postSize, uint32_t postCount) {
  return preSize >= kPreB + kLenB && postSize >= kPostB + kLenB &&
         preCount >= 2 && postCount >= 1;
}

// Pairs the i-th pre-frame of an entity with its (i-1)-th post-frame and
// XORs the predicted spans. Self-inverse: the decoder calls it again on the
// reconstructed post-frame stream. When probe=true, only measures the
// span-A hit rate on a prefix and mutates nothing.
template <class Pre, class Post>
static uint32_t cross_pred_records(Pre pre, uint32_t preCount, Post post, uint32_t postCount,
                                   bool probe, uint32_t probeMax, uint32_t* pairsOut) {
  static thread_local std::array<uint32_t, 65536> keyIdx{};
  static thread_local std::array<bool, 65536> occ{};
  std::vector<uint16_t> keys;
  std::unique_ptr<uint32_t[]> next(new uint32_t[postCount]);
  for (uint32_t i = postCount; i-- > 0;) {
    const uint8_t* q = post(i);
    uint16_t k = uint16_t(q[4] << 8 | q[5]);
    if (!keyIdx[k]) {
      occ[k] = false;
      keys.push_back(k);
    }
    next[i] = keyIdx[k];
    keyIdx[k] = i + 1;
  }
  uint32_t pairs = 0, hits = 0;
  uint32_t limit = probe ? std::min(preCount, probeMax) : preCount;
  for (uint32_t i = 0; i < limit; i++) {
    uint8_t* p = pre(i);
    uint16_t k = (uint16_t)(p[4] << 8 | p[5]);
    if (!keyIdx[k]) continue;
    if (!occ[k]) {
      occ[k] = true;
      continue;
    }
    uint32_t at = keyIdx[k] - 1;
    const uint8_t* q = post(at);
    keyIdx[k] = next[at];
    if (probe) {
      pairs++;
      hits += memcmp(p + kPreA, q + kPostA, kLenA) == 0;
      continue;
    }
    for (uint32_t b = 0; b < kLenA; b++) p[kPreA + b] ^= q[kPostA + b];
    for (uint32_t b = 0; b < kLenB; b++) p[kPreB + b] ^= q[kPostB + b];
  }
  for (auto k : keys) keyIdx[k] = 0;
  if (pairsOut) *pairsOut = pairs;
  return hits;
}

static uint32_t cross_pred_xor(uint8_t* pre, uint32_t preCount, uint32_t preSize,
                               const uint8_t* post, uint32_t postCount, uint32_t postSize,
                               bool probe, uint32_t probeMax, uint32_t* pairsOut) {
  return cross_pred_records([&](uint32_t i) { return pre + size_t(i) * preSize; }, preCount,
                            [&](uint32_t i) { return post + size_t(i) * postSize; }, postCount,
                            probe, probeMax, pairsOut);
}

// ── PPC branch-target absolutization ────────────────────────────────────────
// Rewrites relative branch displacements (b/bl opcode 18, bc opcode 16) to
// instruction-index-based targets so repeated calls to the same function
// become byte-identical LZ matches (kkrunchy-style, adapted to PowerPC).
// Modular arithmetic on the displacement field makes it exactly invertible.
static void ppc_branch_xform(uint8_t* p, size_t nWords, bool inverse) {
  for (size_t i = 0; i < nWords; i++) {
    uint8_t* w = p + i * 4;
    uint32_t v = be32(w);
    uint32_t op = v >> 26;
    if (op == 18) {
      if (v & 2) continue;  // AA=1: already absolute
      uint32_t li = (v >> 2) & 0xFFFFFF;
      uint32_t t = (inverse ? li - (uint32_t)i : li + (uint32_t)i) & 0xFFFFFF;
      v = (v & 0xFC000003u) | (t << 2);
    } else if (op == 16) {
      if (v & 2) continue;
      uint32_t bd = (v >> 2) & 0x3FFF;
      uint32_t t = (inverse ? bd - (uint32_t)i : bd + (uint32_t)i) & 0x3FFF;
      v = (v & 0xFFFF0003u) | (t << 2);
    } else {
      continue;
    }
    w[0] = (uint8_t)(v >> 24);
    w[1] = (uint8_t)(v >> 16);
    w[2] = (uint8_t)(v >> 8);
    w[3] = (uint8_t)v;
  }
}

// ── per-file compress / decompress ──────────────────────────────────────────
// Compare flat custom coding with a reversible PPC branch transform for
// opaque attachments such as appended Gecko codesets.
static void pack_blob(std::span<const uint8_t> data, int level,
                      Buf& blob, std::atomic<uint64_t>& stat) {
  size_t before = blob.size();
  Buf flat, ppc;
  custom_frame(data, flat, slpz_custom::Generic, level);
  if (data.size() >= 65536) {
    std::vector<uint8_t> xf(data.begin(), data.end());
    ppc_branch_xform(xf.data(), xf.size() / 4, false);
    custom_frame(xf, ppc, slpz_custom::Generic, level);
  }
  bool usePpc = !ppc.empty() && ppc.size() < flat.size();
  blob.put_u8(usePpc ? 4 : 2);
  blob.put_u8(kCustom);
  const auto& winner = usePpc ? ppc : flat;
  blob.put_bytes(winner.data(), winner.size());
  if (g_stats.enabled) stat += blob.size() - before;
}

// Modes 0, 2 and 4 are single custom frames; mode 3 is chunked below.
static std::vector<uint8_t> unpack_blob(Reader& r) {
  uint8_t mode = r.u8();
  if (r.fail) die("corrupt archive (blob header)");
  if (mode == 0) return custom_unframe(r);
  if (mode != 2 && mode != 4) die("corrupt archive (blob mode)");
  auto out = small_decompress(r);
  if (mode == 4) ppc_branch_xform(out.data(), out.size() / 4, true);
  return out;
}

// ── generic worker pool ─────────────────────────────────────────────────────
template <typename Fn>
static void parallel_for(size_t n, int threads, Fn work) {
  if (!n) return;
  threads = int(std::min<size_t>(n, threads));
  std::atomic<size_t> next{0};
  auto worker = [&] {
    for (size_t i; (i = next.fetch_add(1)) < n;) work(i);
  };
  std::vector<std::thread> pool;
  for (int t = 0; t < threads - 1; t++) pool.emplace_back(worker);
  worker();
  for (auto& t : pool) t.join();
}

// ── compress: 3 phases ──────────────────────────────────────────────────────
// Phase 1 (parallel per file): parse, fold, group, write all structural bytes
// into an ordered piece list, deferring the entropy-heavy work as tasks.
// Phase 2 (parallel per TASK across files, largest first): pack groups/blobs.
// Phase 3 (serial): concatenate pieces per file, write the container.
// This removes the wall-clock pole of the largest file: work items are
// group-sized (~a few MB), and huge blobs are chunked (mode 3, 1 MB chunks).
struct FilePlan;

struct PackTask {
  enum Kind : uint8_t { Ungrouped, Group, BlobFrame, BlobTrial, RawFile };
  FilePlan* fp;
  uint32_t piece;
  Kind kind;
  uint32_t stream = 0, groupIdx = 0;
  std::span<const uint8_t> data;  // BlobFrame / BlobTrial / RawFile
  std::atomic<uint64_t>* stat = nullptr;
  size_t est = 0;
};

struct PooledStream {
  uint8_t cmd;
  uint32_t recSize;
  uint32_t count;
  Buf records;
};

struct FilePlan {
  fs::path path;
  std::string storedName;
  uint64_t origSize = 0;
  std::vector<uint8_t> data;
  ParsedSlp slp;
  std::vector<Buf> pieces;
  int curPiece = -1;
  std::vector<Buf> selectors;                  // per stream (grouped only)
  std::vector<std::vector<uint32_t>> gCounts;  // per stream
  std::vector<PackTask> tasks;
  std::vector<PooledStream> pooled;            // records moved out for pooling
  std::atomic<uint32_t> pending{0};            // unfinished tasks

  Buf& live() {
    if (curPiece < 0) {
      pieces.emplace_back();
      curPiece = (int)pieces.size() - 1;
    }
    return pieces[(size_t)curPiece];
  }
  uint32_t defer() {
    pieces.emplace_back();
    curPiece = -1;
    return (uint32_t)pieces.size() - 1;
  }
};

static void plan_blob(FilePlan& fp, std::span<const uint8_t> data,
                      int level, std::atomic<uint64_t>* stat) {
  // Small chunks keep large opaque attachments from becoming a
  // long final task after the replay streams have finished.
  constexpr size_t kChunk = 1u << 20;
  if (data.size() < 65536) {
    Buf& b = fp.live();
    size_t at = b.size();
    b.put_u8(0);
    custom_frame(data, b, slpz_custom::Generic, level);
    if (g_stats.enabled) *stat += b.size() - at;
  } else if (data.size() < kChunk) {
    fp.tasks.push_back({&fp, fp.defer(), PackTask::BlobTrial, 0, 0, data,
                        stat, data.size()});
  } else {
    // decide the PPC transform once on a 256KB sample
    uint8_t xform = 0;
    {
      auto sample = data.first(std::min<size_t>(data.size(), 256 << 10));
      Buf a, bx;
      custom_frame(sample, a);
      std::vector<uint8_t> xf(sample.begin(), sample.end());
      ppc_branch_xform(xf.data(), xf.size() / 4, false);
      custom_frame(xf, bx);
      xform = bx.size() < a.size() ? 1 : 0;
    }
    uint32_t n = (uint32_t)((data.size() + kChunk - 1) / kChunk);
    Buf& b = fp.live();
    b.put_u8(3);
    b.put_u8(kCustom);
    b.put_u8(xform);
    b.put_u32(n);
    if (g_stats.enabled) *stat += 7;
    for (uint32_t i = 0; i < n; i++) {
      size_t off = (size_t)i * kChunk;
      auto chunk = data.subspan(off, std::min(kChunk, data.size() - off));
      fp.tasks.push_back({&fp, fp.defer(), PackTask::BlobFrame, 0, xform, chunk,
                          stat, chunk.size()});
    }
  }
}

static void plan_compress(FilePlan& fp, int level) {
  fp.data = read_file(fp.path);
  fp.origSize = fp.data.size();
  if (!parse_slp(fp.data, fp.slp)) {
    fp.live().put_u8(kModeRaw);
    fp.tasks.push_back({&fp, fp.defer(), PackTask::RawFile, 0, 0,
                        std::span<const uint8_t>(fp.data), &g_stats.misc,
                        fp.data.size()});
    return;
  }
  {
    Buf& b = fp.live();
    b.put_u8(kModeSplit);
    b.put_bytes(fp.slp.prefix.data(), kSlpHeaderLen);
    b.put_u8((uint8_t)fp.slp.payloadsEvent.size());
    b.put_bytes(fp.slp.payloadsEvent.data(), fp.slp.payloadsEvent.size());
  }
  plan_blob(fp, fp.slp.suffix, level, &g_stats.suffix);
  plan_blob(fp, fp.slp.tail, level, &g_stats.tail);
  {
    Buf& b = fp.live();
    size_t before = b.size();
    small_compress(fp.slp.order, level, b, slpz_custom::Discrete);
    if (g_stats.enabled) {
      g_stats.order += b.size() - before;
      g_stats.misc += kSlpHeaderLen + 1 + fp.slp.payloadsEvent.size();
    }
  }
  fp.live().put_u8((uint8_t)fp.slp.streams.size());
  fp.selectors.resize(fp.slp.streams.size());
  fp.gCounts.resize(fp.slp.streams.size());
  EventStream *pre = nullptr, *post = nullptr;
  for (auto& st : fp.slp.streams) {
    if (st.cmd == 0x37) pre = &st;
    if (st.cmd == 0x38) post = &st;
  }
  bool xpred = false;
  if (pre && post &&
      cross_pred_eligible(pre->recSize, pre->count, post->recSize, post->count)) {
    uint32_t pairs = 0;
    uint32_t hits = cross_pred_xor(pre->records.data(), pre->count, pre->recSize,
                                   post->records.data(), post->count,
                                   post->recSize, /*probe=*/true, 128, &pairs);
    if (pairs >= 8 && hits * 2 >= pairs) {
      cross_pred_xor(pre->records.data(), pre->count, pre->recSize,
                     post->records.data(), post->count, post->recSize,
                     /*probe=*/false, 0, nullptr);
      xpred = true;
    }
  }
  static thread_local std::vector<int16_t> groupOf;
  for (uint32_t s = 0; s < fp.slp.streams.size(); s++) {
    auto& st = fp.slp.streams[s];
    auto* stat = &g_stats.byCmd[st.cmd];
    uint8_t flags = 0;
    if (xpred && st.cmd == 0x37) flags |= 4;
    Buf seedStream;
    uint32_t seedOff = seed_offset_for(st.cmd, st.recSize);
    // small side-event streams (game start, gecko lists, message splits...)
    // are pooled across all files: identical/near-identical content between
    // replays of the same setup becomes LZ matches in one shared buffer
    if (!splittable(st.cmd, st.recSize) && !seedOff && st.count > 0 &&
        st.records.size() < (128u << 10)) {
      Buf& b = fp.live();
      b.put_u8(st.cmd);
      b.put_u32(st.recSize);
      b.put_u32(st.count);
      b.put_u8(8);  // flags: pooled
      fp.pooled.push_back({st.cmd, st.recSize, st.count, std::move(st.records)});
      st.count = 0;
      continue;
    }
    // group records by the 2-byte entity key at payload offsets 4-5
    // (before seed folding: the seed stream is emitted group-ordered)
    std::vector<uint16_t> keys;
    auto& counts = fp.gCounts[s];
    bool grouped = false;
    if (splittable(st.cmd, st.recSize) && st.count > 0) {
      groupOf.assign(65536, -1);
      grouped = true;
      for (uint32_t i = 0; i < st.count; i++) {
        const uint8_t* rec = st.records.data() + (size_t)i * st.recSize;
        uint16_t key = (uint16_t)(rec[4] << 8 | rec[5]);
        if (groupOf[key] < 0) {
          if (keys.size() >= 255) { grouped = false; counts.clear(); break; }
          groupOf[key] = (int16_t)keys.size();
          keys.push_back(key);
          counts.push_back(0);
        }
        counts[(size_t)groupOf[key]]++;
      }
    }
    auto& selector = fp.selectors[s];
    if (grouped) {
      flags |= 1;
      selector.reserve(st.count);
      for (uint32_t i = 0; i < st.count; i++) {
        const uint8_t* rec = st.records.data() + (size_t)i * st.recSize;
        selector.put_u8((uint8_t)groupOf[(uint16_t)(rec[4] << 8 | rec[5])]);
      }
    }
    if (grouped) {
      if (st.cmd == 0x37 && st.recSize >= 59) flags |= 32;
      if (st.cmd == 0x38 && st.recSize >= 29u) flags |= 16;
    }
    if (seedOff && fold_seeds(st, seedOff, seedStream,
                              grouped ? selector.data() : nullptr))
      flags |= 2;
    {
      Buf& b = fp.live();
      b.put_u8(st.cmd);
      b.put_u32(st.recSize);
      b.put_u32(st.count);
      b.put_u8(flags);
      if (flags & 2) {
        size_t before = b.size();
        small_compress(seedStream, level, b, slpz_custom::Rng);
        if (g_stats.enabled) g_stats.seeds += b.size() - before;
      }
    }
    if (!grouped) {
      fp.tasks.push_back({&fp, fp.defer(), PackTask::Ungrouped, s, 0, {},
                          stat, st.records.size()});
      continue;
    }
    {
      Buf& b = fp.live();
      size_t before = b.size();
      small_compress(selector, level, b, slpz_custom::Discrete);
      if (g_stats.enabled) g_stats.selector += b.size() - before;
      b.put_u8((uint8_t)keys.size());
    }
    for (uint32_t g = 0; g < keys.size(); g++)
      fp.tasks.push_back({&fp, fp.defer(), PackTask::Group, s, g, {}, stat,
                          (size_t)counts[g] * st.recSize});
  }
}

// Position predictors use IEEE binary32 round-to-nearest operations. Volatile
// intermediates prevent contraction/excess precision; non-finite predictions
// use zero so NaN payload bits remain literal and reconstruct exactly.
static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4);
#ifdef __FAST_MATH__
#error SLPZ v10 predictors require IEEE floating-point semantics
#endif
static float read_float(const uint8_t* p) {
  uint32_t u = be32(p);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
// Swap exact integral binary32 values in [-4096,4096] with zigzag codes.
// The swap also defines the inverse for subnormals whose bits are those codes;
// all other representations, including signed zero and NaN payloads, pass through.
static uint32_t integer_grid(uint32_t value) {
  if (value <= 8192) {
    int n = int(value >> 1) ^ -int(value & 1);
    float f = float(n);
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
  }
  uint32_t magnitude = value & 0x7fffffffu;
  if (magnitude < 0x3f800000u || magnitude > 0x45800000u) return value;
  // Exact integral values have no significand bits below the units position.
  unsigned shift = 150 - (magnitude >> 23);
  uint32_t mantissa = (magnitude & 0x7fffffu) | 0x800000u;
  unsigned n = mantissa >> shift;
  return (n << shift) == mantissa ? n * 2 - (value >> 31) : value;
}
template <bool inverse>
static void motion_predict_impl(uint8_t* data, uint32_t count, uint32_t size) {
  if (size < 29u) return;
  float prev[2] = {}, velocity[2] = {}, acceleration[2] = {};
  // Percent, shield, and attack velocity fields use first differences of
  // their unsigned bit patterns. Skip fields absent in older record sizes.
  // Percent/shield use first differences. Attack velocities use two-sample
  // extrapolation; animation counters learn their entry value and positive
  // increment separately for each action state.
  uint32_t last[5] = {}, older[4] = {};
  std::array<float, 1024> starts{}, steps{};
  steps.fill(1.f);
  unsigned lastState = 65536;
  auto store = [](uint8_t* p, uint32_t x) {
    p[0] = x >> 24;
    p[1] = x >> 16;
    p[2] = x >> 8;
    p[3] = x;
  };
  auto transform = [&](uint8_t* r) {
    auto field = [&]<unsigned K, unsigned O>() {
      if (O + 4 > size) return;
      uint32_t prediction = last[K];
      if constexpr (K >= 2) {
        float x, y;
        memcpy(&x, &last[K], 4);
        memcpy(&y, &older[K], 4);
        volatile float delta = x - y;
        volatile float p = x + delta;
        float safe = std::isfinite(p) ? p : 0.f;
        memcpy(&prediction, &safe, 4);
      }
      uint32_t v = be32(r + O), actual = inverse ? prediction + ((v >> 1) ^ (0u - (v & 1))) : v;
      uint32_t d = v - prediction, code = inverse ? actual : (d << 1) ^ (0u - (d >> 31));
      older[K] = last[K];
      last[K] = actual;
      store(r + O, code);
    };
#if SLPZ_NEON && !defined(SLPZ_CUSTOM_SCALAR)
    // Paired fields preserve the scalar binary32 operation order without
    // spilling volatile intermediates to the stack.
    auto pair = [&](unsigned offset, unsigned stateIndex, bool extrapolate) {
      auto previous = vld1_u32(last + stateIndex), prediction = previous;
      if (extrapolate) {
        auto x = vreinterpret_f32_u32(previous),
             y = vreinterpret_f32_u32(vld1_u32(older + stateIndex));
        prediction = vreinterpret_u32_f32(vadd_f32(x, vsub_f32(x, y)));
        prediction = vand_u32(prediction, vclt_u32(vand_u32(prediction, vdup_n_u32(0x7fffffffu)),
                                                   vdup_n_u32(0x7f800000u)));
      }
      auto input = vreinterpret_u32_u8(vrev32_u8(vld1_u8(r + offset)));
      uint32x2_t actual, code;
      if constexpr (inverse) {
        auto delta =
            veor_u32(vshr_n_u32(input, 1), vsub_u32(vdup_n_u32(0), vand_u32(input, vdup_n_u32(1))));
        actual = vadd_u32(prediction, delta);
        code = actual;
      } else {
        actual = input;
        auto delta = vsub_u32(actual, prediction);
        code = veor_u32(vshl_n_u32(delta, 1), vsub_u32(vdup_n_u32(0), vshr_n_u32(delta, 31)));
      }
      vst1_u32(older + stateIndex, previous);
      vst1_u32(last + stateIndex, actual);
      vst1_u8(r + offset, vrev32_u8(vreinterpret_u8_u32(code)));
    };
    pair(21, 0, false);
    if (size >= 68)
      pair(60, 2, true);
    else {
      field.template operator()<2, 60>();
      field.template operator()<3, 64>();
    }
#else
    field.template operator()<0, 21>();
    field.template operator()<1, 25>();
    field.template operator()<2, 60>();
    field.template operator()<3, 64>();
#endif
    if (size >= 37) {
      unsigned state = unsigned(r[7]) * 256 + r[8];
      float x;
      memcpy(&x, &last[4], 4);
      float safe;
      if (state < 1024) {
        volatile float p = state == lastState ? x + steps[state] : starts[state];
        safe = std::isfinite(p) ? p : 0.f;
      } else {
        volatile float p = x + 1.f;
        safe = std::isfinite(p) ? p : 0.f;
      }
      uint32_t prediction;
      memcpy(&prediction, &safe, 4);
      prediction = integer_grid(prediction);
      uint32_t v = be32(r + 33);
      if constexpr (!inverse) v = integer_grid(v);
      uint32_t actual = inverse ? prediction + ((v >> 1) ^ (0u - (v & 1))) : v;
      uint32_t d = v - prediction, code = (d << 1) ^ (0u - (d >> 31));
      actual = integer_grid(actual);
      if constexpr (inverse) code = actual;
      if (state < 1024) {
        float a;
        memcpy(&a, &actual, 4);
        if (state != lastState)
          starts[state] = a;
        else if (a > x) {
          volatile float d = a - x;
          steps[state] = d;
        }
      }
      lastState = state;
      last[4] = actual;
      store(r + 33, code);
    }
  };
#if SLPZ_NEON && !defined(SLPZ_CUSTOM_SCALAR)
  // Both axes follow the same binary32 addition order. On inverse passes,
  // the restored attack velocities are already available in predictor state.
  if (size >= 72) {
    auto position = vdup_n_f32(0);
    auto zero = vdup_n_u32(0), one = vdup_n_u32(1);
    auto signMask = vdup_n_u32(0x7fffffffu), infinity = vdup_n_u32(0x7f800000u);
    for (uint32_t i = 0; i < count; ++i) {
      auto* r = data + size_t(i) * size;
      if constexpr (inverse) transform(r);
      float32x2_t self = {read_float(r + (r[46] ? 52 : 68)), read_float(r + 56)};
      uint32x2_t attack;
      if constexpr (inverse)
        attack = vld1_u32(last + 2);
      else
        attack = vreinterpret_u32_u8(vrev32_u8(vld1_u8(r + 60)));
      auto pred =
          vreinterpret_u32_f32(vadd_f32(position, vadd_f32(self, vreinterpret_f32_u32(attack))));
      pred = vand_u32(pred, vclt_u32(vand_u32(pred, signMask), infinity));
      auto input = vreinterpret_u32_u8(vrev32_u8(vld1_u8(r + 9)));
      uint32x2_t actual, code;
      if constexpr (inverse) {
        auto delta = veor_u32(vshr_n_u32(input, 1), vsub_u32(zero, vand_u32(input, one)));
        actual = vadd_u32(pred, delta);
        code = actual;
      } else {
        actual = input;
        auto delta = vsub_u32(actual, pred);
        code = veor_u32(vshl_n_u32(delta, 1), vsub_u32(zero, vshr_n_u32(delta, 31)));
      }
      vst1_u8(r + 9, vrev32_u8(vreinterpret_u8_u32(code)));
      position = vreinterpret_f32_u32(actual);
      if constexpr (!inverse) transform(r);
    }
    return;
  }

  if (size < 72) {
    // The axes are independent; each lane follows the scalar binary32
    // operation order. Residual arithmetic preserves all original bits.
    auto position = vdup_n_f32(0), speed = vdup_n_f32(0), accel = vdup_n_f32(0);
    auto zero = vdup_n_u32(0), one = vdup_n_u32(1);
    auto signMask = vdup_n_u32(0x7fffffffu), infinity = vdup_n_u32(0x7f800000u);
    for (uint32_t i = 0; i < count; ++i) {
      auto* r = data + size_t(i) * size;
      if constexpr (inverse) transform(r);
      auto pred = vreinterpret_u32_f32(vadd_f32(position, vadd_f32(speed, accel)));
      pred = vand_u32(pred, vclt_u32(vand_u32(pred, signMask), infinity));
      auto input = vreinterpret_u32_u8(vrev32_u8(vld1_u8(r + 9)));
      uint32x2_t actual, code;
      if constexpr (inverse) {
        auto delta = veor_u32(vshr_n_u32(input, 1), vsub_u32(zero, vand_u32(input, one)));
        actual = vadd_u32(pred, delta);
        code = actual;
      } else {
        actual = input;
        auto delta = vsub_u32(actual, pred);
        code = veor_u32(vshl_n_u32(delta, 1), vsub_u32(zero, vshr_n_u32(delta, 31)));
      }
      vst1_u8(r + 9, vrev32_u8(vreinterpret_u8_u32(code)));
      auto current = vreinterpret_f32_u32(actual), nextSpeed = vsub_f32(current, position);
      accel = vsub_f32(nextSpeed, speed);
      speed = nextSpeed;
      position = current;
      if constexpr (!inverse) transform(r);
    }
    return;
  }
#endif
  for (uint32_t i = 0; i < count; i++) {
    uint8_t* r = data + (size_t)i * size;
    if constexpr (inverse) transform(r);
    float pred[2] = {};
    if (size >= 72) {
      volatile float vx = read_float(r + (r[46] ? 52 : 68)) + read_float(r + 60);
      volatile float x = prev[0] + vx;
      pred[0] = std::isfinite(x) ? x : 0.f;
      volatile float vy = read_float(r + 56) + read_float(r + 64);
      volatile float y = prev[1] + vy;
      pred[1] = std::isfinite(y) ? y : 0.f;
    } else {
      for (unsigned j = 0; j < 2; ++j) {
        volatile float nextVelocity = velocity[j] + acceleration[j];
        volatile float position = prev[j] + nextVelocity;
        pred[j] = std::isfinite(position) ? position : 0.f;
      }
    }
    for (uint32_t j = 0; j < 2; j++) {
      uint8_t* f = r + 9 + 4 * j;
      uint32_t pb;
      memcpy(&pb, pred + j, 4);
      uint32_t v = be32(f), decoded, residual;
      decoded = inverse ? pb + ((v >> 1) ^ (0u - (v & 1))) : v;
      uint32_t delta = v - pb;
      residual = inverse ? decoded : (delta << 1) ^ (0u - (delta >> 31));
      f[0] = (uint8_t)(residual >> 24);
      f[1] = (uint8_t)(residual >> 16);
      f[2] = (uint8_t)(residual >> 8);
      f[3] = (uint8_t)residual;
      if (size < 72) {
        float position;
        memcpy(&position, &decoded, 4);
        volatile float nextVelocity = position - prev[j];
        volatile float nextAcceleration = nextVelocity - velocity[j];
        velocity[j] = nextVelocity;
        acceleration[j] = nextAcceleration;
      }
      memcpy(prev + j, &decoded, 4);
    }
    if constexpr (!inverse) transform(r);
  }
}
static void motion_predict(uint8_t* data, uint32_t count, uint32_t size, bool inverse) {
  if (inverse) motion_predict_impl<true>(data, count, size);
  else motion_predict_impl<false>(data, count, size);
}
// Swap each exact q/80 controller value with its small zigzag code. The
// explicit binary32 table makes the permutation independent of float division.
static uint32_t controller_grid(uint32_t value) {
  static constexpr uint32_t grid[] = {
      0x0u,        0xbc4ccccdu, 0x3c4ccccdu, 0xbccccccdu, 0x3ccccccdu, 0xbd19999au, 0x3d19999au,
      0xbd4ccccdu, 0x3d4ccccdu, 0xbd800000u, 0x3d800000u, 0xbd99999au, 0x3d99999au, 0xbdb33333u,
      0x3db33333u, 0xbdcccccdu, 0x3dcccccdu, 0xbde66666u, 0x3de66666u, 0xbe000000u, 0x3e000000u,
      0xbe0ccccdu, 0x3e0ccccdu, 0xbe19999au, 0x3e19999au, 0xbe266666u, 0x3e266666u, 0xbe333333u,
      0x3e333333u, 0xbe400000u, 0x3e400000u, 0xbe4ccccdu, 0x3e4ccccdu, 0xbe59999au, 0x3e59999au,
      0xbe666666u, 0x3e666666u, 0xbe733333u, 0x3e733333u, 0xbe800000u, 0x3e800000u, 0xbe866666u,
      0x3e866666u, 0xbe8ccccdu, 0x3e8ccccdu, 0xbe933333u, 0x3e933333u, 0xbe99999au, 0x3e99999au,
      0xbea00000u, 0x3ea00000u, 0xbea66666u, 0x3ea66666u, 0xbeaccccdu, 0x3eaccccdu, 0xbeb33333u,
      0x3eb33333u, 0xbeb9999au, 0x3eb9999au, 0xbec00000u, 0x3ec00000u, 0xbec66666u, 0x3ec66666u,
      0xbecccccdu, 0x3ecccccdu, 0xbed33333u, 0x3ed33333u, 0xbed9999au, 0x3ed9999au, 0xbee00000u,
      0x3ee00000u, 0xbee66666u, 0x3ee66666u, 0xbeeccccdu, 0x3eeccccdu, 0xbef33333u, 0x3ef33333u,
      0xbef9999au, 0x3ef9999au, 0xbf000000u, 0x3f000000u, 0xbf033333u, 0x3f033333u, 0xbf066666u,
      0x3f066666u, 0xbf09999au, 0x3f09999au, 0xbf0ccccdu, 0x3f0ccccdu, 0xbf100000u, 0x3f100000u,
      0xbf133333u, 0x3f133333u, 0xbf166666u, 0x3f166666u, 0xbf19999au, 0x3f19999au, 0xbf1ccccdu,
      0x3f1ccccdu, 0xbf200000u, 0x3f200000u, 0xbf233333u, 0x3f233333u, 0xbf266666u, 0x3f266666u,
      0xbf29999au, 0x3f29999au, 0xbf2ccccdu, 0x3f2ccccdu, 0xbf300000u, 0x3f300000u, 0xbf333333u,
      0x3f333333u, 0xbf366666u, 0x3f366666u, 0xbf39999au, 0x3f39999au, 0xbf3ccccdu, 0x3f3ccccdu,
      0xbf400000u, 0x3f400000u, 0xbf433333u, 0x3f433333u, 0xbf466666u, 0x3f466666u, 0xbf49999au,
      0x3f49999au, 0xbf4ccccdu, 0x3f4ccccdu, 0xbf500000u, 0x3f500000u, 0xbf533333u, 0x3f533333u,
      0xbf566666u, 0x3f566666u, 0xbf59999au, 0x3f59999au, 0xbf5ccccdu, 0x3f5ccccdu, 0xbf600000u,
      0x3f600000u, 0xbf633333u, 0x3f633333u, 0xbf666666u, 0x3f666666u, 0xbf69999au, 0x3f69999au,
      0xbf6ccccdu, 0x3f6ccccdu, 0xbf700000u, 0x3f700000u, 0xbf733333u, 0x3f733333u, 0xbf766666u,
      0x3f766666u, 0xbf79999au, 0x3f79999au, 0xbf7ccccdu, 0x3f7ccccdu, 0xbf800000u, 0x3f800000u};
  if (value <= 160) return grid[value];
  uint32_t magnitude = value & 0x7fffffffu;
  if (magnitude > 0x3f800000u) return value;
  float f;
  memcpy(&f, &magnitude, 4);
  unsigned q = unsigned(f * 80.f + 0.5f), code = q * 2 - (value >> 31);
  return code <= 160 && grid[code] == value ? code : value;
}
template <bool inverse>
static void input_predict_impl(uint8_t* data, uint32_t count, uint32_t size) {
  if (size < 59) return;
  uint32_t table[4][256] = {};
  constexpr unsigned bits = 12;
  unsigned slots = 1u << bits;
  unsigned axes = 1 + (size > 63) + (size > 64) + (size > 65);
  std::vector<uint64_t> pair(axes * slots, UINT64_MAX);

  auto map = [](uint8_t* r) {
    for (unsigned off : {24u, 28u, 32u, 36u}) {
      uint32_t code = controller_grid(be32(r + off));
      r[off] = code >> 24;
      r[off + 1] = code >> 16;
      r[off + 2] = code >> 8;
      r[off + 3] = code;
    }
  };
  const uint32_t offsets[4] = {24, 28, 32, 36}, raw[4] = {58, 63, 64, 65};
  for (uint32_t i = 0; i < count; i++) {
    uint8_t* r = data + (size_t)i * size;
    if constexpr (!inverse) map(r);
    // Processed and physical button copies; float trigger vs max(L,R).
    r[46] ^= r[48];
    r[47] ^= r[49];
    const uint8_t* pred = be32(r + 50) >= be32(r + 54) ? r + 50 : r + 54;
    for (uint32_t j = 0; j < 4; j++) r[40 + j] ^= pred[j];
    for (uint32_t j = 0; j < 4; j++) {
      if (raw[j] >= size) continue;
      uint8_t* f = r + offsets[j];
      uint8_t key = r[raw[j]];
      unsigned other = j ^ 1;
      uint32_t pairKey = key;
      if (raw[other] < size)
        pairKey |= uint32_t(r[raw[other]]) << 8;
      else
        pairKey ^= (be32(r + offsets[other]) & 0x7fffffffu) * 2246822519u;
      unsigned index = j * slots + ((pairKey * 2654435761u) >> (32 - bits));
      uint32_t v = be32(f),
               prediction = pair[index] != UINT64_MAX && uint32_t(pair[index] >> 32) == pairKey
                                ? uint32_t(pair[index])
                                : table[j][key];
      uint32_t actual = inverse ? prediction + ((v >> 1) ^ (0u - (v & 1))) : v;
      uint32_t delta = v - prediction;
      uint32_t code = inverse ? actual : (delta << 1) ^ (0u - (delta >> 31));
      table[j][key] = actual;
      pair[index] = (uint64_t(pairKey) << 32) | actual;
      f[0] = uint8_t(code >> 24);
      f[1] = uint8_t(code >> 16);
      f[2] = uint8_t(code >> 8);
      f[3] = uint8_t(code);
    }
    if constexpr (inverse) map(r);
  }
}
static void input_predict(uint8_t* data, uint32_t count, uint32_t size, bool inverse) {
  if (inverse) input_predict_impl<true>(data, count, size);
  else input_predict_impl<false>(data, count, size);
}
static void run_pack_task(PackTask& t, int level) {
  Buf& out = t.fp->pieces[t.piece];
  switch (t.kind) {
    case PackTask::RawFile: {
      size_t n = custom_frame(t.data, out, slpz_custom::Generic, level);
      if (g_stats.enabled) *t.stat += n;
      break;
    }
    case PackTask::BlobTrial:
      pack_blob(t.data, level, out, *t.stat);
      break;
    case PackTask::BlobFrame: {
      size_t at = out.size();
      if (t.groupIdx) {  // PPC transform, chunk-local word indices
        std::vector<uint8_t> xf(t.data.begin(), t.data.end());
        ppc_branch_xform(xf.data(), xf.size() / 4, false);
        custom_frame(xf, out, slpz_custom::Generic, level);
      } else {
        custom_frame(t.data, out, slpz_custom::Generic, level);
      }
      if (g_stats.enabled) *t.stat += out.size() - at;
      break;
    }
    case PackTask::Ungrouped: {
      auto& st = t.fp->slp.streams[t.stream];
      pack_records(st.records, st.count, st.recSize, level, out, t.stat, st.cmd);
      break;
    }
    case PackTask::Group: {
      auto& st = t.fp->slp.streams[t.stream];
      auto& sel = t.fp->selectors[t.stream];
      uint32_t cnt = t.fp->gCounts[t.stream][t.groupIdx];
      Buf group;
      group.reserve((size_t)cnt * st.recSize);
      for (uint32_t i = 0; i < st.count; i++)
        if (sel[i] == t.groupIdx)
          group.put_bytes(st.records.data() + (size_t)i * st.recSize,
                          st.recSize);
      out.put_u32(cnt);
      if (st.cmd == 0x37) input_predict(group.data(), cnt, st.recSize, false);
      if (st.cmd == 0x38) motion_predict(group.data(), cnt, st.recSize, false);
      pack_records(group, cnt, st.recSize, level, out, t.stat, st.cmd);
      break;
    }
  }
}

static void cmd_compress(const fs::path& outPath, std::vector<fs::path> inputs,
                         int level, int threads) {
  std::vector<FilePlan> plans(inputs.size());
  for (size_t i = 0; i < inputs.size(); i++) {
    plans[i].path = inputs[i];
    plans[i].storedName = inputs[i].generic_string();
  }
  auto t0 = std::chrono::steady_clock::now();
  auto lap = [&](const char* what) {
    if (!getenv("SLPZ_TIMING")) return;
    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "  [%s] %.3fs\n", what,
            std::chrono::duration<double>(t1 - t0).count());
    t0 = t1;
  };
  // pipelined: workers drain the pack heap (largest first) and plan more
  // files whenever no pack work is ready — no barrier between the phases.
  // A sliding admission window bounds memory: a file's parse/group buffers
  // are freed as soon as its last task retires.
  constexpr size_t kWindow = 64;
  {
    std::mutex m;
    std::vector<PackTask*> ready;
    auto heapCmp = [](PackTask* a, PackTask* b) { return a->est < b->est; };
    size_t nextFile = 0;
    std::atomic<size_t> planned{0}, retired{0};
    auto retire = [&](FilePlan& p) {
      p.data = {};
      p.data.shrink_to_fit();
      p.slp = ParsedSlp();
      p.selectors.clear();
      p.gCounts.clear();
      retired.fetch_add(1);
    };
    auto worker = [&]() {
      for (;;) {
        PackTask* t = nullptr;
        size_t fi = SIZE_MAX;
        {
          std::lock_guard<std::mutex> g(m);
          if (!ready.empty()) {
            std::pop_heap(ready.begin(), ready.end(), heapCmp);
            t = ready.back();
            ready.pop_back();
          } else if (nextFile < plans.size() &&
                     nextFile - retired.load() < kWindow) {
            fi = nextFile++;
          }
        }
        if (t) {
          run_pack_task(*t, level);
          if (t->fp->pending.fetch_sub(1) == 1) retire(*t->fp);
        } else if (fi != SIZE_MAX) {
          plan_compress(plans[fi], level);
          plans[fi].pending.store((uint32_t)plans[fi].tasks.size());
          if (plans[fi].tasks.empty()) {
            retire(plans[fi]);
          } else {
            std::lock_guard<std::mutex> g(m);
            for (auto& tk : plans[fi].tasks) {
              ready.push_back(&tk);
              std::push_heap(ready.begin(), ready.end(), heapCmp);
            }
          }
          planned.fetch_add(1);
        } else if (planned.load() < plans.size()) {
          std::this_thread::yield();  // planners/window in flight
        } else {
          return;
        }
      }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads - 1; t++)
      pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();
  }
  lap("plan+pack");

  // cross-replay pools: concatenate same-(cmd, recSize) small streams from
  // every file, in file order, and pack each pool once
  struct Pool {
    uint8_t cmd;
    uint32_t recSize;
    uint32_t total = 0;
    Buf records;
    Buf packed;
  };
  std::vector<Pool> pools;
  for (auto& p : plans)
    for (auto& ps : p.pooled) {
      Pool* pl = nullptr;
      for (auto& q : pools)
        if (q.cmd == ps.cmd && q.recSize == ps.recSize) { pl = &q; break; }
      if (!pl) {
        pools.push_back({ps.cmd, ps.recSize, 0, {}, {}});
        pl = &pools.back();
      }
      pl->total += ps.count;
      pl->records.put_bytes(ps.records.data(), ps.records.size());
      ps.records = Buf();
    }
  parallel_for(pools.size(), threads, [&](size_t i) {
    auto& pl = pools[i];
    pack_records(pl.records, pl.total, pl.recSize, level, pl.packed,
                 g_stats.enabled ? &g_stats.byCmd[pl.cmd] : nullptr, pl.cmd);
    pl.records = Buf();
  });
  lap("pools");

  Buf out;
  out.put_u32(kMagic);
  out.put_u8(kVersion);
  out.put_u8(1);  // custom payload backend
  out.put_u32((uint32_t)pools.size());
  for (auto& pl : pools) {
    out.put_u8(pl.cmd);
    out.put_u32(pl.recSize);
    out.put_u32(pl.total);
    out.put_bytes(pl.packed.data(), pl.packed.size());
  }
  out.put_u32((uint32_t)plans.size());
  uint64_t totalIn = 0;
  for (auto& p : plans) {
    uint64_t blobLen = 0;
    for (auto& piece : p.pieces) blobLen += piece.size();
    out.put_u32((uint32_t)p.storedName.size());
    out.put_bytes(p.storedName.data(), p.storedName.size());
    out.put_u64(p.origSize);
    out.put_u64(blobLen);
    for (auto& piece : p.pieces) out.put_bytes(piece.data(), piece.size());
    totalIn += p.origSize;
  }
  write_file(outPath, out);
  lap("assemble+write");
  fprintf(stderr, "slpz: %zu files, %.1f MB -> %.1f MB (%.2f%%)\n",
          plans.size(), totalIn / 1e6, out.size() / 1e6,
          100.0 * out.size() / (double)totalIn);
  if (g_stats.enabled) {
    fprintf(stderr, "  compressed bytes by section:\n");
    for (int c = 0; c < 256; c++)
      if (g_stats.byCmd[c])
        fprintf(stderr, "    cmd 0x%02x : %10llu\n", c,
                (unsigned long long)g_stats.byCmd[c].load());
    fprintf(stderr, "    order    : %10llu\n", (unsigned long long)g_stats.order.load());
    fprintf(stderr, "    selector : %10llu\n", (unsigned long long)g_stats.selector.load());
    fprintf(stderr, "    metadata : %10llu\n", (unsigned long long)g_stats.suffix.load());
    fprintf(stderr, "    tail     : %10llu\n", (unsigned long long)g_stats.tail.load());
    fprintf(stderr, "    seeds    : %10llu  (folded %llu, exceptions %llu)\n",
            (unsigned long long)g_stats.seeds.load(),
            (unsigned long long)g_stats.seedHit.load(),
            (unsigned long long)g_stats.seedMiss.load());
    fprintf(stderr, "    misc     : %10llu\n", (unsigned long long)g_stats.misc.load());
  }
}

// ── decompress: 3 phases ────────────────────────────────────────────────────
// Phase 1 (parallel per file): walk the blob structure, decode the small side
// streams inline, and record byte spans for every packed section / blob frame.
// Phase 2 (parallel per task, largest first): decode spans into records.
// Phase 3 (parallel per file): gather source/prediction pointers, restore
// seeds and cross-frame residuals during final reconstruction, verify, write.
struct BlobPlan {
  uint8_t mode = 0, xform = 0;
  std::span<const uint8_t> span;                 // modes 0/2/4, including mode byte
  std::vector<std::span<const uint8_t>> chunks;  // mode 3 frames
  std::vector<std::vector<uint8_t>> chunkOut;
  std::vector<uint8_t> out;
};

struct DecStream {
  uint8_t cmd = 0;
  uint32_t recSize = 0, count = 0;
  uint8_t flags = 0;
  int poolIdx = -1;
  uint64_t poolOff = 0;  // record offset of this file's slice in the pool
  std::vector<uint8_t> seedStream, selector;
  std::vector<uint32_t> gCounts;
  std::vector<std::span<const uint8_t>> spans;  // 1 if ungrouped, else per group
  std::vector<OutputBytes> recs;                // task outputs
};

struct DecPlan {
  std::string storedName;
  uint64_t origSize = 0;
  std::span<const uint8_t> blob;
  bool raw = false;
  std::span<const uint8_t> rawFrame;
  std::span<const uint8_t> prefix, ep;
  BlobPlan suffix, tail;
  std::vector<uint8_t> order;
  std::vector<DecStream> streams;
  OutputBytes outData;
  std::atomic<uint32_t> pending{0};
  void release() {
    // note: `vec = {}` binds the initializer_list overload — it empties the
    // vector but KEEPS capacity; swap with a temporary actually deallocates
    OutputBytes().swap(outData);
    streams.clear();
    streams.shrink_to_fit();
    std::vector<uint8_t>().swap(order);
    suffix = BlobPlan();
    tail = BlobPlan();
  }
};

struct DecPool {
  uint8_t cmd = 0;
  uint32_t recSize = 0, total = 0;
  std::span<const uint8_t> span;
  OutputBytes records;
  std::vector<DecPlan*> refs;
  uint64_t cursor = 0;
};

static std::span<const uint8_t> skip_frame(Reader& r) {
  const uint8_t* start = r.p;
  uint32_t comp = r.u32();
  r.u32();
  r.bytes(comp);
  return {start, (size_t)(r.p - start)};
}

static std::span<const uint8_t> skip_packed(Reader& r, uint32_t size) {
  const uint8_t* start = r.p;
  uint8_t nFields = r.u8();
  uint32_t nOut = size;
  bool packedDictionary = false;
  for (uint8_t f = 0; f < nFields; ++f) {
    uint8_t width = r.u8();
    if (f == 0 && (width & 128)) {
      uint8_t transform = (width >> 4) & 7;
      if (!transform || transform > 4) { r.fail = true; return {}; }
      packedDictionary = true;
      width &= 15;
    }
    r.u8(); r.u8();  // offset; the decoder validates bounds and overlap
    uint32_t nVals = uint32_t(r.u8()) + 1;
    if ((width != 2 && width != 4) || nOut < width) { r.fail = true; return {}; }
    if (!packedDictionary) r.bytes(nVals * width);
    nOut -= width - 1 + (nVals == 1);
  }
  if (packedDictionary) { r.u8(); skip_frame(r); }
  r.bytes(nOut);          // transform selectors
  uint8_t layout = r.u8();
  if (layout == 2) skip_frame(r);
  else r.fail = true;
  return {start, (size_t)(r.p - start)};
}

static void scan_blob(Reader& r, BlobPlan& bp) {
  const uint8_t* start = r.p;
  bp.mode = r.u8();
  if (bp.mode == 0) {
    skip_frame(r);
    bp.span = {start, (size_t)(r.p - start)};
  } else if (bp.mode == 2 || bp.mode == 4) {
    r.u8();
    skip_frame(r);
    bp.span = {start, (size_t)(r.p - start)};
  } else if (bp.mode == 3) {
    if (r.u8() != kCustom) { r.fail = true; return; }
    bp.xform = r.u8();
    if (bp.xform > 1) { r.fail = true; return; }
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && !r.fail; i++) bp.chunks.push_back(skip_frame(r));
    bp.chunkOut.resize(bp.chunks.size());
  } else {
    r.fail = true;
  }
}

static void scan_decompress(DecPlan& dp) {
  Reader r{dp.blob.data(), dp.blob.data() + dp.blob.size()};
  uint8_t mode = r.u8();
  if (mode == kModeRaw) {
    dp.raw = true;
    dp.rawFrame = skip_frame(r);
    if (r.fail) die("corrupt archive (raw frame)");
    return;
  }
  if (mode != kModeSplit) die("corrupt archive (mode byte)");
  dp.prefix = r.bytes(kSlpHeaderLen);
  uint8_t epSize = r.u8();
  dp.ep = r.bytes(epSize);
  if (r.fail) die("corrupt archive (slp header)");
  scan_blob(r, dp.suffix);
  scan_blob(r, dp.tail);
  if (r.fail) die("corrupt archive (blob scan)");
  dp.order = small_decompress(r);
  uint8_t nStreams = r.u8();
  dp.streams.resize(nStreams);
  for (auto& st : dp.streams) {
    st.cmd = r.u8();
    st.recSize = r.u32();
    st.count = r.u32();
    st.flags = r.u8();
    if (r.fail) die("corrupt archive (stream header)");
    if (st.flags & 8) continue;  // pooled: records live in the shared pool
    if (st.flags & 2) st.seedStream = small_decompress(r);
    if (!(st.flags & 1)) {
      st.spans.push_back(skip_packed(r, st.recSize));
      st.gCounts.push_back(st.count);
    } else {
      st.selector = small_decompress(r);
      if (st.selector.size() != st.count) die("corrupt archive (selector)");
      uint8_t nGroups = r.u8();
      for (uint8_t g = 0; g < nGroups; g++) {
        st.gCounts.push_back(r.u32());
        st.spans.push_back(skip_packed(r, st.recSize));
      }
    }
    st.recs.resize(st.spans.size());
  }
  if (r.fail) die("corrupt archive (stream scan)");
}

struct DecTask {
  enum Kind : uint8_t { Stream, Blob, BlobChunk, Raw, Pool, AssembleOnly };
  DecPlan* dp = nullptr;
  Kind kind;
  uint32_t stream = 0, idx = 0;
  BlobPlan* bp = nullptr;
  DecPool* pool = nullptr;
  size_t est = 0;
};

static void run_dec_task(DecTask& t) {
  switch (t.kind) {
    case DecTask::Raw: {
      Reader r{t.dp->rawFrame.data(), t.dp->rawFrame.data() + t.dp->rawFrame.size()};
      auto bytes = custom_unframe(r);
      t.dp->outData.assign(bytes.begin(), bytes.end());
      break;
    }
    case DecTask::Blob: {
      Reader r{t.bp->span.data(), t.bp->span.data() + t.bp->span.size()};
      t.bp->out = unpack_blob(r);
      break;
    }
    case DecTask::BlobChunk: {
      auto& c = t.bp->chunks[t.idx];
      Reader r{c.data(), c.data() + c.size()};
      auto& out = t.bp->chunkOut[t.idx];
      out = custom_unframe(r);
      if (t.bp->xform) ppc_branch_xform(out.data(), out.size() / 4, true);
      break;
    }
    case DecTask::Stream: {
      auto& st = t.dp->streams[t.stream];
      auto& sp = st.spans[t.idx];
      Reader r{sp.data(), sp.data() + sp.size()};
      unpack_records(r, st.gCounts[t.idx], st.recSize, st.recs[t.idx]);
      if (st.flags & 16) {
        if (st.cmd != 0x38 || !(st.flags & 1) || st.recSize < 29u)
          die("corrupt motion predictor flag");
        motion_predict(st.recs[t.idx].data(), st.gCounts[t.idx], st.recSize, true);
      }
      if (st.flags & 32) {
        if (st.cmd != 0x37 || !(st.flags & 1) || st.recSize < 59)
          die("corrupt input predictor flag");
        input_predict(st.recs[t.idx].data(), st.gCounts[t.idx], st.recSize, true);
      }
      break;
    }
    case DecTask::Pool: {
      Reader r{t.pool->span.data(), t.pool->span.data() + t.pool->span.size()};
      unpack_records(r, t.pool->total, t.pool->recSize, t.pool->records);
      break;
    }
    case DecTask::AssembleOnly:
      break;
  }
}

static void finish_blob(BlobPlan& bp) {
  if (bp.mode != 3) return;
  size_t total = 0;
  for (auto& c : bp.chunkOut) total += c.size();
  bp.out.reserve(total);
  for (auto& c : bp.chunkOut) bp.out.insert(bp.out.end(), c.begin(), c.end());
}

// Copy grouped records straight into their final event positions. Small
// source/prediction pointers let RNG and cross-frame restoration happen while
// the destination record is still hot in cache.
static void assemble_decompress(DecPlan& dp, std::vector<DecPool>& pools) {
  if (dp.raw) {
    if (dp.outData.size() != dp.origSize) die("size mismatch for " + dp.storedName);
    return;
  }
  finish_blob(dp.suffix);
  finish_blob(dp.tail);
  int16_t idxOf[256];
  for (auto& i : idxOf) i = -1;
  struct Source {
    std::vector<const uint8_t*> records, predictions;
    std::vector<size_t> seedCursors;
    uint32_t at = 0, seed = 0, seedOffset = 0;
  };
  std::vector<Source> sources(dp.streams.size());
  size_t rawSize = 1 + dp.ep.size() + dp.tail.out.size();
  for (size_t j = 0; j < dp.streams.size(); ++j) {
    auto& st = dp.streams[j];
    auto& src = sources[j];
    if (idxOf[st.cmd] >= 0) die("corrupt archive (duplicate stream)");
    idxOf[st.cmd] = int16_t(j);
    rawSize += size_t(st.recSize) * st.count + st.count;
    src.records.resize(st.count);
    if (st.flags & 8) {
      if (st.poolIdx < 0) die("corrupt archive (pool ref)");
      auto& pl = pools[size_t(st.poolIdx)];
      const uint8_t* base = pl.records.data();
      if (st.recSize) base += st.poolOff * st.recSize;
      for (unsigned i = 0; i < st.count; ++i)
        src.records[i] = st.recSize ? base + size_t(i) * st.recSize : base;
    } else {
      if (st.recs.size() != st.gCounts.size()) die("corrupt archive (groups)");
      for (size_t g = 0; g < st.recs.size(); ++g)
        if (st.recs[g].size() != size_t(st.gCounts[g]) * st.recSize)
          die("corrupt archive (group size)");
      std::vector<uint32_t> used(st.recs.size());
      for (unsigned i = 0; i < st.count; ++i) {
        unsigned g = st.flags & 1 ? st.selector[i] : 0;
        if (g >= used.size() || used[g] >= st.gCounts[g]) die("corrupt archive (selector count)");
        const uint8_t* base = st.recs[g].data();
        src.records[i] = st.recSize ? base + size_t(used[g]) * st.recSize : base;
        ++used[g];
      }
    }
    if (st.flags & 2) {
      src.seedOffset = seed_offset_for(st.cmd, st.recSize);
      if (!src.seedOffset) die("corrupt archive (seed flag on seedless cmd)");
      bool grouped = st.flags & 1;
      unsigned groups = grouped ? unsigned(st.gCounts.size()) : 1;
      src.seedCursors.resize(groups);
      size_t pos = 0;
      bool fail = false;
      for (unsigned g = 0; g < groups; ++g) {
        src.seedCursors[g] = pos;
        unsigned n = grouped ? st.gCounts[g] : st.count;
        for (unsigned i = 0; i < n; ++i) {
          uint32_t k = get_varint(st.seedStream, pos, fail);
          if (k == kSeedException) pos += 4;
          if (fail || pos > st.seedStream.size()) die("corrupt archive (seed stream)");
        }
      }
    }
  }
  for (size_t j = 0; j < dp.streams.size(); ++j) {
    auto& pre = dp.streams[j];
    if (!(pre.flags & 4)) continue;
    if (pre.cmd != 0x37 || idxOf[0x38] < 0) die("corrupt archive (xpred flag)");
    unsigned postIdx = idxOf[0x38];
    auto& post = dp.streams[postIdx];
    if (!cross_pred_eligible(pre.recSize, pre.count, post.recSize, post.count))
      die("corrupt archive (xpred shape)");
    auto& predictions = sources[j].predictions;
    predictions.resize(pre.count, nullptr);
    static thread_local std::array<uint32_t, 65536> heads{};
    static thread_local std::array<bool, 65536> seen{};
    std::unique_ptr<uint32_t[]> next(new uint32_t[post.count]);
    std::vector<uint16_t> keys;
    for (unsigned i = post.count; i-- > 0;) {
      const uint8_t* q = sources[postIdx].records[i];
      uint16_t k = uint16_t(q[4] << 8 | q[5]);
      if (!heads[k]) {
        keys.push_back(k);
        seen[k] = false;
      }
      next[i] = heads[k];
      heads[k] = i + 1;
    }
    for (unsigned i = 0; i < pre.count; ++i) {
      const uint8_t* p = sources[j].records[i];
      uint16_t k = uint16_t(p[4] << 8 | p[5]);
      if (!heads[k]) continue;
      if (!seen[k]) {
        seen[k] = true;
        continue;
      }
      unsigned at = heads[k] - 1;
      predictions[i] = sources[postIdx].records[at];
      heads[k] = next[at];
    }
    for (auto k : keys) heads[k] = 0;
  }
  auto& out = dp.outData;
  out.resize(dp.prefix.size() + rawSize + dp.suffix.out.size());
  uint8_t* dest = out.data();
  auto append = [&](std::span<const uint8_t> part) {
    if (!part.empty()) memcpy(dest, part.data(), part.size());
    dest += part.size();
  };
  append(dp.prefix);
  *dest++ = 0x35;
  append(dp.ep);
  for (uint8_t cmd : dp.order) {
    int16_t j = idxOf[cmd];
    if (j < 0) die("corrupt archive (order stream)");
    auto& st = dp.streams[size_t(j)];
    auto& src = sources[size_t(j)];
    unsigned i = src.at++;
    if (i >= st.count) die("corrupt archive (order count)");
    *dest++ = cmd;
    if (st.recSize) memcpy(dest, src.records[i], st.recSize);
    if (src.seedOffset) {
      unsigned g = st.flags & 1 ? st.selector[i] : 0;
      if (g >= src.seedCursors.size()) die("corrupt archive (seed group)");
      auto& cursor = src.seedCursors[g];
      bool fail = false;
      uint32_t k = get_varint(st.seedStream, cursor, fail);
      if (k == kSeedException) {
        if (cursor + 4 > st.seedStream.size())
          fail = true;
        else {
          memcpy(&src.seed, st.seedStream.data() + cursor, 4);
          cursor += 4;
        }
      } else
        src.seed = lcg_advance(src.seed, k);
      if (fail) die("corrupt archive (seed stream)");
      auto* p = dest + src.seedOffset;
      p[0] = src.seed >> 24;
      p[1] = src.seed >> 16;
      p[2] = src.seed >> 8;
      p[3] = src.seed;
    }
    if (!src.predictions.empty())
      if (const uint8_t* q = src.predictions[i]) {
        for (unsigned k = 0; k < kLenA; ++k) dest[kPreA + k] ^= q[kPostA + k];
        for (unsigned k = 0; k < kLenB; ++k) dest[kPreB + k] ^= q[kPostB + k];
      }
    dest += st.recSize;
  }
  append(dp.tail.out);
  append(dp.suffix.out);
  if (dest != out.data() + out.size() || out.size() != dp.origSize)
    die("size mismatch for " + dp.storedName);
  for (size_t j = 0; j < dp.streams.size(); ++j) {
    if (sources[j].at != dp.streams[j].count) die("corrupt archive (order count)");
    dp.streams[j].recs.clear();
    dp.streams[j].recs.shrink_to_fit();
  }
}

static void cmd_extract(const fs::path& arcPath, const fs::path& outDir,
                        int threads, bool testOnly, bool hashMode = false) {
  auto arc = read_file(arcPath);
  Reader r{arc.data(), arc.data() + arc.size()};
  if (r.u32() != kMagic) die("not a slpz archive");
  uint8_t version = r.u8(), flags = r.u8();
  if ((version != kVersion && version != kOriginalVersion) || flags != 1 || r.fail)
    die("unsupported slpz format (requires v1 custom or its original v15 header; use the compatibility reader for older formats)");
  uint32_t nPools = r.u32();
  std::vector<DecPool> pools(nPools);
  for (auto& pl : pools) {
    pl.cmd = r.u8();
    pl.recSize = r.u32();
    pl.total = r.u32();
    pl.span = skip_packed(r, pl.recSize);
    if (r.fail) die("corrupt archive (pool directory)");
  }
  uint32_t n = r.u32();
  std::vector<DecPlan> plans(n);
  for (auto& dp : plans) {
    uint32_t nameLen = r.u32();
    auto name = r.bytes(nameLen);
    dp.origSize = r.u64();
    uint64_t blobLen = r.u64();
    auto blob = r.bytes(blobLen);
    if (r.fail) die("corrupt archive (directory)");
    dp.storedName.assign((const char*)name.data(), name.size());
    dp.blob = blob;
  }

  auto t0 = std::chrono::steady_clock::now();
  auto lap = [&](const char* what) {
    if (!getenv("SLPZ_TIMING")) return;
    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "  [%s] %.3fs\n", what,
            std::chrono::duration<double>(t1 - t0).count());
    t0 = t1;
  };
  parallel_for(plans.size(), threads,
               [&](size_t i) { scan_decompress(plans[i]); });
  lap("scan");

  // resolve pool slices in file order; each pooled stream is a dependency
  for (auto& dp : plans) {
    if (dp.raw) continue;
    for (auto& st : dp.streams) {
      if (!(st.flags & 8)) continue;
      DecPool* pl = nullptr;
      for (size_t i = 0; i < pools.size(); i++)
        if (pools[i].cmd == st.cmd && pools[i].recSize == st.recSize) {
          st.poolIdx = (int)i;
          pl = &pools[i];
          break;
        }
      if (!pl || pl->cursor + st.count > pl->total)
        die("corrupt archive (pool slice)");
      st.poolOff = pl->cursor;
      pl->cursor += st.count;
    }
  }

#ifndef SLPZ_FUZZ  // memfs needs no directories
  if (!testOnly)
    for (auto& dp : plans)
      fs::create_directories(
          (outDir / fs::path(dp.storedName).relative_path()).parent_path());
#endif

  // decode pools up front: they are small, live for the whole run, and every
  // wave below may reference them — no per-file dependency accounting needed
  if (!pools.empty()) {
    std::vector<DecTask> ptasks;
    for (auto& pl : pools)
      ptasks.push_back({nullptr, DecTask::Pool, 0, 0, nullptr, &pl, 0});
    parallel_for(ptasks.size(), threads,
                 [&](size_t i) { run_dec_task(ptasks[i]); });
  }
  lap("pools");

  // dedicated writer threads: decode workers never block on the filesystem,
  // and APFS gets its preferred low write concurrency. Queue is bounded so
  // memory stays proportional to in-flight files.
  struct WriteJob {
    fs::path path;
    OutputBytes data;
  };
  std::mutex wm;
  std::condition_variable wcv, wspace;
  std::deque<WriteJob> wq;
  bool wdone = false;
  std::vector<std::thread> writers;
  if (!testOnly)
    for (int i = 0; i < 3; i++)
      writers.emplace_back([&] {
        for (;;) {
          WriteJob job;
          {
            std::unique_lock<std::mutex> lk(wm);
            wcv.wait(lk, [&] { return !wq.empty() || wdone; });
            if (wq.empty()) return;
            job = std::move(wq.front());
            wq.pop_front();
          }
          wspace.notify_one();
          write_file(job.path, job.data);
        }
      });
  std::vector<std::string> hashes(hashMode ? plans.size() : 0);
  auto finish = [&](DecPlan& dp) {
    assemble_decompress(dp, pools);
    if (hashMode) {
#if defined(__APPLE__)
      uint8_t d[CC_SHA1_DIGEST_LENGTH];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      CC_SHA1(dp.outData.data(), (CC_LONG)dp.outData.size(), d);
#pragma clang diagnostic pop
#else
      uint8_t d[SHA_DIGEST_LENGTH];
      SHA1(dp.outData.data(), dp.outData.size(), d);
#endif
      char hex[41];
      for (int b = 0; b < 20; b++) snprintf(hex + 2 * b, 3, "%02x", d[b]);
      hashes[(size_t)(&dp - plans.data())].assign(hex, 40);
    }
    if (!testOnly) {
      std::unique_lock<std::mutex> lk(wm);
      wspace.wait(lk, [&] { return wq.size() < 48; });
      wq.push_back({outDir / fs::path(dp.storedName).relative_path(),
                    std::move(dp.outData)});
      wcv.notify_one();
    }
    dp.release();  // free this file's memory now, not at exit
  };
  // admission waves: only ~kDecBudget of decoded output is in flight at once,
  // so peak memory stays bounded instead of scaling with archive raw size
  constexpr uint64_t kDecBudget = 2ull << 30;
  size_t next = 0, waveNo = 0;
  while (next < plans.size()) {
    size_t first = next;
    uint64_t bytes = 0;
    std::vector<DecTask> tasks;
    while (next < plans.size() && (next == first || bytes < kDecBudget)) {
      auto& dp = plans[next++];
      bytes += dp.origSize;
      size_t before = tasks.size();
      if (dp.raw) {
        tasks.push_back({&dp, DecTask::Raw, 0, 0, nullptr, nullptr,
                         dp.rawFrame.size()});
      } else {
        for (auto* bp : {&dp.suffix, &dp.tail}) {
          if (bp->mode == 3)
            for (uint32_t i = 0; i < bp->chunks.size(); i++)
              tasks.push_back({&dp, DecTask::BlobChunk, 0, i, bp, nullptr,
                               bp->chunks[i].size()});
          else if (!bp->span.empty())
            tasks.push_back({&dp, DecTask::Blob, 0, 0, bp, nullptr, bp->span.size()});
        }
        for (uint32_t s = 0; s < dp.streams.size(); s++)
          for (uint32_t g = 0; g < dp.streams[s].spans.size(); g++)
            tasks.push_back({&dp, DecTask::Stream, s, g, nullptr, nullptr,
                             (size_t)dp.streams[s].gCounts[g] * dp.streams[s].recSize});
      }
      dp.pending.fetch_add((uint32_t)(tasks.size() - before));
      if (dp.pending.load() == 0)  // nothing to decode (e.g. empty file)
        tasks.push_back({&dp, DecTask::AssembleOnly, 0, 0, nullptr, nullptr, 0});
    }
    std::sort(tasks.begin(), tasks.end(),
              [](const DecTask& a, const DecTask& b) { return a.est > b.est; });
    parallel_for(tasks.size(), threads, [&](size_t i) {
      auto& t = tasks[i];
      run_dec_task(t);
      if (t.kind == DecTask::AssembleOnly) finish(*t.dp);
      else if (t.dp->pending.fetch_sub(1) == 1) finish(*t.dp);
    });
    // Return buffers between waves to keep memory bounded. The final wave
    // needs no allocator-wide sweep; the CLI is about to exit.
#if defined(__APPLE__)
    if (next < plans.size()) malloc_zone_pressure_relief(nullptr, 0);
#else
    if (next < plans.size()) malloc_trim(0);
#endif
    if (getenv("SLPZ_TIMING")) {
#if defined(__APPLE__)
      task_vm_info_data_t vi;
      mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
      task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt);
      fprintf(stderr, "  wave %zu files [%zu,%zu) bytes %.2f GB footprint %.2f GB\n",
              waveNo++, first, next, bytes / 1073741824.0,
              vi.phys_footprint / 1073741824.0);
#else
      long pages = 0;
      if (FILE* statm = fopen("/proc/self/statm", "r")) {
        long ignored = 0;
        if (fscanf(statm, "%ld %ld", &ignored, &pages) != 2) pages = 0;
        fclose(statm);
      }
      double rss = (double)pages * (double)sysconf(_SC_PAGESIZE) / 1073741824.0;
      fprintf(stderr, "  wave %zu files [%zu,%zu) bytes %.2f GB rss %.2f GB\n",
              waveNo++, first, next, bytes / 1073741824.0, rss);
#endif
    }
  }
  {
    std::lock_guard<std::mutex> lk(wm);
    wdone = true;
  }
  wcv.notify_all();
  for (auto& w : writers) w.join();
  lap("decode+assemble+write");
  for (size_t i = 0; i < hashes.size(); i++)  // shasum-compatible, to stdout
    printf("%s  %s\n", hashes[i].c_str(), plans[i].storedName.c_str());
  fprintf(stderr, "slpz: %s %zu files\n",
          hashMode ? "hashed" : testOnly ? "tested" : "extracted",
          plans.size());
}

// ── selftest: NEON kernels vs scalar references ─────────────────────────────
static int cmd_selftest() {
  std::mt19937 rng(1234);
  auto rnd = [&](size_t n) {
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = (uint8_t)rng();
    return v;
  };
  int fails = 0;
  auto check = [&](bool ok, const char* what, size_t a, size_t b) {
    if (!ok) { fprintf(stderr, "FAIL %s (%zu,%zu)\n", what, a, b); fails++; }
  };
  for (uint32_t count : {1u, 5u, 15u, 16u, 17u, 100u, 1000u, 4097u}) {
    for (uint32_t size : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u,
                          12u, 13u, 14u, 15u, 16u, 33u, 51u, 63u, 66u, 71u, 84u, 517u}) {
      auto recs = rnd((size_t)count * size);
      Buf planes;
      transpose_fwd(recs, count, size, planes);
      for (uint32_t i = 0; i < count && !fails; i++)
        for (uint32_t j = 0; j < size; j++)
          if (planes[(size_t)j * count + i] != recs[(size_t)i * size + j]) {
            check(false, "transpose_fwd", count, size);
            break;
          }
      std::vector<uint8_t> back(recs.size());
      transpose_rev(planes, count, size, back.data());
      check(memcmp(back.data(), recs.data(), recs.size()) == 0,
            "transpose_rev", count, size);
    }
  }
  for (size_t n : {0u, 1u, 2u, 15u, 16u, 17u, 31u, 100u, 65536u, 100001u}) {
    auto orig = rnd(n);
    auto v = orig;
    delta_fwd<false>(v.data(), n);
    // scalar reference for encode
    auto ref = orig;
    for (size_t i = n; i-- > 1;) ref[i] = (uint8_t)(ref[i] - ref[i - 1]);
    check(v == ref, "delta_fwd/sub", n, 0);
    scan_rev<false>(v.data(), n);
    check(v == orig, "scan_rev/sub", n, 0);
    v = orig;
    delta_fwd<true>(v.data(), n);
    ref = orig;
    for (size_t i = n; i-- > 1;) ref[i] = (uint8_t)(ref[i] ^ ref[i - 1]);
    check(v == ref, "delta_fwd/xor", n, 0);
    scan_rev<true>(v.data(), n);
    check(v == orig, "scan_rev/xor", n, 0);
  }
  // Custom frame roundtrip, including empty and opaque inputs.
  for (size_t n : {0u, 1u, 100u, 65536u, 1000001u}) {
    auto orig = rnd(n);
    Buf enc;
    custom_frame(orig, enc);
    Reader r{enc.data(), enc.data() + enc.size()};
    auto dec = custom_unframe(r);
    check(dec == orig && r.p == r.end, "custom frame roundtrip", n, 0);
  }
  // ppc transform roundtrip (random words exercise both branch opcodes)
  for (size_t n : {0u, 1u, 4u, 1024u, 100003u}) {
    auto orig = rnd(n * 4);
    auto v = orig;
    ppc_branch_xform(v.data(), n, false);
    ppc_branch_xform(v.data(), n, true);
    check(v == orig, "ppc xform roundtrip", n, 0);
  }
  // pack/unpack roundtrip: random planes, drifting counters, and a
  // dictionarizable 4-byte field (j in [4,8) cycles through 23 values)
  {
    for (auto [count, size] : {std::pair<uint32_t, uint32_t>{20000, 13},
                               {5000, 63}, {100, 5}, {1, 9}, {6000, 1},
                               {70, 12}}) {
      std::vector<uint8_t> recs((size_t)count * size);
      uint32_t v = 12345;
      for (uint32_t i = 0; i < count; i++)
        for (uint32_t j = 0; j < size; j++) {
          v = v * 1103515245 + 12345;
          uint8_t b = (j % 3 == 0) ? (uint8_t)(v >> 24)
                    : (j % 3 == 1) ? (uint8_t)(i >> 3)
                                   : (uint8_t)((v >> 28) + i);
          if (j >= 4 && j < 8 && size >= 8)
            b = (uint8_t)(((i % 23) * 41) + j);  // 23 distinct u32s
          if (j >= 8 && j < 10 && size >= 10)
            b = (uint8_t)(((i % 11) * 3) + j);   // 11 distinct u16s
          recs[(size_t)i * size + j] = b;
        }
      for (int level : {3, 12}) {
        Buf blob;
        pack_records(recs, count, size, level, blob, nullptr);
        Reader r{blob.data(), blob.data() + blob.size()};
        std::vector<uint8_t> back;
        unpack_records(r, count, size, back);
        check(back == recs, "pack/unpack roundtrip", count, size);
      }
    }
  }
  fprintf(stderr, fails ? "selftest: %d FAILURES\n" : "selftest: all passed\n", fails);
  return fails ? 1 : 0;
}

// ── CLI ─────────────────────────────────────────────────────────────────────
static void usage() {
  die("usage:\n"
      "  slpz c -o out.slpz [-l level] [-T threads] [-v] "
      "<file.slp...|@listfile>\n"
      "  slpz x archive.slpz -d outdir [-T threads]\n"
      "  slpz t archive.slpz [-T threads]\n"
      "  slpz hash archive.slpz [-T threads]   (sha1 per file, shasum format)\n"
      "  slpz selftest");
}

#ifndef SLPZ_FUZZ  // fuzz harness (fuzz/slpz_fuzz.cpp) supplies its own entry
int main(int argc, char** argv) {
  if (std::fesetround(FE_TONEAREST)) die("cannot set IEEE rounding mode");
  if (argc < 2) usage();
  std::string mode = argv[1];
  if (mode == "selftest") return cmd_selftest();
  if (argc < 3) usage();
  fs::path out, dir;
  int level = 3, threads = (int)std::thread::hardware_concurrency();
  std::vector<fs::path> inputs;
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (++i >= argc) usage();
      return argv[i];
    };
    if (a == "-o") out = next();
    else if (a == "-d") dir = next();
    else if (a == "-l") level = std::stoi(next());
    else if (a == "-T") threads = std::stoi(next());
    else if (a == "--codec") {
      // Accept existing custom-encoder scripts without retaining backend selection.
      if (next() != "custom") die("only the custom codec is supported");
    } else if (a == "-v")
      g_stats.enabled = true;
    else if (a[0] == '@') {
      std::ifstream lf(a.substr(1));
      if (!lf) die("cannot open list file " + a.substr(1));
      for (std::string line; std::getline(lf, line);)
        if (!line.empty()) inputs.emplace_back(line);
    } else inputs.emplace_back(a);
  }
  if (threads < 1) threads = 1;

  if (mode == "c") {
    if (out.empty() || inputs.empty()) usage();
    cmd_compress(out, inputs, level, threads);
  } else if (mode == "x" || mode == "t" || mode == "hash") {
    if (inputs.size() != 1 || (mode == "x" && dir.empty())) usage();
    cmd_extract(inputs[0], dir, threads, mode != "x", mode == "hash");
  } else usage();
  fflush(nullptr);
  std::_Exit(0);  // skip destructor teardown of multi-GB buffers
}
#endif  // SLPZ_FUZZ
