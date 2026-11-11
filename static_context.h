#pragma once
#include "codec_types.h"

namespace slippack_custom {
// Included inside slippack_custom. Static byte-context trees keep decoding to one
// table lookup per byte; only useful splits (including table cost) are stored.
struct ContextTree {
  std::array<uint32_t, 256> hist{};
  unsigned begin = 0, end = 256;
  double cost = 0;
};
inline double count_entropy(uint32_t n) {
  static const auto table = []() {
    std::array<double, 65536> t{};
    for (unsigned i = 1; i < t.size(); ++i) {
      t[i] = double(i) * std::log2(double(i));
    }
    return t;
  }();
  return n < table.size() ? table[n] : double(n) * std::log2(double(n));
}
inline double context_cost(const std::array<uint32_t, 256> &h) {
  double c = 0;
  uint32_t n = 0, used = 0;
  for (auto x : h) {
    if (x) {
      n += x;
      c -= count_entropy(x);
      ++used;
    }
  }
  if (n) {
    c += count_entropy(n);
  }
  return c + 16 + used * 19;
}
inline Bytes context_encode(View input, View contexts) {
  require(contexts.empty() || contexts.size() == input.size(), "context input shape");
  if (!contexts.empty()) {
    std::array<std::array<uint32_t, 256>, ProfileCount> hist{};
    std::array<uint32_t, 256> merged{};
    for (size_t i = 0; i < input.size(); ++i) {
      require(contexts[i] < ProfileCount, "context profile");
      ++hist[contexts[i]][input[i]];
      ++merged[input[i]];
    }
    double separate = 0;
    for (auto &h : hist) {
      separate += context_cost(h);
    }
    if (separate >= context_cost(merged)) {
      contexts = {};
    }
  }
  const unsigned nc = contexts.empty() ? 1 : ProfileCount;
  std::array<unsigned, ProfileCount> ids{}, counts{};
  if (contexts.empty()) {
    counts[0] = input.size();
  } else {
    for (auto c : contexts) {
      ++counts[c];
    }
  }
  unsigned active = 0;
  for (unsigned c = 0; c < nc; ++c) {
    if (counts[c]) {
      ids[c] = active++;
    }
  }
  // Only these boundaries are candidates for tree splits. Aggregate history
  // directly into their intervals instead of constructing 256 histograms.
  constexpr unsigned edges[] = {0,   1,   2,   4,   8,   16,  32,  64,  96, 128,
                                160, 192, 224, 240, 248, 252, 254, 255, 256};
  constexpr unsigned bins = std::size(edges) - 1;
  static const auto bucket = [=] {
    std::array<uint8_t, 256> out{};
    for (unsigned b = 0; b < bins; ++b) {
      for (unsigned p = edges[b]; p < edges[b + 1]; ++p) {
        out[p] = uint8_t(b);
      }
    }
    return out;
  }();
  std::vector<std::array<uint32_t, 256>> h(active * bins);
  std::vector<uint32_t> prior_counts(active * bins, 0);
  uint8_t previous = 0, pc = 255;
  for (size_t i = 0; i < input.size(); ++i) {
    unsigned c = contexts.empty() ? 0 : contexts[i];
    if (c != pc) {
      previous = 0;
      pc = c;
    }
    h[ids[c] * bins + bucket[previous]][input[i]]++;
    prior_counts[ids[c] * bins + bucket[previous]]++;
    previous = input[i];
  }
  std::vector<Model> models;
  std::vector<uint16_t> map(nc * 256);
  Bytes out{uint8_t(nc)};
  for (unsigned c = 0; c < nc; ++c) {
    ContextTree root;
    root.end = bins;
    if (counts[c]) {
      for (unsigned p = 0; p < bins; ++p) {
        if (prior_counts[ids[c] * bins + p]) {
          for (unsigned b = 0; b < 256; ++b) {
            root.hist[b] += h[ids[c] * bins + p][b];
          }
        }
      }
    }
    root.cost = context_cost(root.hist);
    std::vector<ContextTree> leaves{root};
    for (size_t k = 0; counts[c] && k < leaves.size(); ++k) {
      auto cur = leaves[k];
      double saving = 0;
      unsigned split = 0;
      ContextTree left, right, bestleft, bestright;
      right = cur;
      left.begin = cur.begin;
      // Update entropy sums only for symbols moved across this split.
      uint32_t ln = 0, rn = 0, lu = 0, ru = 0;
      double le = 0, re = 0;
      for (auto v : cur.hist) {
        rn += v;
        ru += v != 0;
        re += count_entropy(v);
      }

      for (unsigned p = cur.begin; p + 1 < cur.end; ++p) {
        if (!prior_counts[ids[c] * bins + p]) {
          continue;
        }
        for (unsigned b = 0; b < 256; ++b) {
          uint32_t v = h[ids[c] * bins + p][b];
          if (!v) {
            continue;
          }
          auto &l = left.hist[b];
          auto &r = right.hist[b];
          lu += l == 0;
          le += count_entropy(l + v) - count_entropy(l);
          re += count_entropy(r - v) - count_entropy(r);
          l += v;
          r -= v;
          ru -= r == 0;
          ln += v;
          rn -= v;
        }
        unsigned s = p + 1;
        double lc = count_entropy(ln) - le + 16 + lu * 19,
               rc = count_entropy(rn) - re + 16 + ru * 19, gain = cur.cost - lc - rc;
        if (gain > saving + 1e-9) {
          saving = gain;
          split = s;
          bestleft = left;
          bestright = right;
          bestleft.cost = lc;
          bestright.cost = rc;
        }
      }
      if (split && leaves.size() < 16) {
        bestleft.end = split;
        bestright.begin = split;
        leaves[k] = bestleft;
        leaves.insert(leaves.begin() + k + 1, bestright);
        --k;
      }
    }
    out.push_back(uint8_t(leaves.size()));
    for (auto &leaf : leaves) {
      out.push_back(uint8_t(edges[leaf.end] - 1));
      Model m;
      m.hist = leaf.hist;
      m.normalize();
      for (unsigned p = edges[leaf.begin]; p < edges[leaf.end]; ++p) {
        map[c * 256 + p] = uint16_t(models.size());
      }
      unsigned used = 0;
      for (auto f : m.freq) {
        used += f != 0;
      }
      varint(out, used);
      for (unsigned b = 0; b < 256; ++b) {
        if (m.freq[b]) {
          out.push_back(b);
          varint(out, m.freq[b]);
        }
      }
      models.push_back(m);
    }
  }
  uint32_t state[4] = {1u << 23, 1u << 23, 1u << 23, 1u << 23};
  Bytes bytes;
  for (size_t i = input.size(); i-- > 0;) {
    unsigned c = contexts.empty() ? 0 : contexts[i],
             prev = i && (contexts.empty() || contexts[i - 1] == c) ? input[i - 1] : 0;
    auto &m = models[map[c * 256 + prev]];
    uint32_t f = m.freq[input[i]], &x = state[i & 3];
    while (x >= ((1u << 23 >> 12) << 8) * f) {
      bytes.push_back(uint8_t(x));
      x >>= 8;
    }
    x = x / f * 4096 + x % f + m.start[input[i]];
  }
  for (auto x : state) {
    u32(out, x);
  }
  out.insert(out.end(), bytes.rbegin(), bytes.rend());
  return out;
}
inline Bytes context_decode(Reader &r, uint32_t n, View contexts) {
  unsigned nc = r.byte();
  require(nc == 1 || nc == ProfileCount, "context class count");
  require(nc == 1 || contexts.size() == n, "context dimensions");
  std::vector<Model> models;
  std::vector<uint16_t> map(nc * 256);
  // One load supplies the symbol, frequency-minus-one, and CDF offset.
  // Each occupies 8/12/12 bits respectively.
  std::vector<uint32_t> lookup;
  for (unsigned c = 0; c < nc; ++c) {
    unsigned count = r.byte();
    require(count && count <= 16, "context tree size");
    unsigned begin = 0;
    for (unsigned j = 0; j < count; ++j) {
      unsigned end = unsigned(r.byte()) + 1;
      require(end > begin, "context tree range");
      for (unsigned p = begin; p < end; ++p) {
        map[c * 256 + p] = uint16_t(models.size());
      }
      begin = end;
      Model m;
      unsigned used = r.var();
      require(used && used <= 256, "context alphabet");
      for (unsigned k = 0; k < used; ++k) {
        unsigned b = r.byte(), f = r.var();
        require(f && f <= 4096 && !m.freq[b], "context frequency");
        m.freq[b] = uint16_t(f);
      }
      unsigned sum = 0;
      for (unsigned b = 0; b < 256; ++b) {
        m.start[b] = sum;
        sum += m.freq[b];
        require(sum <= 4096, "context sum");
        size_t at = lookup.size();
        lookup.resize(at + m.freq[b]);
        uint32_t base = b | ((uint32_t(m.freq[b]) - 1) << 20);
        for (unsigned j = 0; j < m.freq[b]; ++j) {
          lookup[at + j] = base | (j << 8);
        }
      }
      require(sum == 4096, "context sum");
      models.push_back(m);
    }
    require(begin == 256, "context tree coverage");
  }
  uint32_t state[4];
  for (auto &x : state) {
    x = r.word();
    require(x >= (1u << 23) && x < (1u << 31), "context state");
  }
  Bytes out(n);
  uint8_t previous = 0, pc = 255;
  for (size_t i = 0; i < n; ++i) {
    unsigned c = nc == 1 ? 0 : contexts[i];
    require(c < nc, "context ID");
    if (c != pc) {
      previous = 0;
      pc = c;
    }
    unsigned m = map[c * 256 + previous];
    uint32_t &x = state[i & 3], slot = x & 4095;
    uint32_t entry = lookup[m * 4096 + slot];
    uint8_t b = uint8_t(entry);
    out[i] = b;
    previous = b;
    x = ((entry >> 20) + 1) * (x >> 12) + ((entry >> 8) & 4095);
    while (x < (1u << 23)) {
      x = (x << 8) | r.byte();
    }
  }
  for (auto x : state) {
    require(x == (1u << 23), "context terminal state");
  }
  return out;
}

} // namespace slippack_custom
