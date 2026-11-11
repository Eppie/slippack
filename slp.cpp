#include "slp.h"
#include "codec_types.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#if defined(__aarch64__) && !defined(SLPZ_CUSTOM_SCALAR)
#include <arm_neon.h>
#define SLPZ_NEON 1
#else
#define SLPZ_NEON 0
#endif

namespace slpz {
std::span<const uint16_t> semantic_fields(uint8_t cmd) {
  static constexpr uint16_t pre[] = {12, 16, 20, 24, 28, 32, 36, 40, 50, 54, 59};
  static constexpr uint16_t post[] = {9, 13, 17, 21, 25, 33, 42, 52, 56, 60, 64, 68, 72, 76, 80};
  if (cmd == 0x37) {
    return pre;
  }
  if (cmd == 0x38) {
    return post;
  }
  return {};
}
uint8_t field_profile(uint8_t cmd, uint32_t offset, bool dictionary) {
  using namespace slpz_custom;
  if (dictionary) {
    return cmd == 0x37 && offset >= 24 ? Controller : Discrete;
  }
  if ((cmd == 0x37 || cmd == 0x38 || cmd == 0x3A || cmd == 0x3C) && offset < 4) {
    return Counter;
  }
  if (cmd == 0x37) {
    if (offset >= 24 && offset < 59) {
      return Controller;
    }
    if (offset >= 63) {
      return Controller;
    }
    for (uint32_t start : semantic_fields(cmd)) {
      if (offset >= start && offset < start + 4) {
        return offset - start < 2 ? FloatHigh : FloatLow;
      }
    }
    if (offset >= 6 && offset < 10) {
      return Rng;
    }
  } else if (cmd == 0x38) {
    for (uint32_t start : semantic_fields(cmd)) {
      if (offset >= start && offset < start + 4) {
        return offset - start < 2 ? FloatHigh : FloatLow;
      }
    }
  } else if (cmd == 0x3A && offset >= 4 && offset < 8) {
    return Rng;
  }
  return Generic;
}

// seed field location per command: pre-frame (0x37) at offset 6,
// frame start (0x3A) at offset 4; 0 = command carries no seed
uint32_t seed_offset_for(uint8_t cmd, uint32_t recSize) {
  if (cmd == 0x37 && recSize >= 10) {
    return 6;
  }
  if (cmd == 0x3A && recSize >= 8) {
    return 4;
  }
  return 0;
}

const LcgTables &lcg_tables() {
  static const LcgTables tables;
  return tables;
}
uint32_t lcg_advance(uint32_t seed, uint32_t steps) {
  if (steps > kSeedCap) {
    fail("corrupt archive (seed step count)");
  }
  if (!steps) {
    return seed;
  }
  const auto &jump = lcg_tables().jumps[steps];
  return jump.multiply * seed + jump.add;
}
// Low 13 bits visit all 8192 states, so they identify at most one candidate
// in the 0..4096 search window. Verify the full state before accepting it.
uint32_t lcg_distance(uint32_t a, uint32_t b) {
  if (a == b) {
    return 0;
  }
  const auto &table = lcg_tables();
  uint32_t k = (uint32_t(table.phase[b & 8191]) - table.phase[a & 8191]) & 8191;
  if (k > kSeedCap) {
    return UINT32_MAX;
  }
  const auto &jump = table.jumps[k];
  return jump.multiply * a + jump.add == b ? k : UINT32_MAX;
}

bool parse_slp(std::span<const uint8_t> file, ParsedSlp &out) {
  out = ParsedSlp{};
  if (file.size() < kSlpHeaderLen + 2) {
    return false;
  }
  if (memcmp(file.data(), kSlpMagic, 11) != 0) {
    return false;
  }
  uint32_t rawLen = be32(file.data() + 11);

  size_t rawStart = kSlpHeaderLen;
  size_t rawEnd; // exclusive
  if (rawLen != 0 && rawStart + rawLen <= file.size()) {
    rawEnd = rawStart + rawLen;
  } else {
    rawEnd = file.size(); // in-progress replay: events run to EOF
  }
  out.prefix = file.subspan(0, rawStart);
  out.suffix = file.subspan(rawEnd);

  const uint8_t *p = file.data() + rawStart;
  const uint8_t *end = file.data() + rawEnd;
  if (p >= end || *p != 0x35) {
    return false;
  }
  if (size_t(end - p) < 2) {
    return false;
  }
  uint8_t epSize = p[1]; // includes the size byte itself
  if (epSize < 1 || (epSize - 1) % 3 != 0 || size_t(end - p) < size_t(1 + epSize)) {
    return false;
  }
  out.payloadsEvent = std::span<const uint8_t>(p + 1, epSize);

  int32_t sizeOf[256];
  for (auto &s : sizeOf) {
    s = -1;
  }
  int16_t idxOf[256];
  for (auto &i : idxOf) {
    i = -1;
  }
  for (const uint8_t *t = p + 2; t < p + 1 + epSize; t += 3) {
    uint8_t cmd = t[0];
    int32_t sz = be16(t + 1);
    // corrupt files can redeclare a command with a different size; the stream
    // would keep the first recSize while records append at the last → OOB
    if (sizeOf[cmd] >= 0 && sizeOf[cmd] != sz) {
      return false;
    }
    sizeOf[cmd] = sz;
    if (idxOf[cmd] < 0) {
      idxOf[cmd] = (int16_t)out.streams.size();
      out.streams.push_back({cmd, (uint32_t)sz, {}, 0});
    }
  }
  p += 1 + epSize;

  {
    uint32_t events = 0;
    for (const uint8_t *q = p; q < end;) {
      uint8_t cmd = *q;
      int32_t sz = sizeOf[cmd];
      if (sz < 0 || size_t(end - q) <= size_t(sz)) {
        break;
      }
      ++out.streams[size_t(idxOf[cmd])].count;
      ++events;
      q += 1 + sz;
    }
    out.order.reserve(events);
    for (auto &st : out.streams) {
      st.records.reserve(size_t(st.count) * st.recSize);
      st.count = 0;
    }
  }
  while (p < end) {
    uint8_t cmd = *p;
    int32_t sz = sizeOf[cmd];
    if (sz < 0 || size_t(end - p) <= size_t(sz)) {
      break; // rest becomes tail
    }
    auto &st = out.streams[(size_t)idxOf[cmd]];
    st.records.put_bytes(p + 1, (size_t)sz);
    st.count++;
    out.order.put_u8(cmd);
    p += 1 + sz;
  }
  out.tail = std::span<const uint8_t>(p, (size_t)(end - p));
  // The original declaration table is already preserved in payloadsEvent.
  // An unused declaration needs no stream packet, selectors, or plane models.
  // Keep zero-width streams when actual command-only events were observed.
  std::erase_if(out.streams, [](const EventStream &st) { return st.count == 0; });
  return true;
}

// Pre/post-frame events (0x37/0x38) carry (playerIndex, isFollower) at
// payload offsets 4-5; item updates (0x3B) carry the item typeID u16 there.
// Splitting on those two bytes makes plane deltas coherent per entity.
bool splittable(uint8_t cmd, uint32_t recSize) {
  return (cmd == 0x37 || cmd == 0x38 || cmd == 0x3B) && recSize >= 6;
}

// Replace each record's seed with LCG-steps-from-previous; zero the seed
// bytes in place. Returns false (leaving records untouched) when the file's
// seeds don't follow the Melee LCG.
// Distances are computed in global event order (the RNG is global), but the
// varint stream is EMITTED grouped by entity: per-player step counts are far
// more periodic than the interleaved sequence (measured -21%).
bool fold_seeds(EventStream &st, uint32_t seedOff, Buf &seedStream, const uint8_t *groupSel,
                Stats *stats) {
  if (st.count < 2) {
    return false;
  }
  auto seed_at = [&](uint32_t i) {
    return be32(st.records.data() + (size_t)i * st.recSize + seedOff);
  };
  uint32_t probes = st.count - 1 < 32 ? st.count - 1 : 32, hits = 0;
  for (uint32_t i = 0; i < probes; i++) {
    if (lcg_distance(seed_at(i), seed_at(i + 1)) != UINT32_MAX) {
      hits++;
    }
  }
  if (hits * 4 < probes * 3) {
    return false;
  }

  std::vector<uint32_t> ks(st.count), raw(st.count);
  uint32_t prev = 0;
  uint8_t maxG = 0;
  for (uint32_t i = 0; i < st.count; i++) {
    uint8_t *rec = st.records.data() + (size_t)i * st.recSize;
    uint32_t seed = be32(rec + seedOff);
    ks[i] = i == 0 ? UINT32_MAX : lcg_distance(prev, seed);
    raw[i] = seed;
    if (stats) {
      (ks[i] == UINT32_MAX ? stats->seedMiss : stats->seedHit)++;
    }
    memset(rec + seedOff, 0, 4);
    prev = seed;
    if (groupSel && groupSel[i] > maxG) {
      maxG = groupSel[i];
    }
  }
  for (uint32_t g = 0; g <= (groupSel ? (uint32_t)maxG : 0u); g++) {
    for (uint32_t i = 0; i < st.count; i++) {
      if (groupSel && groupSel[i] != g) {
        continue;
      }
      if (ks[i] == UINT32_MAX) {
        varint(seedStream, kSeedException);
        seedStream.put_u32(raw[i]); // LE raw
      } else {
        varint(seedStream, ks[i]);
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
bool cross_pred_eligible(uint32_t preSize, uint32_t preCount, uint32_t postSize,
                         uint32_t postCount) {
  return preSize >= kPreB + kLenB && postSize >= kPostB + kLenB && preCount >= 2 && postCount >= 1;
}

// Pairs the i-th pre-frame of an entity with its (i-1)-th post-frame and
// XORs the predicted spans. Self-inverse: the decoder calls it again on the
// reconstructed post-frame stream. When probe=true, only measures the
// span-A hit rate on a prefix and mutates nothing.
template <class Pre, class Post>
static uint32_t cross_pred_records(Pre pre, uint32_t preCount, Post post, uint32_t postCount,
                                   bool probe, uint32_t probeMax, uint32_t *pairsOut) {
  static thread_local std::array<uint32_t, 65536> keyIdx{};
  static thread_local std::array<bool, 65536> occ{};
  std::vector<uint16_t> keys;
  keys.reserve(std::min<uint32_t>(65536, postCount));
  std::unique_ptr<uint32_t[]> next(new uint32_t[postCount]);
  for (uint32_t i = postCount; i-- > 0;) {
    const uint8_t *q = post(i);
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
    uint8_t *p = pre(i);
    uint16_t k = (uint16_t)(p[4] << 8 | p[5]);
    if (!keyIdx[k]) {
      continue;
    }
    if (!occ[k]) {
      occ[k] = true;
      continue;
    }
    uint32_t at = keyIdx[k] - 1;
    const uint8_t *q = post(at);
    keyIdx[k] = next[at];
    if (probe) {
      pairs++;
      hits += memcmp(p + kPreA, q + kPostA, kLenA) == 0;
      continue;
    }
    for (uint32_t b = 0; b < kLenA; b++) {
      p[kPreA + b] ^= q[kPostA + b];
    }
    for (uint32_t b = 0; b < kLenB; b++) {
      p[kPreB + b] ^= q[kPostB + b];
    }
  }
  for (auto k : keys) {
    keyIdx[k] = 0;
  }
  if (pairsOut) {
    *pairsOut = pairs;
  }
  return hits;
}

uint32_t cross_pred_xor(uint8_t *pre, uint32_t preCount, uint32_t preSize, const uint8_t *post,
                        uint32_t postCount, uint32_t postSize, bool probe, uint32_t probeMax,
                        uint32_t *pairsOut) {
  return cross_pred_records([&](uint32_t i) { return pre + size_t(i) * preSize; }, preCount,
                            [&](uint32_t i) { return post + size_t(i) * postSize; }, postCount,
                            probe, probeMax, pairsOut);
}

// ── PPC branch-target absolutization ────────────────────────────────────────
// Rewrites relative branch displacements (b/bl opcode 18, bc opcode 16) to
// instruction-index-based targets so repeated calls to the same function
// become byte-identical LZ matches (kkrunchy-style, adapted to PowerPC).
// Modular arithmetic on the displacement field makes it exactly invertible.
void ppc_branch_xform(uint8_t *p, size_t nWords, bool inverse) {
  for (size_t i = 0; i < nWords; i++) {
    uint8_t *w = p + i * 4;
    uint32_t v = be32(w);
    uint32_t op = v >> 26;
    if (op == 18) {
      if (v & 2) {
        continue; // AA=1: already absolute
      }
      uint32_t li = (v >> 2) & 0xFFFFFF;
      uint32_t t = (inverse ? li - (uint32_t)i : li + (uint32_t)i) & 0xFFFFFF;
      v = (v & 0xFC000003u) | (t << 2);
    } else if (op == 16) {
      if (v & 2) {
        continue;
      }
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

// Position predictors use IEEE binary32 round-to-nearest operations. Volatile
// intermediates prevent contraction/excess precision; non-finite predictions
// use zero so NaN payload bits remain literal and reconstruct exactly.
static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4);
#ifdef __FAST_MATH__
#error SLPZ v1 predictors require IEEE floating-point semantics
#endif
float read_float(const uint8_t *p) {
  uint32_t u = be32(p);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
// Swap exact integral binary32 values in [-4096,4096] with zigzag codes.
// The swap also defines the inverse for subnormals whose bits are those codes;
// all other representations, including signed zero and NaN payloads, pass through.
uint32_t integer_grid(uint32_t value) {
  if (value <= 8192) {
    int n = int(value >> 1) ^ -int(value & 1);
    float f = float(n);
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
  }
  uint32_t magnitude = value & 0x7fffffffu;
  if (magnitude < 0x3f800000u || magnitude > 0x45800000u) {
    return value;
  }
  // Exact integral values have no significand bits below the units position.
  unsigned shift = 150 - (magnitude >> 23);
  uint32_t mantissa = (magnitude & 0x7fffffu) | 0x800000u;
  unsigned n = mantissa >> shift;
  return (n << shift) == mantissa ? n * 2 - (value >> 31) : value;
}
template <bool inverse>
static void motion_predict_impl(uint8_t *data, uint32_t count, uint32_t size) {
  if (size < 29u) {
    return;
  }
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
  auto store = [](uint8_t *p, uint32_t x) {
    p[0] = x >> 24;
    p[1] = x >> 16;
    p[2] = x >> 8;
    p[3] = x;
  };
  auto transform = [&](uint8_t *r) {
    auto field = [&]<unsigned K, unsigned O>() {
      if (O + 4 > size) {
        return;
      }
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
    if (size >= 68) {
      pair(60, 2, true);
    } else {
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
      if constexpr (!inverse) {
        v = integer_grid(v);
      }
      uint32_t actual = inverse ? prediction + ((v >> 1) ^ (0u - (v & 1))) : v;
      uint32_t d = v - prediction, code = (d << 1) ^ (0u - (d >> 31));
      actual = integer_grid(actual);
      if constexpr (inverse) {
        code = actual;
      }
      if (state < 1024) {
        float a;
        memcpy(&a, &actual, 4);
        if (state != lastState) {
          starts[state] = a;
        } else if (a > x) {
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
      auto *r = data + size_t(i) * size;
      if constexpr (inverse) {
        transform(r);
      }
      float32x2_t self = {read_float(r + (r[46] ? 52 : 68)), read_float(r + 56)};
      uint32x2_t attack;
      if constexpr (inverse) {
        attack = vld1_u32(last + 2);
      } else {
        attack = vreinterpret_u32_u8(vrev32_u8(vld1_u8(r + 60)));
      }
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
      if constexpr (!inverse) {
        transform(r);
      }
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
      auto *r = data + size_t(i) * size;
      if constexpr (inverse) {
        transform(r);
      }
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
      if constexpr (!inverse) {
        transform(r);
      }
    }
    return;
  }
#endif
  for (uint32_t i = 0; i < count; i++) {
    uint8_t *r = data + (size_t)i * size;
    if constexpr (inverse) {
      transform(r);
    }
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
      uint8_t *f = r + 9 + 4 * j;
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
    if constexpr (!inverse) {
      transform(r);
    }
  }
}
void motion_predict(uint8_t *data, uint32_t count, uint32_t size, bool inverse) {
  if (inverse) {
    motion_predict_impl<true>(data, count, size);
  } else {
    motion_predict_impl<false>(data, count, size);
  }
}
// Swap each exact q/80 controller value with its small zigzag code. The
// explicit binary32 table makes the permutation independent of float division.
uint32_t controller_grid(uint32_t value) {
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
  if (value <= 160) {
    return grid[value];
  }
  uint32_t magnitude = value & 0x7fffffffu;
  if (magnitude > 0x3f800000u) {
    return value;
  }
  float f;
  memcpy(&f, &magnitude, 4);
  unsigned q = unsigned(f * 80.f + 0.5f), code = q * 2 - (value >> 31);
  return code <= 160 && grid[code] == value ? code : value;
}
template <bool inverse>
static void input_predict_impl(uint8_t *data, uint32_t count, uint32_t size) {
  if (size < 59) {
    return;
  }
  uint32_t table[4][256] = {};
  constexpr unsigned bits = 12;
  unsigned slots = 1u << bits;
  unsigned axes = 1 + (size > 63) + (size > 64) + (size > 65);
  std::vector<uint64_t> pair(axes * slots, UINT64_MAX);

  auto map = [](uint8_t *r) {
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
    uint8_t *r = data + (size_t)i * size;
    if constexpr (!inverse) {
      map(r);
    }
    // Processed and physical button copies; float trigger vs max(L,R).
    r[46] ^= r[48];
    r[47] ^= r[49];
    const uint8_t *pred = be32(r + 50) >= be32(r + 54) ? r + 50 : r + 54;
    for (uint32_t j = 0; j < 4; j++) {
      r[40 + j] ^= pred[j];
    }
    for (uint32_t j = 0; j < 4; j++) {
      if (raw[j] >= size) {
        continue;
      }
      uint8_t *f = r + offsets[j];
      uint8_t key = r[raw[j]];
      unsigned other = j ^ 1;
      uint32_t pairKey = key;
      if (raw[other] < size) {
        pairKey |= uint32_t(r[raw[other]]) << 8;
      } else {
        pairKey ^= (be32(r + offsets[other]) & 0x7fffffffu) * 2246822519u;
      }
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
    if constexpr (inverse) {
      map(r);
    }
  }
}
void input_predict(uint8_t *data, uint32_t count, uint32_t size, bool inverse) {
  if (inverse) {
    input_predict_impl<true>(data, count, size);
  } else {
    input_predict_impl<false>(data, count, size);
  }
}

} // namespace slpz
