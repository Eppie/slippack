#include "record_codec.h"
#include "slp.h"
#include "custom_codec.h"
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
#if defined(__aarch64__) && !defined(SLIPPACK_CUSTOM_SCALAR)
#include <arm_neon.h>
#define SLIPPACK_NEON 1
#else
#define SLIPPACK_NEON 0
#endif

namespace slippack {
template <unsigned Rows = 16, unsigned Cols = 16>
static inline void tile16(const uint8_t *src, size_t sStride, uint8_t *dst, size_t dStride) {
#if SLIPPACK_NEON
  uint8x16_t x[16];
  for (unsigned k = 0; k < 16; ++k) {
    if (k >= Rows) {
      x[k] = vdupq_n_u8(0);
    } else if constexpr (Cols == 16) {
      x[k] = vld1q_u8(src + size_t(k) * sStride);
    } else {
      uint8_t row[16] = {};
      memcpy(row, src + size_t(k) * sStride, Cols);
      x[k] = vld1q_u8(row);
    }
  }
  for (int k = 0; k < 16; k += 2) {
    uint8x16_t lo = vtrn1q_u8(x[k], x[k + 1]), hi = vtrn2q_u8(x[k], x[k + 1]);
    x[k] = lo;
    x[k + 1] = hi;
  }
  for (int k = 0; k < 16; k++) {
    if (!(k & 2)) {
      uint16x8_t a = vreinterpretq_u16_u8(x[k]), b = vreinterpretq_u16_u8(x[k | 2]);
      x[k] = vreinterpretq_u8_u16(vtrn1q_u16(a, b));
      x[k | 2] = vreinterpretq_u8_u16(vtrn2q_u16(a, b));
    }
  }
  for (int k = 0; k < 16; k++) {
    if (!(k & 4)) {
      uint32x4_t a = vreinterpretq_u32_u8(x[k]), b = vreinterpretq_u32_u8(x[k | 4]);
      x[k] = vreinterpretq_u8_u32(vtrn1q_u32(a, b));
      x[k | 4] = vreinterpretq_u8_u32(vtrn2q_u32(a, b));
    }
  }
  for (int k = 0; k < 16; k++) {
    if (!(k & 8)) {
      uint64x2_t a = vreinterpretq_u64_u8(x[k]), b = vreinterpretq_u64_u8(x[k | 8]);
      x[k] = vreinterpretq_u8_u64(vtrn1q_u64(a, b));
      x[k | 8] = vreinterpretq_u8_u64(vtrn2q_u64(a, b));
    }
  }
  for (unsigned k = 0; k < Cols; ++k) {
    if constexpr (Rows == 16) {
      vst1q_u8(dst + size_t(k) * dStride, x[k]);
    } else {
      uint8_t row[16];
      vst1q_u8(row, x[k]);
      memcpy(dst + size_t(k) * dStride, row, Rows);
    }
  }
#else
  for (size_t i = 0; i < Rows; i++) {
    for (size_t j = 0; j < Cols; j++) {
      dst[j * dStride + i] = src[i * sStride + j];
    }
  }
#endif
}

// records (count × size) → planes (size × count)
static void transpose_fwd(std::span<const uint8_t> recs, uint32_t count, uint32_t size, Buf &out) {
  out.resize(recs.size());
  const uint8_t *in = recs.data();
  uint32_t iTiles = count & ~15u, jTiles = size & ~15u;
  for (uint32_t j = 0; j < jTiles; j += 16) {
    for (uint32_t i = 0; i < iTiles; i += 16) {
      tile16(in + (size_t)i * size + j, size, out.data() + (size_t)j * count + i, count);
    }
  }
  // Keep incomplete record-width tiles in the vector path, without reading
  // or writing outside the short edge of the source/destination.
  auto tail = [&]<unsigned W>() {
    for (unsigned i = 0; i < iTiles; i += 16) {
      tile16<16, W>(in + size_t(i) * size + jTiles, size, out.data() + size_t(jTiles) * count + i,
                    count);
    }
  };
  switch (size - jTiles) {
  case 1:
    tail.operator()<1>();
    break;
  case 2:
    tail.operator()<2>();
    break;
  case 3:
    tail.operator()<3>();
    break;
  case 4:
    tail.operator()<4>();
    break;
  case 5:
    tail.operator()<5>();
    break;
  case 6:
    tail.operator()<6>();
    break;
  case 7:
    tail.operator()<7>();
    break;
  case 8:
    tail.operator()<8>();
    break;
  case 9:
    tail.operator()<9>();
    break;
  case 10:
    tail.operator()<10>();
    break;
  case 11:
    tail.operator()<11>();
    break;
  case 12:
    tail.operator()<12>();
    break;
  case 13:
    tail.operator()<13>();
    break;
  case 14:
    tail.operator()<14>();
    break;
  case 15:
    tail.operator()<15>();
    break;
  }
  for (uint32_t j = jTiles; j < size; j++) {
    for (uint32_t i = iTiles; i < count; i++) {
      out[(size_t)j * count + i] = in[(size_t)i * size + j];
    }
  }
  for (uint32_t j = 0; j < jTiles; j++) {
    for (uint32_t i = iTiles; i < count; i++) {
      out[(size_t)j * count + i] = in[(size_t)i * size + j];
    }
  }
}

// planes (size × count) → records (count × size)
static void transpose_rev(std::span<const uint8_t> planes, uint32_t count, uint32_t size,
                          uint8_t *out) {
  const uint8_t *in = planes.data();
  uint32_t iTiles = count & ~15u, jTiles = size & ~15u;
  for (uint32_t i = 0; i < iTiles; i += 16) {
    for (uint32_t j = 0; j < jTiles; j += 16) {
      tile16(in + (size_t)j * count + i, count, out + (size_t)i * size + j, size);
    }
  }
  // Keep incomplete record-width tiles in the vector path, without reading
  // or writing outside the short edge of the source/destination.
  auto tail = [&]<unsigned W>() {
    for (unsigned i = 0; i < iTiles; i += 16) {
      tile16<W, 16>(in + size_t(jTiles) * count + i, count, out + size_t(i) * size + jTiles, size);
    }
  };
  switch (size - jTiles) {
  case 1:
    tail.operator()<1>();
    break;
  case 2:
    tail.operator()<2>();
    break;
  case 3:
    tail.operator()<3>();
    break;
  case 4:
    tail.operator()<4>();
    break;
  case 5:
    tail.operator()<5>();
    break;
  case 6:
    tail.operator()<6>();
    break;
  case 7:
    tail.operator()<7>();
    break;
  case 8:
    tail.operator()<8>();
    break;
  case 9:
    tail.operator()<9>();
    break;
  case 10:
    tail.operator()<10>();
    break;
  case 11:
    tail.operator()<11>();
    break;
  case 12:
    tail.operator()<12>();
    break;
  case 13:
    tail.operator()<13>();
    break;
  case 14:
    tail.operator()<14>();
    break;
  case 15:
    tail.operator()<15>();
    break;
  }
  for (uint32_t j = jTiles; j < size; j++) {
    for (uint32_t i = iTiles; i < count; i++) {
      out[(size_t)i * size + j] = in[(size_t)j * count + i];
    }
  }
  for (uint32_t j = 0; j < jTiles; j++) {
    for (uint32_t i = iTiles; i < count; i++) {
      out[(size_t)i * size + j] = in[(size_t)j * count + i];
    }
  }
}

// in-place p[i] -= p[i-1] (or ^=), processed back-to-front so reads see
// original values; vector blocks stay 16 behind the last write.
template <bool XOR> static void delta_fwd(uint8_t *p, size_t n) {
  if (n < 2) {
    return;
  }
#if SLIPPACK_NEON
  size_t pos = n;
  while (pos >= 17) {
    pos -= 16;
    uint8x16_t v = vld1q_u8(p + pos), vp = vld1q_u8(p + pos - 1);
    vst1q_u8(p + pos, XOR ? veorq_u8(v, vp) : vsubq_u8(v, vp));
  }
  for (size_t i = pos; i-- > 1;) {
    p[i] = XOR ? (uint8_t)(p[i] ^ p[i - 1]) : (uint8_t)(p[i] - p[i - 1]);
  }
#else
  for (size_t i = n; i-- > 1;) {
    p[i] = XOR ? (uint8_t)(p[i] ^ p[i - 1]) : (uint8_t)(p[i] - p[i - 1]);
  }
#endif
}

// inclusive scan undoing delta_fwd (Hillis–Steele within a vector + carry)
template <bool XOR> static void scan_rev(uint8_t *p, size_t n) {
#if SLIPPACK_NEON
  const uint8x16_t zero = vdupq_n_u8(0);
  uint8x16_t carry = zero;
  auto combine = [](uint8x16_t a, uint8x16_t b) { return XOR ? veorq_u8(a, b) : vaddq_u8(a, b); };
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
  for (size_t i = 1; i < n; i++) {
    p[i] = XOR ? (uint8_t)(p[i] ^ p[i - 1]) : (uint8_t)(p[i] + p[i - 1]);
  }
#endif
}

// ── per-plane transform choice ──────────────────────────────────────────────
// Order-0 entropy over contiguous sample windows decides plain vs sub vs xor.
// Contiguous windows keep the delta candidates honest (true neighbors) and
// the memory access pattern cache-friendly.
static const float *xlog2x_lut() {
  constexpr uint32_t N = 1 << 15;
  static float *lut = [] {
    float *t = new float[N + 1];
    t[0] = 0.f;
    for (uint32_t c = 1; c <= N; c++) {
      t[c] = (float)c * std::log2f((float)c);
    }
    return t;
  }();
  return lut;
}

// Return the transform with the lowest estimated order-0 entropy.
static uint8_t choose_transform(const uint8_t *plane, uint32_t count) {
  // Constant planes have an explicit compact representation.
  if (slippack_custom::is_constant({plane, count})) {
    return T_PLAIN;
  }

  uint32_t hist[3][256] = {};
  uint32_t m = 0;
  constexpr uint32_t kWin = 256;
  uint32_t starts[3] = {0, count > 2 * kWin ? count / 2 : count,
                        count > 3 * kWin ? count - kWin : count};
  for (uint32_t s : starts) {
    if (s >= count) {
      continue;
    }
    uint32_t n = count - s < kWin ? count - s : kWin;
    const uint8_t *p = plane + s;
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
  const float *lut = xlog2x_lut();
  float score[3];
  for (int t = 0; t < 3; t++) {
    float s = 0;
    for (uint32_t c = 0; c < 256; c++) {
      s += lut[hist[t][c]];
    }
    score[t] = s; // maximize Σ c·log2 c  ⇔  minimize entropy
  }
  const float eps = 0.01f * (float)m;
  uint8_t t;
  if (score[0] + eps >= score[1] && score[0] + eps >= score[2]) {
    t = T_PLAIN;
  } else if (score[1] + eps / 2 >= score[2]) {
    t = T_SUB;
  } else {
    t = T_XOR;
  }
  return t;
}

// ── field dictionarization ──────────────────────────────────────────────────
// Melee analog fields are quantized floats: a 4-byte column often takes few
// distinct values. Byte-plane splitting scatters that structure over 4
// planes; replacing the field with a 1-byte dictionary index recovers it
// (and shrinks the entropy-stage input ~40%).
struct U32Set {
  static constexpr uint32_t N = 1024;
  uint32_t values[256];
  uint16_t slots[N] = {};
  uint32_t n = 0;
  int find_or_insert(uint32_t key) {
    uint32_t h = (key * 2654435761u) >> 22;
    for (;; h = (h + 1) & (N - 1)) {
      unsigned slot = slots[h];
      if (!slot) {
        if (n >= 256) {
          return -1;
        }
        values[n] = key;
        slots[h] = uint16_t(n + 1);
        return int(n++);
      }
      if (values[slot - 1] == key) {
        return int(slot - 1);
      }
    }
  }
};

// Two varying bytes identify a value exactly; avoid hash collisions and
// the dependent key comparison. Clear only touched entries before reuse.
struct U16Lookup {
  U32Set &set;
  uint16_t *slots = nullptr;
  unsigned lo, hi;
  unsigned index(uint32_t value) const {
    return ((value >> lo) & 255) | (((value >> hi) & 255) << 8);
  }
  U16Lookup(U32Set &s, unsigned changing) : set(s), lo(0), hi(0) {
    if (std::popcount(changing) == 2) {
      lo = std::countr_zero(changing) * 8;
      hi = (31 - std::countl_zero(changing)) * 8;
      static thread_local std::array<uint16_t, 65536> table{};
      slots = table.data();
    }
  }
  U16Lookup(const U16Lookup &) = delete;
  U16Lookup &operator=(const U16Lookup &) = delete;
  ~U16Lookup() {
    if (slots) {
      for (unsigned i = 0; i < set.n; ++i) {
        slots[index(set.values[i])] = 0;
      }
    }
  }
  int find_or_insert(uint32_t value) {
    auto &slot = slots[index(value)];
    if (slot) {
      return slot - 1;
    }
    if (set.n == 256) {
      return -1;
    }
    set.values[set.n] = value;
    slot = ++set.n;
    return slot - 1;
  }
};

void sort_dictionaries(std::vector<DictField> &fields) {
  for (auto &f : fields) {
    if (f.vals.size() < 2) {
      continue;
    }
    std::array<uint16_t, 256> order;
    std::array<uint32_t, 256> keys;
    std::array<uint8_t, 256> remap;
    for (size_t i = 0; i < f.vals.size(); ++i) {
      order[i] = uint16_t(i);
      keys[i] = 0;
      for (unsigned b = 0; b < f.width; ++b) {
        keys[i] = (keys[i] << 8) | uint8_t(f.vals[i] >> (8 * b));
      }
    }
    if (std::is_sorted(keys.begin(), keys.begin() + f.vals.size())) {
      continue;
    }
    std::sort(order.begin(), order.begin() + f.vals.size(),
              [&](unsigned a, unsigned b) { return keys[a] < keys[b]; });
    auto old = f.vals;
    for (size_t i = 0; i < f.vals.size(); ++i) {
      f.vals[i] = old[order[i]];
      remap[order[i]] = uint8_t(i);
    }
    for (auto &id : f.idx) {
      id = remap[id];
    }
  }
}

// Values are shuffled per field. Modes 2/3 predict the previous byte in each
// plane; mode 4 predicts the previous whole big-endian field before shuffling.
Buf dictionary_forward(const std::vector<DictField> &fields, uint8_t transform) {
  Buf result;
  size_t bytes = 0;
  for (const auto &f : fields) {
    bytes += f.vals.size() * f.width;
  }
  result.reserve(bytes);
  for (const auto &f : fields) {
    std::array<uint32_t, 256> deltas;
    if (transform == 4) {
      uint32_t previous = 0;
      for (size_t i = 0; i < f.vals.size(); ++i) {
        uint32_t value = 0;
        for (unsigned b = 0; b < f.width; ++b) {
          value = (value << 8) | uint8_t(f.vals[i] >> (8 * b));
        }
        deltas[i] = value - previous;
        previous = value;
      }
    }
    for (unsigned b = 0; b < f.width; ++b) {
      uint8_t previous = 0;
      for (size_t i = 0; i < f.vals.size(); ++i) {
        uint8_t value = uint8_t(f.vals[i] >> (8 * b));
        uint8_t encoded = transform == 2   ? value ^ previous
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

void dictionary_reverse(std::span<const uint8_t> data, uint8_t transform,
                        std::vector<DictField> &fields) {
  size_t at = 0;
  for (auto &f : fields) {
    for (unsigned b = 0; b < f.width; ++b) {
      uint8_t previous = 0;
      for (auto &v : f.vals) {
        uint8_t value = data[at++];
        if (transform == 2) {
          value ^= previous;
        } else if (transform == 3) {
          value += previous;
        }
        v = (v & ~(255u << (8 * b))) | (uint32_t(value) << (8 * b));
        previous = value;
      }
    }
    if (transform == 4) {
      uint32_t previous = 0;
      for (auto &v : f.vals) {
        uint32_t delta = 0;
        for (unsigned b = 0; b < f.width; ++b) {
          delta = (delta << 8) | uint8_t(v >> (8 * b));
        }
        uint32_t value = previous + delta;
        previous = value;
        v = 0;
        for (unsigned b = 0; b < f.width; ++b) {
          v |= uint32_t(uint8_t(value >> (8 * (f.width - 1 - b)))) << (8 * b);
        }
      }
    }
  }
}

// Transpose, dictionarize complete fields, and select a reversible byte
// transform per plane. Layout 2 stores the semantic custom plane packet.
void pack_records(std::span<const uint8_t> recs, uint32_t count, uint32_t size, int level,
                  Buf &blob, std::atomic<uint64_t> *stat, uint8_t cmd) {
  require(size <= UINT16_MAX && recs.size() == uint64_t(count) * size,
          "invalid record input shape");
  size_t blobAt = blob.size();
  Buf planes;
  transpose_fwd(recs, count, size, planes);

  std::vector<DictField> fields;
  std::vector<uint8_t> widthAt(size, 0); // field width at start offset
  if (count >= 64) {
    std::vector<uint8_t> covered(size, 0), constant(size, 0);
    std::vector<unsigned> alphabets(size, 0);
    constexpr unsigned limit = 128;
    for (unsigned o = 0; o < size; ++o) {
      constant[o] = slippack_custom::is_constant({planes.data() + size_t(o) * count, count});
    }
    Buf idxPlane;
    // Preserve complete semantic fields before considering sliding windows.
    std::vector<std::pair<unsigned, unsigned>> trials;
    trials.reserve(size_t(size) * 2 + 16);
    for (unsigned offset : semantic_fields(cmd)) {
      if (offset + 4 <= size) {
        trials.emplace_back(4, offset);
      }
    }
    for (unsigned width : {4u, 2u}) {
      for (unsigned w = 0; w + width <= size; ++w) {
        trials.emplace_back(width, w);
      }
    }
    for (auto [width, w] : trials) {
      bool free = true;
      for (uint32_t b = 0; b < width; b++) {
        free = free && !covered[w + b];
      }
      if (!free) {
        continue;
      }
      unsigned changing = 0, changingOffset = 0, changingMask = 0;
      for (unsigned b = 0; b < width; ++b) {
        if (!constant[w + b]) {
          ++changing;
          changingMask |= 1u << b;
          changingOffset = w + b;
        }
      }
      // A large alphabet in one varying byte gains little from a dictionary:
      // explicit constant planes already represent its fixed neighbors.
      if (changing == 1) {
        if (!alphabets[changingOffset]) {
          std::array<bool, 256> seen{};
          unsigned distinct = 0;
          for (unsigned i = 0; i < count; ++i) {
            uint8_t v = planes[size_t(changingOffset) * count + i];
            if (!seen[v]) {
              seen[v] = true;
              if (++distinct == limit) {
                break;
              }
            }
          }
          alphabets[changingOffset] = distinct;
        }
        if (alphabets[changingOffset] >= limit) {
          continue;
        }
      }
      U32Set set;
      // A successful varying-field scan writes every index. Retain the size
      // between trials so resize does not zero the same storage repeatedly.
      if (changing) {
        idxPlane.resize(count);
      }
      bool ok = true;
      if (changing == 0) {
        uint32_t v = little_word(recs.data() + w, width);
        set.find_or_insert(v); // every index is already zero
      } else if (changing == 1) {
        std::array<int16_t, 256> ids;
        ids.fill(-1);
        auto *plane = planes.data() + size_t(changingOffset) * count;
        for (uint32_t i = 0; i < count; ++i) {
          uint8_t b = plane[i];
          if (ids[b] < 0) {
            uint32_t v = little_word(recs.data() + size_t(i) * size + w, width);
            ids[b] = set.find_or_insert(v);
          }
          idxPlane[i] = uint8_t(ids[b]);
        }
      } else {
        auto gather = [&]<unsigned Width, bool Direct>() {
          // No allocations or callbacks while the shared table contains entries.
          U16Lookup direct(set, Direct ? changingMask : 0);
          auto lookup = [&](uint32_t value) {
            if constexpr (Direct) {
              return direct.find_or_insert(value);
            } else {
              return set.find_or_insert(value);
            }
          };
          uint32_t lastKey = 0;
          int lastId = -1;
          const uint8_t *column = planes.data() + size_t(w) * count;
          uint32_t first = 0;
#if SLIPPACK_NEON && !defined(SLIPPACK_CUSTOM_SCALAR)
          for (; first + 16 <= count; first += 16) {
            uint8_t packed[Width * 16];
            if constexpr (Width == 4) {
              uint8x16x4_t input;
              for (unsigned b = 0; b < 4; ++b) {
                input.val[b] = vld1q_u8(column + size_t(b) * count + first);
              }
              auto equal = vdupq_n_u8(255);
              for (unsigned b = 0; b < 4; ++b) {
                equal = vandq_u8(equal,
                                 vceqq_u8(input.val[b], vdupq_n_u8(uint8_t(lastKey >> (8 * b)))));
              }
              if (lastId >= 0 && vminvq_u8(equal) != 0) {
                std::memset(idxPlane.data() + first, lastId, 16);
                continue;
              }
              vst4q_u8(packed, input);
            } else {
              uint8x16x2_t input;
              input.val[0] = vld1q_u8(column + first);
              input.val[1] = vld1q_u8(column + count + first);
              auto equal = vdupq_n_u8(255);
              for (unsigned b = 0; b < 2; ++b) {
                equal = vandq_u8(equal,
                                 vceqq_u8(input.val[b], vdupq_n_u8(uint8_t(lastKey >> (8 * b)))));
              }
              if (lastId >= 0 && vminvq_u8(equal) != 0) {
                std::memset(idxPlane.data() + first, lastId, 16);
                continue;
              }
              vst2q_u8(packed, input);
            }
            for (unsigned k = 0; k < 16; ++k) {
              uint32_t value = little_word(packed + k * Width, Width);
              if (lastId < 0 || lastKey != value) {
                lastId = lookup(value);
                lastKey = value;
              }
              if (lastId < 0) {
                return false;
              }
              idxPlane[first + k] = uint8_t(lastId);
            }
          }
#endif
          for (uint32_t i = first; i < count; ++i) {
            uint32_t value = uint32_t(column[i]) | uint32_t(column[count + i]) << 8;
            if constexpr (Width == 4) {
              value |= uint32_t(column[size_t(count) * 2 + i]) << 16 |
                       uint32_t(column[size_t(count) * 3 + i]) << 24;
            }
            if (lastId < 0 || lastKey != value) {
              lastId = lookup(value);
              lastKey = value;
            }
            if (lastId < 0) {
              return false;
            }
            idxPlane[i] = uint8_t(lastId);
          }
          return true;
        };
        if (changing == 2) {
          ok = width == 4 ? gather.operator()<4, true>() : gather.operator()<2, true>();
        } else {
          ok = width == 4 ? gather.operator()<4, false>() : gather.operator()<2, false>();
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
        f.vals.assign(set.values, set.values + set.n);
        // Constant dictionaries have an implicit zero index plane.
        if (changing) {
          f.idx = idxPlane;
        }
        fields.push_back(std::move(f));
        widthAt[w] = width;
        for (uint32_t b = 0; b < width; b++) {
          covered[w + b] = 1;
        }
      }
    }
    std::sort(fields.begin(), fields.end(),
              [](const DictField &a, const DictField &b) { return a.offset < b.offset; });
  }

  sort_dictionaries(fields);

  uint32_t nOut = size;
  for (auto &f : fields) {
    nOut -= f.width - 1 + implicit_dictionary(f);
  }
  Buf out;
  out.reserve((size_t)nOut * count);
  {
    size_t fi = 0;
    for (uint32_t o = 0; o < size;) {
      if (widthAt[o]) {
        if (!implicit_dictionary(fields[fi])) {
          out.put_bytes(fields[fi].idx.data(), count);
        }
        o += fields[fi].width;
        fi++;
      } else {
        out.put_bytes(planes.data() + (size_t)o * count, count);
        o += 1;
      }
    }
  }

  if (fields.size() > 255) {
    fail("internal: dict field count overflow");
  }
  // Dictionary descriptors remain directly scannable; only value bytes are
  // compressed. The first width byte marks the new representation.
  Buf dictionary;
  uint8_t dictionaryTransform = 0;
  size_t dictionaryBytes = 0;
  for (const auto &f : fields) {
    dictionaryBytes += f.vals.size() * f.width;
  }
  if (dictionaryBytes >= 128) {
    Buf candidates[4];
    unsigned best = 0;
    double cost = std::numeric_limits<double>::infinity();
    for (unsigned t = 0; t < 4; ++t) {
      candidates[t] = dictionary_forward(fields, uint8_t(t + 1));
      std::array<uint32_t, 256> hist{};
      for (auto b : candidates[t]) {
        ++hist[b];
      }
      double c = slippack_custom::context_cost(hist);
      if (c < cost) {
        cost = c;
        best = t;
      }
    }
    small_compress(candidates[best], std::min(level, 3), dictionary, slippack_custom::Discrete);
    if (dictionary.size() < dictionaryBytes) {
      dictionaryTransform = uint8_t(best + 1);
    }
  }
  blob.put_u8(uint8_t(fields.size()));
  for (size_t j = 0; j < fields.size(); ++j) {
    const auto &f = fields[j];
    uint8_t tag = j == 0 && dictionaryTransform ? uint8_t(128 | (dictionaryTransform << 4)) : 0;
    blob.put_u8(f.width | tag);
    blob.put_u8(uint8_t(f.offset));
    blob.put_u8(uint8_t(f.offset >> 8));
    blob.put_u8(uint8_t(f.vals.size() - 1));
    if (!dictionaryTransform) {
      for (uint32_t v : f.vals) {
        for (unsigned byte = 0; byte < f.width; ++byte) {
          blob.put_u8(uint8_t(v >> (8 * byte)));
        }
      }
    }
  }
  if (dictionaryTransform) {
    blob.put_bytes(dictionary.data(), dictionary.size());
  }
  std::vector<uint8_t> profiles;
  for (uint32_t offset = 0; offset < size;) {
    bool constant = false;
    if (widthAt[offset]) {
      for (auto &f : fields) {
        if (f.offset == offset) {
          constant = implicit_dictionary(f);
          break;
        }
      }
    }
    if (!constant) {
      profiles.push_back(field_profile(cmd, offset, widthAt[offset] != 0));
    }
    offset += widthAt[offset] ? widthAt[offset] : 1;
  }
  for (uint32_t j = 0; j < nOut; j++) {
    uint8_t *pl = out.data() + (size_t)j * count;
    uint8_t t = count >= 8 ? choose_transform(pl, count) : T_PLAIN;
    if (t == T_SUB) {
      delta_fwd<false>(pl, count);
    } else if (t == T_XOR) {
      delta_fwd<true>(pl, count);
    }
    blob.put_u8(t);
  }
  blob.put_u8(2);
  custom_planes_frame(out, count, profiles, level, blob);
  if (stat) {
    *stat += blob.size() - blobAt;
  }
}

// Validate the index plane once, then expand at a fixed field width. Small
// alphabets fit in NEON lookup tables; large ones use direct scalar gathers.
template <unsigned W>
void expand_dictionary(const DictField &f, const uint8_t *indexes, uint32_t count,
                       uint8_t *output) {
  if (implicit_dictionary(f)) {
    for (unsigned b = 0; b < W; ++b) {
      memset(output + size_t(b) * count, uint8_t(f.vals[0] >> (8 * b)), count);
    }
    return;
  }
  unsigned maximum = 0, i = 0;
#if SLIPPACK_NEON && !defined(SLIPPACK_CUSTOM_SCALAR)
  auto maxes = vdupq_n_u8(0);
  for (; i + 16 <= count; i += 16) {
    maxes = vmaxq_u8(maxes, vld1q_u8(indexes + i));
  }
  maximum = vmaxvq_u8(maxes);
#endif
  for (; i < count; ++i) {
    maximum = std::max(maximum, unsigned(indexes[i]));
  }
  if (maximum >= f.vals.size()) {
    fail("corrupt archive (dict index)");
  }

  i = 0;
#if SLIPPACK_NEON && !defined(SLIPPACK_CUSTOM_SCALAR)
  if (f.vals.size() <= 64) {
    uint8_t tables[W][64] = {};
    for (unsigned v = 0; v < f.vals.size(); ++v) {
      for (unsigned b = 0; b < W; ++b) {
        tables[b][v] = uint8_t(f.vals[v] >> (8 * b));
      }
    }
    if (f.vals.size() <= 16) {
      uint8x16_t table[W];
      for (unsigned b = 0; b < W; ++b) {
        table[b] = vld1q_u8(tables[b]);
      }
      for (; i + 16 <= count; i += 16) {
        auto ids = vld1q_u8(indexes + i);
        for (unsigned b = 0; b < W; ++b) {
          vst1q_u8(output + size_t(b) * count + i, vqtbl1q_u8(table[b], ids));
        }
      }
    } else {
      uint8x16x4_t table[W];
      for (unsigned b = 0; b < W; ++b) {
        table[b] = vld1q_u8_x4(tables[b]);
      }
      for (; i + 16 <= count; i += 16) {
        auto ids = vld1q_u8(indexes + i);
        for (unsigned b = 0; b < W; ++b) {
          vst1q_u8(output + size_t(b) * count + i, vqtbl4q_u8(table[b], ids));
        }
      }
    }
  }
  // Gather full values, then deinterleave with NEON rather than storing
  // every expanded byte separately for palettes larger than 64 entries.
  if constexpr (std::endian::native == std::endian::little) {
    for (; i + 16 <= count; i += 16) {
      if constexpr (W == 4) {
        uint32_t values[16];
        for (unsigned k = 0; k < 16; ++k) {
          values[k] = f.vals[indexes[i + k]];
        }
        auto columns = vld4q_u8(reinterpret_cast<const uint8_t *>(values));
        for (unsigned b = 0; b < 4; ++b) {
          vst1q_u8(output + size_t(b) * count + i, columns.val[b]);
        }
      } else {
        uint16_t values[16];
        for (unsigned k = 0; k < 16; ++k) {
          values[k] = uint16_t(f.vals[indexes[i + k]]);
        }
        auto columns = vld2q_u8(reinterpret_cast<const uint8_t *>(values));
        for (unsigned b = 0; b < 2; ++b) {
          vst1q_u8(output + size_t(b) * count + i, columns.val[b]);
        }
      }
    }
  }
#endif
  for (; i < count; ++i) {
    uint32_t value = f.vals[indexes[i]];
    for (unsigned b = 0; b < W; ++b) {
      output[size_t(b) * count + i] = uint8_t(value >> (8 * b));
    }
  }
}

// One metadata parser is used for scanning and decoding. Views borrow the
// archive, and dictionary descriptors own their small value tables.
struct RecordPacket {
  std::vector<DictField> fields;
  uint32_t planes;
  uint8_t dictionary_transform;
  View dictionary_frame, selectors, plane_frame;
};
static RecordPacket read_record_packet(Reader &r, uint32_t size, uint64_t count) {
  require(size <= UINT16_MAX, "corrupt record width");
  uint8_t nFields = r.u8();
  std::vector<DictField> fields(nFields);
  uint32_t nOut = size;
  uint8_t dictionaryTransform = 0;
  uint32_t dictionaryBytes = 0;
  std::vector<bool> covered(size, false);
  for (size_t j = 0; j < fields.size(); ++j) {
    auto &f = fields[j];
    f.width = r.u8();
    if (j == 0 && (f.width & 128)) {
      dictionaryTransform = (f.width >> 4) & 7;
      if (!dictionaryTransform || dictionaryTransform > 4) {
        fail("corrupt dictionary transform");
      }
      f.width &= 15;
    }
    f.offset = r.u8();
    f.offset |= uint16_t(r.u8()) << 8;
    uint32_t nVals = uint32_t(r.u8()) + 1;
    if ((f.width != 2 && f.width != 4) || f.offset + f.width > size) {
      fail("corrupt archive (dict field)");
    }
    for (unsigned b = 0; b < f.width; ++b) {
      if (covered[f.offset + b]) {
        fail("corrupt archive (dict overlap)");
      }
      covered[f.offset + b] = true;
    }
    f.vals.resize(nVals);
    dictionaryBytes += nVals * f.width;
    if (!dictionaryTransform) {
      auto raw = r.bytes(nVals * f.width);
      for (uint32_t v = 0; v < nVals; ++v) {
        for (unsigned byte = 0; byte < f.width; ++byte) {
          f.vals[v] |= uint32_t(raw[size_t(v) * f.width + byte]) << (byte * 8);
        }
      }
    }
    if (nOut < f.width) {
      fail("corrupt archive (dict count)");
    }
    nOut -= f.width - 1 + implicit_dictionary(f);
  }
  View dictionaryFrame;
  if (dictionaryTransform) {
    require(r.u8() == kCustom, "corrupt dictionary codec");
    dictionaryFrame = skip_frame(r);
    require(frame_size(dictionaryFrame) == dictionaryBytes, "corrupt dictionary frame size");
  }
  auto selectors = r.bytes(nOut);
  for (auto selector : selectors) {
    require(selector <= T_XOR, "corrupt plane transform");
  }
  require(r.u8() == 2, "corrupt archive (layout id)");
  auto planes = skip_frame(r);
  if (count != UINT64_MAX) {
    require(count * size <= r.output_limit, "records exceed output limit");
    require(frame_size(planes) == count * nOut, "corrupt archive (stream size)");
  }
  return {std::move(fields), nOut, dictionaryTransform, dictionaryFrame, selectors, planes};
}
template <class RecordBuffer>
void unpack_records(Reader &r, uint32_t count, uint32_t size, RecordBuffer &records) {
  auto packet = read_record_packet(r, size, count);
  auto &fields = packet.fields;
  auto nOut = packet.planes;
  if (packet.dictionary_transform) {
    Reader dictionary(packet.dictionary_frame);
    auto bytes = custom_unframe(dictionary);
    dictionary_reverse(bytes, packet.dictionary_transform, fields);
  }
  auto sel = packet.selectors;
  Reader planeReader(packet.plane_frame);
  auto out = custom_unframe(planeReader, true);

  for (uint32_t j = 0; j < nOut; j++) {
    uint8_t *pl = out.data() + (size_t)j * count;
    uint8_t t = sel[j] & 3;
    if (t == T_SUB) {
      scan_rev<false>(pl, count);
    } else if (t == T_XOR) {
      scan_rev<true>(pl, count);
    }
  }

  // Expand and transpose one cache-sized block, avoiding a full-sized plane
  // allocation and its extra memory traffic.
  std::vector<const DictField *> fieldAt(size, nullptr);
  for (auto &f : fields) {
    fieldAt[f.offset] = &f;
  }
  static const unsigned block = 512;
  OutputBytes planes(size_t(std::min(block, count)) * size);
  records.resize(size_t(count) * size);
  for (unsigned at = 0; at < count; at += block) {
    unsigned take = std::min(block, count - at);
    size_t srcAt = 0;
    for (unsigned o = 0; o < size;) {
      bool constant = fieldAt[o] && implicit_dictionary(*fieldAt[o]);
      if (!constant && srcAt + count > out.size()) {
        fail("corrupt archive (plane underrun)");
      }
      const uint8_t *pl = constant ? nullptr : out.data() + srcAt + at;
      auto *dest = planes.data() + size_t(o) * take;
      if (fieldAt[o]) {
        const auto &f = *fieldAt[o];
        if (f.width == 4) {
          expand_dictionary<4>(f, pl, take, dest);
        } else {
          expand_dictionary<2>(f, pl, take, dest);
        }
        o += f.width;
      } else {
        memcpy(dest, pl, take);
        ++o;
      }
      if (!constant) {
        srcAt += count;
      }
    }
    transpose_rev({planes.data(), size_t(take) * size}, take, size,
                  records.data() + size_t(at) * size);
  }
}

int cmd_selftest() {
  std::mt19937 rng(1234);
  auto rnd = [&](size_t n) {
    std::vector<uint8_t> v(n);
    for (auto &b : v) {
      b = (uint8_t)rng();
    }
    return v;
  };
  int fails = 0;
  auto check = [&](bool ok, const char *what, size_t a, size_t b) {
    if (!ok) {
      fprintf(stderr, "FAIL %s (%zu,%zu)\n", what, a, b);
      fails++;
    }
  };
  for (uint32_t count : {1u, 5u, 15u, 16u, 17u, 100u, 1000u, 4097u}) {
    for (uint32_t size : {1u,  2u,  3u,  4u,  5u,  6u,  7u,  8u,  9u,  10u, 11u, 12u,
                          13u, 14u, 15u, 16u, 33u, 51u, 63u, 66u, 71u, 84u, 517u}) {
      auto recs = rnd((size_t)count * size);
      Buf planes;
      transpose_fwd(recs, count, size, planes);
      for (uint32_t i = 0; i < count && !fails; i++) {
        for (uint32_t j = 0; j < size; j++) {
          if (planes[(size_t)j * count + i] != recs[(size_t)i * size + j]) {
            check(false, "transpose_fwd", count, size);
            break;
          }
        }
      }
      std::vector<uint8_t> back(recs.size());
      transpose_rev(planes, count, size, back.data());
      check(memcmp(back.data(), recs.data(), recs.size()) == 0, "transpose_rev", count, size);
    }
  }
  for (size_t n : {0u, 1u, 2u, 15u, 16u, 17u, 31u, 100u, 65536u, 100001u}) {
    auto orig = rnd(n);
    auto v = orig;
    delta_fwd<false>(v.data(), n);
    // scalar reference for encode
    auto ref = orig;
    for (size_t i = n; i-- > 1;) {
      ref[i] = (uint8_t)(ref[i] - ref[i - 1]);
    }
    check(v == ref, "delta_fwd/sub", n, 0);
    scan_rev<false>(v.data(), n);
    check(v == orig, "scan_rev/sub", n, 0);
    v = orig;
    delta_fwd<true>(v.data(), n);
    ref = orig;
    for (size_t i = n; i-- > 1;) {
      ref[i] = (uint8_t)(ref[i] ^ ref[i - 1]);
    }
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
                               {5000, 63},
                               {100, 5},
                               {1, 9},
                               {6000, 1},
                               {70, 12}}) {
      std::vector<uint8_t> recs((size_t)count * size);
      uint32_t v = 12345;
      for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < size; j++) {
          v = v * 1103515245 + 12345;
          uint8_t b = (j % 3 == 0)   ? (uint8_t)(v >> 24)
                      : (j % 3 == 1) ? (uint8_t)(i >> 3)
                                     : (uint8_t)((v >> 28) + i);
          if (j >= 4 && j < 8 && size >= 8) {
            b = (uint8_t)(((i % 23) * 41) + j); // 23 distinct u32s
          }
          if (j >= 8 && j < 10 && size >= 10) {
            b = (uint8_t)(((i % 11) * 3) + j); // 11 distinct u16s
          }
          recs[(size_t)i * size + j] = b;
        }
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
View skip_packed(Reader &r, uint32_t size, uint64_t count) {
  const auto *start = r.p;
  read_record_packet(r, size, count);
  return {start, size_t(r.p - start)};
}

template void expand_dictionary<2>(const DictField &, const uint8_t *, uint32_t, uint8_t *);
template void expand_dictionary<4>(const DictField &, const uint8_t *, uint32_t, uint8_t *);
template void unpack_records<Bytes>(Reader &, uint32_t, uint32_t, Bytes &);
template void unpack_records<OutputBytes>(Reader &, uint32_t, uint32_t, OutputBytes &);

} // namespace slippack
