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
//   4. compress each plane buffer with zstd.
// A one-byte-per-event order stream (itself extremely compressible) lets us
// reconstruct the exact interleaving. Anything that doesn't parse — truncated
// events, unknown commands, non-.slp input — falls back to raw zstd of the
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
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <lzma.h>
#include <unistd.h>
#include <zstd.h>

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
constexpr uint8_t kVersion = 9;
constexpr uint8_t kCustomVersion = 10;
static bool g_custom = false, g_tuned = false;
constexpr uint8_t kModeRaw = 0;    // whole file = one zstd blob
constexpr uint8_t kModeSplit = 1;  // parsed event streams

constexpr size_t kSlpHeaderLen = 15;  // "{U\x03raw[$U#l" + u32 BE raw length
constexpr char kSlpMagic[] = "{U\x03raw[$U#l";

enum : uint8_t { T_PLAIN = 0, T_SUB = 1, T_XOR = 2 };

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

// v10 custom frames are self-describing; semantic labels are part of the
// payload. The legacy v9 framing and codec IDs remain unchanged.
static size_t custom_frame(std::span<const uint8_t> in, Buf& out,
                           slpz_custom::Profile profile = slpz_custom::Generic, int level = 3) {
  try {
    auto packed = slpz_custom::encode(in, profile, level >= 9 ? 24 : level <= 2 ? 2 : 8);
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
                                             level >= 9   ? 24
                                             : level <= 2 ? 2
                                                          : 8);
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
  std::atomic<uint64_t> lzmaWin{0}, zstdWin{0};
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

// steps from a to b, or UINT32_MAX if > kSeedCap
static uint32_t lcg_distance(uint32_t a, uint32_t b) {
  for (uint32_t k = 0; k <= kSeedCap; k++) {
    if (a == b) return k;
    a = lcg_next(a);
  }
  return UINT32_MAX;
}

// ── zstd wrappers (contexts reused per worker thread) ───────────────────────
struct Zctx {
  ZSTD_CCtx* c = ZSTD_createCCtx();
  ZSTD_DCtx* d = ZSTD_createDCtx();
  ~Zctx() { ZSTD_freeCCtx(c); ZSTD_freeDCtx(d); }
};

// ── LZMA raw frames ─────────────────────────────────────────────────────────
// LZMA's literal coder models (prev-byte high bits, position) context —
// exactly the order-1 structure the transformed planes carry, which zstd's
// order-0 literal stage cannot see. Measured on transformed planes: LZMA
// fast ≈ 10% smaller than zstd -3 at 76 MB/s/thread; normal ≈ zstd -19
// quality at similar speed to zstd -12. pb=0 because planes are positionless
// byte streams.
enum : uint8_t {
  kLzmaFast = 1,
  kLzmaNormal = 2,
  kLzmaLite = 3,
  kBrotli = 4,
  kLzmaMax = 5,
  kCustom = 6
};

// planes codec per CLI level: brotli q5 measured +3.5% size vs LZMA fast but
// 2.6x faster encode / 3.5x faster decode -> it is the speed tier
static uint8_t codec_id_for_level(int level) {
  if (g_custom) return kCustom;
  return level <= 2 ? kBrotli : level <= 8 ? kLzmaFast : kLzmaMax;
}

static lzma_options_lzma lzma_opts_for(uint8_t id) {
  lzma_options_lzma o;
  lzma_lzma_preset(&o, id == kLzmaNormal ? 6 : 1);
  o.lc = 3;
  o.lp = 0;
  o.pb = 0;
  if (id == kLzmaLite) {
    o.dict_size = 1u << 20;
    o.mode = LZMA_MODE_FAST;
    o.nice_len = 32;
    o.mf = LZMA_MF_HC4;
  } else if (id == kLzmaFast) {
    // nice=128 + 8MB dict measured 2% smaller than nice=32 + 1MB at the
    // same throughput (~57 MB/s/thread) on transformed planes
    o.dict_size = 8u << 20;
    o.mode = LZMA_MODE_FAST;
    o.nice_len = 128;
    o.mf = LZMA_MF_HC4;
  } else if (id == kLzmaMax) {
    // BT4/nice=273: -6.4% on frame planes at ~5 MB/s/thread — used only at
    // -l >=9 where the split routes just the entropy-rich planes here
    o.dict_size = 8u << 20;
    o.mode = LZMA_MODE_NORMAL;
    o.nice_len = 273;
    o.mf = LZMA_MF_BT4;
  } else {
    o.dict_size = 8u << 20;
    o.mode = LZMA_MODE_NORMAL;
    o.nice_len = 64;
    o.mf = LZMA_MF_HC4;
  }
  return o;
}

#ifdef SLPZ_FUZZ_STUB_SLOW
// Fuzzing shortcut: replace the slow, upstream-hardened codecs (LZMA, brotli)
// with an identity store — [u32 n][u32 n][raw n bytes], the same frame layout —
// so the round-trip stops spending CPU on code we trust and aren't fuzzing.
// zstd stays real, so the packer's size-based mode/layout choices remain honest
// and the split/alt decode paths still get exercised. Encoder and decoder are
// stubbed together, so round-trip byte-identity still holds.
static size_t fuzz_store_frame(std::span<const uint8_t> in, Buf& out) {
  uint32_t n = (uint32_t)in.size();
  out.put_u32(n);
  out.put_u32(n);
  out.put_bytes(in.data(), in.size());
  return n;
}
static std::vector<uint8_t> fuzz_load_frame(Reader& r) {
  uint32_t comp = r.u32();
  (void)r.u32();  // orig == comp for identity frames
  auto src = r.bytes(comp);
  if (r.fail) die("corrupt archive (stub frame)");
  return std::vector<uint8_t>(src.begin(), src.end());
}
#endif

// frame layout mirrors z_compress: u32 compLen, u32 origLen, bytes
static size_t lzma_compress_frame(std::span<const uint8_t> in, uint8_t id, Buf& out) {
  if (g_custom) return custom_frame(in, out);
#ifdef SLPZ_FUZZ_STUB_SLOW
  (void)id;
  return fuzz_store_frame(in, out);
#endif
  lzma_options_lzma o = lzma_opts_for(id);
  if (g_tuned) {
    uint32_t size = 4096;
    while (size < in.size() && size < o.dict_size) size *= 2;
    o.dict_size = std::min(o.dict_size, size);
  }
  lzma_filter filters[2] = {{LZMA_FILTER_LZMA2, &o}, {LZMA_VLI_UNKNOWN, nullptr}};
  size_t bound = lzma_stream_buffer_bound(in.size());
  size_t at = out.size();
  out.resize(at + 8 + bound);
  size_t pos = 0;
  lzma_ret rc = in.empty()
                    ? LZMA_OK  // empty frame: comp=0, orig=0
                    : lzma_raw_buffer_encode(filters, nullptr, in.data(),
                                             in.size(), out.data() + at + 8,
                                             &pos, bound);
  if (rc != LZMA_OK) die("lzma encode failed");
  out.resize(at + 8 + pos);
  uint32_t comp = (uint32_t)pos, orig = (uint32_t)in.size();
  memcpy(out.data() + at, &comp, 4);
  memcpy(out.data() + at + 4, &orig, 4);
  return pos;
}

static std::vector<uint8_t> lzma_decompress_frame(Reader& r, uint8_t id);
static size_t z_compress(Zctx& z, std::span<const uint8_t> in, int level, Buf& out);
static std::vector<uint8_t> z_decompress(Zctx& z, Reader& r);
static std::vector<uint8_t> codec_decompress_frame(Zctx& z, Reader& r, uint8_t id);

// small side-streams (order, seeds, selectors): u8 codec + frame,
// zstd vs lzma-fast trial, smaller wins
static void small_compress(Zctx& z, std::span<const uint8_t> in, int level, Buf& blob,
                           slpz_custom::Profile profile = slpz_custom::Generic) {
  if (g_custom) {
    blob.put_u8(kCustom);
    custom_frame(in, blob, profile, level);
    return;
  }
  Buf a, b;
  z_compress(z, in, level, a);
  if (in.size() >= 4096) lzma_compress_frame(in, kLzmaFast, b);
  bool useL = !b.empty() && b.size() < a.size();
  blob.put_u8(useL ? kLzmaFast : 0);
  const Buf& w = useL ? b : a;
  blob.put_bytes(w.data(), w.size());
}

static std::vector<uint8_t> small_decompress(Zctx& z, Reader& r) {
  uint8_t codec = r.u8();
  if (r.fail) die("corrupt archive (side stream)");
  return codec_decompress_frame(z, r, codec);
}

static std::vector<uint8_t> lzma_decompress_frame(Reader& r, uint8_t id) {
#ifdef SLPZ_FUZZ_STUB_SLOW
  (void)id;
  return fuzz_load_frame(r);
#endif
  uint32_t comp = r.u32(), orig = r.u32();
  auto src = r.bytes(comp);
  if (r.fail) die("corrupt archive (lzma frame)");
  if (orig == 0) return {};
  lzma_options_lzma o = lzma_opts_for(id);
  lzma_filter filters[2] = {{LZMA_FILTER_LZMA2, &o}, {LZMA_VLI_UNKNOWN, nullptr}};
  std::vector<uint8_t> out(orig);
  size_t inPos = 0, outPos = 0;
  lzma_ret rc = lzma_raw_buffer_decode(filters, nullptr, src.data(), &inPos,
                                       comp, out.data(), &outPos, orig);
  if (rc != LZMA_OK || outPos != orig) die("corrupt archive (lzma payload)");
  return out;
}

// brotli frames (quality 5, lgwin 22), same u32 comp / u32 orig framing
static size_t brotli_compress_frame(std::span<const uint8_t> in, Buf& out) {
#ifdef SLPZ_FUZZ_STUB_SLOW
  return fuzz_store_frame(in, out);
#endif
  size_t bound = BrotliEncoderMaxCompressedSize(in.size());
  if (!bound) bound = in.size() + 1024;
  size_t at = out.size();
  out.resize(at + 8 + bound);
  size_t n = 0;
  if (!in.empty()) {
    n = bound;
    if (!BrotliEncoderCompress(5, 22, BROTLI_MODE_GENERIC, in.size(),
                               in.data(), &n, out.data() + at + 8))
      die("brotli encode failed");
  }
  out.resize(at + 8 + n);
  uint32_t comp = (uint32_t)n, orig = (uint32_t)in.size();
  memcpy(out.data() + at, &comp, 4);
  memcpy(out.data() + at + 4, &orig, 4);
  return n;
}

static std::vector<uint8_t> brotli_decompress_frame(Reader& r) {
#ifdef SLPZ_FUZZ_STUB_SLOW
  return fuzz_load_frame(r);
#endif
  uint32_t comp = r.u32(), orig = r.u32();
  auto src = r.bytes(comp);
  if (r.fail) die("corrupt archive (brotli frame)");
  if (orig == 0) return {};
  std::vector<uint8_t> out(orig);
  size_t n = orig;
  if (BrotliDecoderDecompress(comp, src.data(), &n, out.data()) !=
          BROTLI_DECODER_RESULT_SUCCESS ||
      n != orig)
    die("corrupt archive (brotli payload)");
  return out;
}

static void codec_compress_frame(Zctx& z, std::span<const uint8_t> in,
                                 uint8_t id, int level, Buf& out) {
  if (id == kCustom && g_custom)
    custom_frame(in, out, slpz_custom::Generic, level);
  else if (id == kBrotli)
    brotli_compress_frame(in, out);
  else if (id == 0) z_compress(z, in, level, out);
  else lzma_compress_frame(in, id, out);
}

static std::vector<uint8_t> codec_decompress_frame(Zctx& z, Reader& r, uint8_t id) {
  if (id == kCustom && g_custom) return custom_unframe(r);
  if (id == kBrotli) return brotli_decompress_frame(r);
  if (id == 0) return z_decompress(z, r);
  if (id == kLzmaFast || id == kLzmaNormal || id == kLzmaLite || id == kLzmaMax)
    return lzma_decompress_frame(r, id);
  die("corrupt archive (codec id)");
}

// frame layout: u32 compLen, u32 origLen, bytes. Returns compressed size.
static size_t z_compress(Zctx& z, std::span<const uint8_t> in, int level, Buf& out) {
  if (g_custom) return custom_frame(in, out, slpz_custom::Generic, level);
  size_t bound = ZSTD_compressBound(in.size());
  size_t at = out.size();
  out.resize(at + 8 + bound);
  size_t n = ZSTD_compressCCtx(z.c, out.data() + at + 8, bound,
                               in.data(), in.size(), level);
  if (ZSTD_isError(n)) die(std::string("zstd: ") + ZSTD_getErrorName(n));
  out.resize(at + 8 + n);
  uint32_t comp = (uint32_t)n, orig = (uint32_t)in.size();
  memcpy(out.data() + at, &comp, 4);
  memcpy(out.data() + at + 4, &orig, 4);
  return n;
}

static std::vector<uint8_t> z_decompress(Zctx& z, Reader& r) {
  if (g_custom) return custom_unframe(r);
  uint32_t comp = r.u32(), orig = r.u32();
  auto src = r.bytes(comp);
  if (r.fail) die("corrupt archive (zstd frame header)");
  std::vector<uint8_t> out(orig);
  size_t n = ZSTD_decompressDCtx(z.d, out.data(), orig, src.data(), comp);
  if (ZSTD_isError(n) || n != orig) die("corrupt archive (zstd payload)");
  return out;
}

// ── NEON kernels ────────────────────────────────────────────────────────────
// 16x16 byte tile transpose: dst row k = column k of the 16 src rows.
static inline void tile16(const uint8_t* src, size_t sStride,
                          uint8_t* dst, size_t dStride) {
#if SLPZ_NEON
  uint8x16_t x[16];
  for (int k = 0; k < 16; k++) x[k] = vld1q_u8(src + (size_t)k * sStride);
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
  for (int k = 0; k < 16; k++) vst1q_u8(dst + (size_t)k * dStride, x[k]);
#else
  for (size_t i = 0; i < 16; i++)
    for (size_t j = 0; j < 16; j++)
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
  for (uint32_t j = jTiles; j < size; j++)
    for (uint32_t i = 0; i < count; i++)
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
  for (uint32_t j = jTiles; j < size; j++)
    for (uint32_t i = 0; i < count; i++)
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

// returns transform id; *estBits = post-transform order-0 bits/byte estimate
static uint8_t choose_transform(const uint8_t* plane, uint32_t count,
                                float* estBits) {
  // constant plane: zstd RLEs it as-is; skip transforms entirely
  uint32_t i = 1;
  const uint8_t b0 = plane[0];
  for (; i < count && plane[i] == b0; i++) {}
  if (i == count) { *estBits = 0.f; return T_PLAIN; }

  uint32_t hist[3][256] = {};
  uint32_t m = 0;
  const uint32_t kWin = 4096;
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
  *estBits = std::log2f((float)m) - score[t] / (float)m;
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
  uint32_t n = 0;
  int find_or_insert(uint32_t k) {
    uint32_t h = (k * 2654435761u) >> 22;
    for (;; h = (h + 1) & (N - 1)) {
      if (!used[h]) {
        if (n >= 256) return -1;
        used[h] = 1; keys[h] = k; idx[h] = (uint8_t)n++;
        return idx[h];
      }
      if (keys[h] == k) return idx[h];
    }
  }
};

struct DictField {
  uint16_t offset;
  uint8_t width;               // 2 or 4 bytes
  std::vector<uint32_t> vals;  // LE-packed field bytes, first-appearance order
  Buf idx;                     // index plane (count bytes)
};

// transpose + field dictionarization (4- then 2-byte) + per-plane transform
// + entropy stage with per-plane codec bucketing.
// Group payload layout (format v8):
//   u8 nFields, per field: u8 width, u16 offset, u8 nVals-1, nVals×width
//   selector byte per OUTPUT plane (bits 0-1 transform, bit 2 cheap bucket)
//   u8 layout: 0 = u8 codec + frame(all planes)
//              1 = frame zstd(cheap planes) + u8 codec + frame(rich planes)
// "Cheap" planes (probe estimate <= kCheapBits bits/byte — runs, constants,
// the zeroed prediction spans) go to zstd at ~15x LZMA's speed; the entropy
// stage only sees planes that pay for it.
constexpr float kCheapBits = 0.30f;

static void pack_records(Zctx& z, std::span<const uint8_t> recs, uint32_t count, uint32_t size,
                         int level, Buf& blob, std::atomic<uint64_t>* stat, bool allowLzma = true,
                         uint8_t cmd = 0) {
  size_t blobAt = blob.size();
  Buf planes;
  transpose_fwd(recs, count, size, planes);

  std::vector<DictField> fields;
  std::vector<uint8_t> widthAt(size, 0);  // field width at start offset
  if (count >= 64) {
    std::vector<uint8_t> covered(size, 0);
    Buf idxPlane;
    for (uint8_t width : {(uint8_t)4, (uint8_t)2}) {
      for (uint32_t w = 0; w + width <= size;) {
        bool free = true;
        for (uint32_t b = 0; b < width; b++) free = free && !covered[w + b];
        if (!free) { w++; continue; }
        U32Set set;
        idxPlane.clear();
        idxPlane.resize(count);
        bool ok = true;
        for (uint32_t i = 0; i < count; i++) {
          uint32_t v = 0;
          memcpy(&v, recs.data() + (size_t)i * size + w, width);
          int id = set.find_or_insert(v);
          if (id < 0) { ok = false; break; }
          idxPlane[i] = (uint8_t)id;
        }
        // The field count ships as one byte, so 255 is a hard ceiling. Records
        // wide enough to exceed it exist (cmd 0x10 is 516 bytes = up to 258
        // two-byte fields), and whether they do depends on the pooled record
        // count, not the record itself: acceptance loosens as `count` grows, so
        // a corpus large enough to pool many 0x10 records tips over. The 4-byte
        // pass runs first, so capping here keeps the highest-value fields.
        if (ok && fields.size() < 255 &&
            set.n * width <= (size_t)count * (width / 2)) {
          DictField f{(uint16_t)w, width, {}, {}};
          f.vals.resize(set.n);
          for (uint32_t h = 0; h < U32Set::N; h++)
            if (set.used[h]) f.vals[set.idx[h]] = set.keys[h];
          f.idx = idxPlane;
          fields.push_back(std::move(f));
          widthAt[w] = width;
          for (uint32_t b = 0; b < width; b++) covered[w + b] = 1;
          w += width;
        } else {
          w += 1;
        }
      }
    }
    std::sort(fields.begin(), fields.end(),
              [](const DictField& a, const DictField& b) { return a.offset < b.offset; });
  }

  uint32_t nOut = size;
  for (auto& f : fields) nOut -= f.width - 1;
  Buf out;
  out.reserve((size_t)nOut * count);
  {
    size_t fi = 0;
    for (uint32_t o = 0; o < size;) {
      if (widthAt[o]) {
        out.put_bytes(fields[fi].idx.data(), count);
        o += fields[fi].width;
        fi++;
      } else {
        out.put_bytes(planes.data() + (size_t)o * count, count);
        o += 1;
      }
    }
  }

  if (fields.size() > 255) die("internal: dict field count overflow");
  blob.put_u8((uint8_t)fields.size());
  for (auto& f : fields) {
    blob.put_u8(f.width);
    blob.put_u8((uint8_t)(f.offset & 0xFF));
    blob.put_u8((uint8_t)(f.offset >> 8));
    blob.put_u8((uint8_t)(f.vals.size() - 1));
    for (uint32_t v : f.vals) blob.put_bytes(&v, f.width);
  }
  std::vector<uint8_t> profiles;
  if (g_custom) {
    for (uint32_t offset = 0; offset < size;) {
      profiles.push_back(field_profile(cmd, offset, widthAt[offset] != 0));
      offset += widthAt[offset] ? widthAt[offset] : 1;
    }
  }
  size_t selAt = blob.size();
  blob.resize(selAt + nOut);
  size_t cheapBytes = 0;
  for (uint32_t j = 0; j < nOut; j++) {
    uint8_t* pl = out.data() + (size_t)j * count;
    float est = 8.f;
    uint8_t t = count >= 8 ? choose_transform(pl, count, &est) : T_PLAIN;
    if (t == T_SUB) delta_fwd<false>(pl, count);
    else if (t == T_XOR) delta_fwd<true>(pl, count);
    if (est <= kCheapBits) {
      t |= 4;
      cheapBytes += count;
    }
    blob[selAt + j] = t;
  }

  if (g_custom) {
    for (uint32_t j = 0; j < nOut; j++) blob[selAt + j] &= 3;
    blob.put_u8(2);
    custom_planes_frame(out, count, profiles, level, blob);
    if (g_stats.enabled && stat) *stat += blob.size() - blobAt;
    return;
  }
  uint8_t altId = codec_id_for_level(level);
  size_t total = out.size();
  bool split = allowLzma && cheapBytes >= 4096 && total - cheapBytes >= 4096 &&
               cheapBytes * 8 >= total;
  Buf whole;
  z_compress(z, out, level, whole);
  Buf splitBuf;
  if (split) {
    Buf cheap, rich;
    cheap.reserve(cheapBytes);
    rich.reserve(total - cheapBytes);
    for (uint32_t j = 0; j < nOut; j++)
      (blob[selAt + j] & 4 ? cheap : rich)
          .put_bytes(out.data() + (size_t)j * count, count);
    z_compress(z, cheap, level, splitBuf);
    splitBuf.put_u8(altId);
    codec_compress_frame(z, rich, altId, level, splitBuf);
  }
  bool useSplit = split && splitBuf.size() + 1 < whole.size() + 2;
  if (useSplit) {
    blob.put_u8(1);
    blob.put_bytes(splitBuf.data(), splitBuf.size());
    if (g_stats.enabled) g_stats.lzmaWin++;
  } else {
    Buf alt;
    if (allowLzma && !split && total >= 4096)
      codec_compress_frame(z, out, altId, level, alt);
    bool useAlt = !alt.empty() && alt.size() < whole.size();
    if (g_stats.enabled && !alt.empty())
      (useAlt ? g_stats.lzmaWin : g_stats.zstdWin)++;
    for (uint32_t j = 0; j < nOut; j++) blob[selAt + j] &= 3;
    blob.put_u8(0);
    blob.put_u8(useAlt ? altId : 0);
    const Buf& win = useAlt ? alt : whole;
    blob.put_bytes(win.data(), win.size());
  }
  if (g_stats.enabled && stat) *stat += blob.size() - blobAt;
}

static void unpack_records(Zctx& z, Reader& r, uint32_t count, uint32_t size,
                           std::vector<uint8_t>& records) {
  uint8_t nFields = r.u8();
  std::vector<DictField> fields(nFields);
  uint32_t nOut = size;
  for (auto& f : fields) {
    f.width = r.u8();
    f.offset = r.u8();
    f.offset |= (uint16_t)r.u8() << 8;
    uint32_t nVals = (uint32_t)r.u8() + 1;
    if ((f.width != 2 && f.width != 4) || f.offset + f.width > size)
      die("corrupt archive (dict field)");
    auto raw = r.bytes(nVals * f.width);
    if (r.fail) die("corrupt archive (dict field)");
    f.vals.resize(nVals);
    for (uint32_t v = 0; v < nVals; v++)
      memcpy(&f.vals[v], raw.data() + (size_t)v * f.width, f.width);
    if (nOut < f.width) die("corrupt archive (dict count)");
    nOut -= f.width - 1;
  }
  auto sel = r.bytes(nOut);
  uint8_t layout = r.u8();
  if (r.fail) die("corrupt archive (plane selectors)");

  std::vector<uint8_t> out;
  if (layout == 0) {
    uint8_t codec = r.u8();
    out = codec_decompress_frame(z, r, codec);
  } else if (layout == 1) {
    auto cheap = z_decompress(z, r);
    uint8_t codec = r.u8();
    auto rich = codec_decompress_frame(z, r, codec);
    if (cheap.size() + rich.size() != (size_t)count * nOut)
      die("corrupt archive (bucket sizes)");
    out.resize((size_t)count * nOut);
    size_t cAt = 0, rAt = 0;
    for (uint32_t j = 0; j < nOut; j++) {
      const bool ch = sel[j] & 4;
      auto& srcb = ch ? cheap : rich;
      size_t& at = ch ? cAt : rAt;
      if (at + count > srcb.size()) die("corrupt archive (bucket underrun)");
      memcpy(out.data() + (size_t)j * count, srcb.data() + at, count);
      at += count;
    }
  } else if (layout == 2 && g_custom) {
    out = custom_unframe(r, true);
  } else {
    die("corrupt archive (layout id)");
  }
  if (out.size() != (size_t)count * nOut) die("corrupt archive (stream size)");

  for (uint32_t j = 0; j < nOut; j++) {
    uint8_t* pl = out.data() + (size_t)j * count;
    uint8_t t = sel[j] & 3;
    if (t == T_SUB) scan_rev<false>(pl, count);
    else if (t == T_XOR) scan_rev<true>(pl, count);
  }

  // expand output planes (+dictionary fields) back into full byte planes
  std::vector<uint8_t> planes((size_t)count * size);
  std::vector<const DictField*> fieldAt(size, nullptr);
  for (auto& f : fields) {
    if (fieldAt[f.offset]) die("corrupt archive (dict overlap)");
    fieldAt[f.offset] = &f;
  }
  size_t srcAt = 0;
  for (uint32_t o = 0; o < size;) {
    if (srcAt + count > out.size()) die("corrupt archive (plane underrun)");
    const uint8_t* pl = out.data() + srcAt;
    if (fieldAt[o]) {
      const auto& f = *fieldAt[o];
      for (uint32_t i = 0; i < count; i++) {
        if (pl[i] >= f.vals.size()) die("corrupt archive (dict index)");
        uint32_t v = f.vals[pl[i]];
        for (uint32_t b = 0; b < f.width; b++)
          planes[(size_t)(o + b) * count + i] = (uint8_t)(v >> (8 * b));
      }
      o += f.width;
    } else {
      memcpy(planes.data() + (size_t)o * count, pl, count);
      o += 1;
    }
    srcAt += count;
  }
  records.resize(planes.size());
  transpose_rev(planes, count, size, records.data());
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
static uint32_t cross_pred_xor(uint8_t* pre, uint32_t preCount, uint32_t preSize,
                               const uint8_t* post, uint32_t postCount,
                               uint32_t postSize, bool probe, uint32_t probeMax,
                               uint32_t* pairsOut) {
  static thread_local std::vector<int32_t> keyIdx;
  static thread_local std::vector<uint32_t> occ;
  keyIdx.assign(65536, -1);
  occ.assign(65536, 0);
  std::vector<std::vector<const uint8_t*>> lists;
  for (uint32_t i = 0; i < postCount; i++) {
    const uint8_t* q = post + (size_t)i * postSize;
    uint16_t k = (uint16_t)(q[4] << 8 | q[5]);
    if (keyIdx[k] < 0) {
      keyIdx[k] = (int32_t)lists.size();
      lists.emplace_back();
    }
    lists[(size_t)keyIdx[k]].push_back(q);
  }
  uint32_t pairs = 0, hits = 0;
  uint32_t limit = probe ? std::min(preCount, probeMax) : preCount;
  for (uint32_t i = 0; i < limit; i++) {
    uint8_t* p = pre + (size_t)i * preSize;
    uint16_t k = (uint16_t)(p[4] << 8 | p[5]);
    uint32_t o = occ[k]++;
    if (o == 0 || keyIdx[k] < 0) continue;
    auto& L = lists[(size_t)keyIdx[k]];
    if (o - 1 >= L.size()) continue;
    const uint8_t* q = L[o - 1];
    if (probe) {
      pairs++;
      hits += memcmp(p + kPreA, q + kPostA, kLenA) == 0;
      continue;
    }
    for (uint32_t b = 0; b < kLenA; b++) p[kPreA + b] ^= q[kPostA + b];
    for (uint32_t b = 0; b < kLenB; b++) p[kPreB + b] ^= q[kPostB + b];
  }
  if (pairsOut) *pairsOut = pairs;
  return hits;
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
// large opaque blobs (appended gecko codesets etc.) are 4-byte-aligned PPC
// code — run them through the plane machinery as 4-byte records
static void pack_blob(Zctx& z, std::span<const uint8_t> data, int level,
                      Buf& blob, std::atomic<uint64_t>& stat) {
  size_t before = blob.size();
  if (g_custom) {
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
    return;
  }
  if (data.size() >= 65536) {
    // try flat zstd vs flat LZMA vs 4-byte-record planes; keep the smallest
    // (LZMA flat usually wins on the appended PPC/gecko code dumps)
    Buf flat, flatL, packed;
    z_compress(z, data, level, flat);
    lzma_compress_frame(data, level <= 8 ? kLzmaFast : kLzmaMax, flatL);
    uint32_t n4 = (uint32_t)(data.size() / 4);
    packed.put_u32(n4);
    // flat-lzma candidate already covers lzma; avoid double lzma work here
    pack_records(z, data.first((size_t)n4 * 4), n4, 4, level, packed, nullptr,
                 /*allowLzma=*/false);
    z_compress(z, data.subspan((size_t)n4 * 4), level, packed);
    Buf ppc;
    {
      std::vector<uint8_t> xf(data.begin(), data.end());
      ppc_branch_xform(xf.data(), xf.size() / 4, false);
      lzma_compress_frame(xf, level <= 8 ? kLzmaFast : kLzmaMax, ppc);
    }
    const Buf* best = &flat;
    uint8_t mode = 0;
    if (flatL.size() < best->size()) { best = &flatL; mode = 2; }
    if (packed.size() < best->size()) { best = &packed; mode = 1; }
    if (ppc.size() < best->size()) { best = &ppc; mode = 4; }
    blob.put_u8(mode);
    if (mode == 2 || mode == 4) blob.put_u8(level <= 8 ? kLzmaFast : kLzmaMax);
    blob.put_bytes(best->data(), best->size());
  } else {
    blob.put_u8(0);
    z_compress(z, data, level, blob);
  }
  if (g_stats.enabled) stat += blob.size() - before;
}

// decodes blob modes 0 (flat zstd), 1 (4-byte-record planes), 2 (flat codec);
// mode 3 (chunked) is handled by the scan/task machinery below
static std::vector<uint8_t> unpack_blob(Zctx& z, Reader& r) {
  uint8_t mode = r.u8();
  if (r.fail) die("corrupt archive (blob header)");
  if (mode == 0) return z_decompress(z, r);
  if (mode == 2 || mode == 4) {
    uint8_t id = r.u8();
    auto out = codec_decompress_frame(z, r, id);
    if (mode == 4) ppc_branch_xform(out.data(), out.size() / 4, true);
    return out;
  }
  if (mode != 1) die("corrupt archive (blob mode)");
  uint32_t n4 = r.u32();
  std::vector<uint8_t> out;
  unpack_records(z, r, n4, 4, out);
  auto rem = z_decompress(z, r);
  out.insert(out.end(), rem.begin(), rem.end());
  return out;
}

// ── generic worker pool ─────────────────────────────────────────────────────
template <typename Fn>
static void parallel_for(size_t n, int threads, Fn work) {
  std::atomic<size_t> next{0};
  auto worker = [&] {
    Zctx z;
    for (size_t i; (i = next.fetch_add(1)) < n;) work(z, i);
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
// group-sized (~a few MB), and huge blobs are chunked (mode 3, 4 MB chunks).
struct FilePlan;

struct PackTask {
  enum Kind : uint8_t { Ungrouped, Group, BlobFrame, BlobTrial, RawFile };
  FilePlan* fp;
  uint32_t piece;
  Kind kind;
  uint32_t stream = 0, groupIdx = 0;
  std::span<const uint8_t> data;  // BlobFrame / BlobTrial / RawFile
  uint8_t codecId = 0;            // BlobFrame
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

static void plan_blob(Zctx& z, FilePlan& fp, std::span<const uint8_t> data,
                      int level, std::atomic<uint64_t>* stat) {
  constexpr size_t kChunk = 4u << 20;
  if (data.size() < 65536) {
    Buf& b = fp.live();
    size_t at = b.size();
    b.put_u8(0);
    z_compress(z, data, level, b);
    if (g_stats.enabled) *stat += b.size() - at;
  } else if (data.size() < kChunk) {
    fp.tasks.push_back({&fp, fp.defer(), PackTask::BlobTrial, 0, 0, data, 0,
                        stat, data.size()});
  } else {
    uint8_t id = codec_id_for_level(level);
    // decide the PPC transform once on a 256KB sample
    uint8_t xform = 0;
    {
      auto sample = data.first(std::min<size_t>(data.size(), 256 << 10));
      Buf a, bx;
      lzma_compress_frame(sample, kLzmaFast, a);
      std::vector<uint8_t> xf(sample.begin(), sample.end());
      ppc_branch_xform(xf.data(), xf.size() / 4, false);
      lzma_compress_frame(xf, kLzmaFast, bx);
      xform = bx.size() < a.size() ? 1 : 0;
    }
    uint32_t n = (uint32_t)((data.size() + kChunk - 1) / kChunk);
    Buf& b = fp.live();
    b.put_u8(3);
    b.put_u8(id);
    b.put_u8(xform);
    b.put_u32(n);
    if (g_stats.enabled) *stat += 7;
    for (uint32_t i = 0; i < n; i++) {
      size_t off = (size_t)i * kChunk;
      auto chunk = data.subspan(off, std::min(kChunk, data.size() - off));
      fp.tasks.push_back({&fp, fp.defer(), PackTask::BlobFrame, 0, xform, chunk,
                          id, stat, chunk.size()});
    }
  }
}

static void plan_compress(Zctx& z, FilePlan& fp, int level) {
  fp.data = read_file(fp.path);
  fp.origSize = fp.data.size();
  if (!parse_slp(fp.data, fp.slp)) {
    fp.live().put_u8(kModeRaw);
    fp.tasks.push_back({&fp, fp.defer(), PackTask::RawFile, 0, 0,
                        std::span<const uint8_t>(fp.data), 0, &g_stats.misc,
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
  plan_blob(z, fp, fp.slp.suffix, level, &g_stats.suffix);
  plan_blob(z, fp, fp.slp.tail, level, &g_stats.tail);
  {
    Buf& b = fp.live();
    size_t before = b.size();
    small_compress(z, fp.slp.order, level, b, slpz_custom::Discrete);
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
    if (g_tuned && grouped) {
      if (st.cmd == 0x37 && st.recSize >= 59) flags |= 32;
      if (st.cmd == 0x38 && st.recSize >= 72) flags |= 16;
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
        small_compress(z, seedStream, level, b, slpz_custom::Rng);
        if (g_stats.enabled) g_stats.seeds += b.size() - before;
      }
    }
    if (!grouped) {
      fp.tasks.push_back({&fp, fp.defer(), PackTask::Ungrouped, s, 0, {}, 0,
                          stat, st.records.size()});
      continue;
    }
    {
      Buf& b = fp.live();
      size_t before = b.size();
      small_compress(z, selector, level, b, slpz_custom::Discrete);
      if (g_stats.enabled) g_stats.selector += b.size() - before;
      b.put_u8((uint8_t)keys.size());
    }
    for (uint32_t g = 0; g < keys.size(); g++)
      fp.tasks.push_back({&fp, fp.defer(), PackTask::Group, s, g, {}, 0, stat,
                          (size_t)counts[g] * st.recSize});
  }
}

// v10 predictors use IEEE binary32 round-to-nearest operations. Volatile
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
static void motion_predict(uint8_t* data, uint32_t count, uint32_t size, bool inverse) {
  if (size < 72) return;
  float prev[2] = {};
  for (uint32_t i = 0; i < count; i++) {
    uint8_t* r = data + (size_t)i * size;
    float pred[2] = {};
    {
      volatile float vx = read_float(r + (r[46] ? 52 : 68)) + read_float(r + 60);
      volatile float x = prev[0] + vx;
      pred[0] = std::isfinite(x) ? x : 0.f;
      volatile float vy = read_float(r + 56) + read_float(r + 64);
      volatile float y = prev[1] + vy;
      pred[1] = std::isfinite(y) ? y : 0.f;
    }
    for (uint32_t j = 0; j < 2; j++) {
      uint8_t* f = r + 9 + 4 * j;
      uint32_t pb;
      memcpy(&pb, pred + j, 4);
      uint32_t v = be32(f), decoded = inverse ? v ^ pb : v;
      uint32_t residual = v ^ pb;
      f[0] = (uint8_t)(residual >> 24);
      f[1] = (uint8_t)(residual >> 16);
      f[2] = (uint8_t)(residual >> 8);
      f[3] = (uint8_t)residual;
      memcpy(prev + j, &decoded, 4);
    }
  }
}
static void input_predict(uint8_t* data, uint32_t count, uint32_t size, bool inverse) {
  if (size < 59) return;
  uint32_t table[4][256] = {};
  const uint32_t offsets[4] = {24, 28, 32, 36}, raw[4] = {58, 63, 64, 65};
  for (uint32_t i = 0; i < count; i++) {
    uint8_t* r = data + (size_t)i * size;
    // Processed and physical button copies; float trigger vs max(L,R).
    r[46] ^= r[48];
    r[47] ^= r[49];
    const uint8_t* pred = be32(r + 50) >= be32(r + 54) ? r + 50 : r + 54;
    for (uint32_t j = 0; j < 4; j++) r[40 + j] ^= pred[j];
    for (uint32_t j = 0; j < 4; j++) {
      if (raw[j] >= size) continue;
      uint32_t v;
      memcpy(&v, r + offsets[j], 4);
      uint8_t key = r[raw[j]];
      uint32_t decoded = inverse ? v ^ table[j][key] : v;
      v ^= table[j][key];
      table[j][key] = decoded;
      memcpy(r + offsets[j], &v, 4);
    }
  }
}
static void run_pack_task(Zctx& z, PackTask& t, int level) {
  Buf& out = t.fp->pieces[t.piece];
  switch (t.kind) {
    case PackTask::RawFile: {
      size_t n = z_compress(z, t.data, level, out);
      if (g_stats.enabled) *t.stat += n;
      break;
    }
    case PackTask::BlobTrial:
      pack_blob(z, t.data, level, out, *t.stat);
      break;
    case PackTask::BlobFrame: {
      size_t at = out.size();
      if (t.groupIdx) {  // PPC transform, chunk-local word indices
        std::vector<uint8_t> xf(t.data.begin(), t.data.end());
        ppc_branch_xform(xf.data(), xf.size() / 4, false);
        codec_compress_frame(z, xf, t.codecId, level, out);
      } else {
        codec_compress_frame(z, t.data, t.codecId, level, out);
      }
      if (g_stats.enabled) *t.stat += out.size() - at;
      break;
    }
    case PackTask::Ungrouped: {
      auto& st = t.fp->slp.streams[t.stream];
      pack_records(z, st.records, st.count, st.recSize, level, out, t.stat, true, st.cmd);
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
      if (g_tuned && st.cmd == 0x37) input_predict(group.data(), cnt, st.recSize, false);
      if (g_tuned && st.cmd == 0x38) motion_predict(group.data(), cnt, st.recSize, false);
      pack_records(z, group, cnt, st.recSize, level, out, t.stat, true, st.cmd);
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
    auto worker = [&](Zctx& z) {
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
          run_pack_task(z, *t, level);
          if (t->fp->pending.fetch_sub(1) == 1) retire(*t->fp);
        } else if (fi != SIZE_MAX) {
          plan_compress(z, plans[fi], level);
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
      pool.emplace_back([&] { Zctx z; worker(z); });
    { Zctx z; worker(z); }
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
  parallel_for(pools.size(), threads, [&](Zctx& z, size_t i) {
    auto& pl = pools[i];
    pack_records(z, pl.records, pl.total, pl.recSize, level, pl.packed,
                 g_stats.enabled ? &g_stats.byCmd[pl.cmd] : nullptr, true, pl.cmd);
    pl.records = Buf();
  });
  lap("pools");

  Buf out;
  out.put_u32(kMagic);
  out.put_u8(g_tuned ? kCustomVersion : kVersion);
  out.put_u8(g_custom ? 1 : 0);  // flags
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
    fprintf(stderr, "    alt codec: won %llu / lost %llu group trials vs zstd\n",
            (unsigned long long)g_stats.lzmaWin.load(),
            (unsigned long long)g_stats.zstdWin.load());
  }
}

// ── decompress: 3 phases ────────────────────────────────────────────────────
// Phase 1 (parallel per file): walk the blob structure, decode the small side
// streams inline, and record byte spans for every packed section / blob frame.
// Phase 2 (parallel per task, largest first): decode spans into records.
// Phase 3 (parallel per file): scatter groups, unfold seeds, rebuild the
// original bytes, verify size, write.
struct BlobPlan {
  uint8_t mode = 0, codec = 0, xform = 0;
  std::span<const uint8_t> span;                 // modes 0/1/2, incl. mode byte
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
  std::vector<std::vector<uint8_t>> recs;       // task outputs
  std::vector<uint8_t> records;                 // event-order (phase 3)
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
  std::vector<uint8_t> outData;
  std::atomic<uint32_t> pending{0};
  void release() {
    // note: `vec = {}` binds the initializer_list overload — it empties the
    // vector but KEEPS capacity; swap with a temporary actually deallocates
    std::vector<uint8_t>().swap(outData);
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
  std::vector<uint8_t> records;
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
  for (uint8_t f = 0; f < nFields; f++) {
    uint8_t width = r.u8();
    r.u8();
    r.u8();
    uint32_t nVals = (uint32_t)r.u8() + 1;
    r.bytes(nVals * width);
    if ((width != 2 && width != 4) || nOut < width) { r.fail = true; return {}; }
    nOut -= width - 1;
  }
  r.bytes(nOut);          // transform selectors
  uint8_t layout = r.u8();
  if (layout == 0) {
    r.u8();  // codec
    skip_frame(r);
  } else if (layout == 1) {
    skip_frame(r);  // cheap bucket (zstd)
    r.u8();         // codec
    skip_frame(r);  // rich bucket
  } else if (layout == 2 && g_custom) {
    skip_frame(r);
  } else {
    r.fail = true;
  }
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
  } else if (bp.mode == 1) {
    r.u32();  // n4
    skip_packed(r, 4);
    skip_frame(r);  // remainder
    bp.span = {start, (size_t)(r.p - start)};
  } else if (bp.mode == 3) {
    bp.codec = r.u8();
    bp.xform = r.u8();
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && !r.fail; i++) bp.chunks.push_back(skip_frame(r));
    bp.chunkOut.resize(bp.chunks.size());
  } else {
    r.fail = true;
  }
}

static void scan_decompress(Zctx& z, DecPlan& dp) {
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
  dp.order = small_decompress(z, r);
  uint8_t nStreams = r.u8();
  dp.streams.resize(nStreams);
  for (auto& st : dp.streams) {
    st.cmd = r.u8();
    st.recSize = r.u32();
    st.count = r.u32();
    st.flags = r.u8();
    if (r.fail) die("corrupt archive (stream header)");
    if (st.flags & 8) continue;  // pooled: records live in the shared pool
    if (st.flags & 2) st.seedStream = small_decompress(z, r);
    if (!(st.flags & 1)) {
      st.spans.push_back(skip_packed(r, st.recSize));
      st.gCounts.push_back(st.count);
    } else {
      st.selector = small_decompress(z, r);
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

static void run_dec_task(Zctx& z, DecTask& t) {
  switch (t.kind) {
    case DecTask::Raw: {
      Reader r{t.dp->rawFrame.data(), t.dp->rawFrame.data() + t.dp->rawFrame.size()};
      t.dp->outData = z_decompress(z, r);
      break;
    }
    case DecTask::Blob: {
      Reader r{t.bp->span.data(), t.bp->span.data() + t.bp->span.size()};
      t.bp->out = unpack_blob(z, r);
      break;
    }
    case DecTask::BlobChunk: {
      auto& c = t.bp->chunks[t.idx];
      Reader r{c.data(), c.data() + c.size()};
      auto& out = t.bp->chunkOut[t.idx];
      out = codec_decompress_frame(z, r, t.bp->codec);
      if (t.bp->xform) ppc_branch_xform(out.data(), out.size() / 4, true);
      break;
    }
    case DecTask::Stream: {
      auto& st = t.dp->streams[t.stream];
      auto& sp = st.spans[t.idx];
      Reader r{sp.data(), sp.data() + sp.size()};
      unpack_records(z, r, st.gCounts[t.idx], st.recSize, st.recs[t.idx]);
      if (st.flags & 16) {
        if (!g_tuned || st.cmd != 0x38 || !(st.flags & 1) || st.recSize < 72)
          die("corrupt motion predictor flag");
        motion_predict(st.recs[t.idx].data(), st.gCounts[t.idx], st.recSize, true);
      }
      if (st.flags & 32) {
        if (!g_tuned || st.cmd != 0x37 || !(st.flags & 1) || st.recSize < 59)
          die("corrupt input predictor flag");
        input_predict(st.recs[t.idx].data(), st.gCounts[t.idx], st.recSize, true);
      }
      break;
    }
    case DecTask::Pool: {
      Reader r{t.pool->span.data(), t.pool->span.data() + t.pool->span.size()};
      unpack_records(z, r, t.pool->total, t.pool->recSize, t.pool->records);
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

static void assemble_decompress(DecPlan& dp, std::vector<DecPool>& pools) {
  if (dp.raw) {
    if (dp.outData.size() != dp.origSize)
      die("size mismatch for " + dp.storedName);
    return;
  }
  finish_blob(dp.suffix);
  finish_blob(dp.tail);

  int16_t idxOf[256];
  for (auto& i : idxOf) i = -1;
  struct Cursor { const uint8_t* next; uint32_t recSize; };
  Cursor cursors[256];
  for (size_t s = 0; s < dp.streams.size(); s++) {
    auto& st = dp.streams[s];
    if (st.flags & 8) {
      if (st.poolIdx < 0) die("corrupt archive (pool ref)");
      auto& pl = pools[(size_t)st.poolIdx];
      const uint8_t* from = pl.records.data() + st.poolOff * st.recSize;
      st.records.assign(from, from + (size_t)st.count * st.recSize);
    } else if (!(st.flags & 1)) {
      st.records = std::move(st.recs[0]);
    } else {
      st.records.resize((size_t)st.recSize * st.count);
      std::vector<const uint8_t*> srcs(st.recs.size());
      for (size_t g = 0; g < st.recs.size(); g++) {
        if (st.recs[g].size() != (size_t)st.gCounts[g] * st.recSize)
          die("corrupt archive (group size)");
        srcs[g] = st.recs[g].data();
      }
      for (uint32_t i = 0; i < st.count; i++) {
        uint8_t g = st.selector[i];
        if (g >= srcs.size()) die("corrupt archive (selector range)");
        memcpy(st.records.data() + (size_t)i * st.recSize, srcs[g], st.recSize);
        srcs[g] += st.recSize;
      }
    }
    st.recs.clear();
    st.recs.shrink_to_fit();
    if (st.flags & 2) {  // re-derive folded seeds (stream is group-ordered)
      uint32_t seedOff = seed_offset_for(st.cmd, st.recSize);
      if (!seedOff) die("corrupt archive (seed flag on seedless cmd)");
      bool grouped = st.flags & 1;
      uint32_t nG = grouped ? (uint32_t)st.gCounts.size() : 1;
      std::vector<size_t> cur(nG);
      {
        size_t pos = 0;
        bool fail = false;
        for (uint32_t g = 0; g < nG; g++) {
          cur[g] = pos;
          uint32_t cnt = grouped ? st.gCounts[g] : st.count;
          for (uint32_t i = 0; i < cnt; i++) {
            uint32_t k = get_varint(st.seedStream, pos, fail);
            if (k == kSeedException) pos += 4;
            if (fail || pos > st.seedStream.size())
              die("corrupt archive (seed stream)");
          }
        }
      }
      bool fail = false;
      uint32_t seed = 0;
      for (uint32_t i = 0; i < st.count; i++) {
        uint32_t g = grouped ? st.selector[i] : 0;
        if (g >= nG) die("corrupt archive (seed group)");
        uint32_t k = get_varint(st.seedStream, cur[g], fail);
        if (k == kSeedException) {
          if (cur[g] + 4 > st.seedStream.size()) fail = true;
          else { memcpy(&seed, st.seedStream.data() + cur[g], 4); cur[g] += 4; }
        } else {
          for (uint32_t t = 0; t < k; t++) seed = lcg_next(seed);
        }
        if (fail) die("corrupt archive (seed stream)");
        uint8_t* rec = st.records.data() + (size_t)i * st.recSize;
        rec[seedOff] = (uint8_t)(seed >> 24);
        rec[seedOff + 1] = (uint8_t)(seed >> 16);
        rec[seedOff + 2] = (uint8_t)(seed >> 8);
        rec[seedOff + 3] = (uint8_t)seed;
      }
    }
    idxOf[st.cmd] = (int16_t)s;
    cursors[s] = {st.records.data(), st.recSize};
  }
  for (auto& st : dp.streams) {
    if (!(st.flags & 4)) continue;
    if (st.cmd != 0x37 || idxOf[0x38] < 0) die("corrupt archive (xpred flag)");
    auto& post = dp.streams[(size_t)idxOf[0x38]];
    if (!cross_pred_eligible(st.recSize, st.count, post.recSize, post.count))
      die("corrupt archive (xpred shape)");
    cross_pred_xor(st.records.data(), st.count, st.recSize, post.records.data(),
                   post.count, post.recSize, /*probe=*/false, 0, nullptr);
  }

  auto& out = dp.outData;
  size_t rawSize = 1 + dp.ep.size() + dp.tail.out.size();
  for (auto& st : dp.streams)
    rawSize += (size_t)st.recSize * st.count + st.count;
  out.reserve(kSlpHeaderLen + rawSize + dp.suffix.out.size());
  out.insert(out.end(), dp.prefix.begin(), dp.prefix.end());
  out.push_back(0x35);
  out.insert(out.end(), dp.ep.begin(), dp.ep.end());
  for (uint8_t cmd : dp.order) {
    int16_t s = idxOf[cmd];
    if (s < 0) die("corrupt archive (order stream)");
    auto& c = cursors[(size_t)s];
    out.push_back(cmd);
    out.insert(out.end(), c.next, c.next + c.recSize);
    c.next += c.recSize;
  }
  out.insert(out.end(), dp.tail.out.begin(), dp.tail.out.end());
  out.insert(out.end(), dp.suffix.out.begin(), dp.suffix.out.end());
  if (out.size() != dp.origSize) die("size mismatch for " + dp.storedName);
}

static void cmd_extract(const fs::path& arcPath, const fs::path& outDir,
                        int threads, bool testOnly, bool hashMode = false) {
  auto arc = read_file(arcPath);
  Reader r{arc.data(), arc.data() + arc.size()};
  if (r.u32() != kMagic) die("not a slpz archive");
  uint8_t version = r.u8(), flags = r.u8();
  if (version != kVersion && version != kCustomVersion) die("unsupported slpz version");
  if (flags & ~1u || (version == kVersion && flags)) die("unsupported slpz flags");
  g_tuned = version == kCustomVersion;
  g_custom = g_tuned && (flags & 1);
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
               [&](Zctx& z, size_t i) { scan_decompress(z, plans[i]); });
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
                 [&](Zctx& z, size_t i) { run_dec_task(z, ptasks[i]); });
  }
  lap("pools");

  // dedicated writer threads: decode workers never block on the filesystem,
  // and APFS gets its preferred low write concurrency. Queue is bounded so
  // memory stays proportional to in-flight files.
  struct WriteJob {
    fs::path path;
    std::vector<uint8_t> data;
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
    parallel_for(tasks.size(), threads, [&](Zctx& z, size_t i) {
      auto& t = tasks[i];
      run_dec_task(z, t);
      if (t.kind == DecTask::AssembleOnly) finish(*t.dp);
      else if (t.dp->pending.fetch_sub(1) == 1) finish(*t.dp);
    });
    // Return freed wave buffers to the OS so RSS tracks the bounded live set.
#if defined(__APPLE__)
    malloc_zone_pressure_relief(nullptr, 0);
#else
    malloc_trim(0);
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
    for (uint32_t size : {1u, 2u, 15u, 16u, 33u, 63u, 517u}) {
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
  // brotli frame roundtrip
  for (size_t n : {0u, 1u, 100u, 65536u, 1000001u}) {
    auto orig = rnd(n);
    Buf enc;
    brotli_compress_frame(orig, enc);
    Reader r{enc.data(), enc.data() + enc.size()};
    auto dec = brotli_decompress_frame(r);
    check(dec == orig, "brotli roundtrip", n, 0);
  }
  // lzma raw frame roundtrip
  for (uint8_t id : {kLzmaFast, kLzmaNormal}) {
    for (size_t n : {0u, 1u, 100u, 65536u, 1000001u}) {
      auto orig = rnd(n);
      Buf enc;
      lzma_compress_frame(orig, id, enc);
      Reader r{enc.data(), enc.data() + enc.size()};
      auto dec = lzma_decompress_frame(r, id);
      check(dec == orig, "lzma roundtrip", id, n);
    }
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
    Zctx z;
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
        pack_records(z, recs, count, size, level, blob, nullptr);
        Reader r{blob.data(), blob.data() + blob.size()};
        std::vector<uint8_t> back;
        unpack_records(z, r, count, size, back);
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
      "  slpz c -o out.slpz [--codec legacy|tuned|custom] [-l level] [-T threads] [-v] "
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
      auto codec = next();
      if (codec != "legacy" && codec != "tuned" && codec != "custom")
        die("codec must be legacy, tuned, or custom");
      g_custom = codec == "custom";
      g_tuned = codec != "legacy";
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
