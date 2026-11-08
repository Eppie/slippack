// Two presence decisions per ANS symbol. Tables are derived deterministically
// from the compact binary model, so no extra conditional histograms are stored.
inline unsigned reverse2(unsigned v) { return ((v & 1) << 1) | ((v & 2) >> 1); }
inline unsigned above4(View masks, size_t start, size_t i, size_t width) {
  size_t byte = i / 8;
  unsigned v = (byte ? masks[start + byte - 1] : 0) | (unsigned(masks[start + byte]) << 8) |
               (byte + 1 < width ? unsigned(masks[start + byte + 1]) << 16 : 0);
  return (v >> (7 + (i & 7))) & 15;
}
struct PresenceTable {
  std::array<uint16_t, 4> freq{}, start{};
  void build(unsigned key, const std::array<uint16_t, 256>& probs) {
    unsigned cls = (key >> 8) * 128;
    unsigned sum = 0, max = 0;
    for (unsigned s = 0; s < 4; ++s) {
      unsigned h = key & 15, a = (key >> 4) & 15;
      uint64_t weight = 1;
      for (unsigned k = 0; k < 2; ++k) {
        unsigned bit = (s >> k) & 1, p = probs[cls + (h & 15) + (a & 7) * 16];
        weight *= bit ? 4096 - p : p;
        h = h * 2 + bit;
        a >>= 1;
      }
      freq[s] = weight ? std::max<uint64_t>(1, weight >> 12) : 0;
      sum += freq[s];
      if (freq[s] > freq[max]) max = s;
    }
    if (sum <= 4096)
      freq[max] += 4096 - sum;
    else {
      unsigned need = sum - 4096;
      for (unsigned s = 0; need && s < 4; ++s) {
        unsigned take = std::min<unsigned>(need, freq[s] ? freq[s] - 1 : 0);
        freq[s] -= take;
        need -= take;
      }
      require(!need, "presence normalization");
    }
    sum = 0;
    for (unsigned s = 0; s < 4; ++s) {
      start[s] = sum;
      sum += freq[s];
    }
    require(sum == 4096, "presence table");
  }
  unsigned symbol(unsigned slot) const {
    return slot < start[2] ? (slot < start[1] ? 0 : 1) : (slot < start[3] ? 2 : 3);
  }
};
inline Bytes encode_presence(View masks, uint32_t count, View profiles, View refs) {
  size_t planes = profiles.size(), width = (size_t(count) + 7) / 8;
  std::array<std::array<uint32_t, 2>, 256> hist{};
  std::vector<uint16_t> trace;
  trace.reserve((size_t(count) + 1) / 2 * planes);
  for (size_t j = 0; j < planes; ++j) {
    size_t src = refs[j] ? (j - refs[j]) * width : 0;
    unsigned history = 0, cls = profiles[j] == FloatLow ? 128 : 0;
    for (size_t byte = 0; byte < width; ++byte) {
      unsigned above = refs[j] ? ((byte ? masks[src + byte - 1] >> 7 : 0) |
          (unsigned(masks[src + byte]) << 1) |
          (byte + 1 < width ? unsigned(masks[src + byte + 1]) << 9 : 0)) : 0;
      unsigned current = masks[j * width + byte];
      unsigned pairs = std::min<size_t>(4, (count - byte * 8 + 1) / 2);
      for (unsigned k = 0; k < pairs; ++k) {
        unsigned s = current & 3;
        unsigned key = history | ((above & 15) << 4) | (cls << 1);
        trace.push_back(key | (s << 9));
        ++hist[cls + history + (above & 7) * 16][s & 1];
        history = ((history << 1) | (s & 1)) & 15;
        ++hist[cls + history + ((above >> 1) & 7) * 16][s >> 1];
        history = ((history << 1) | (s >> 1)) & 15;
        above >>= 2;
        current >>= 2;
      }
    }
  }
  std::array<std::array<uint16_t, 256>, 2> candidates{};
  double costs[2] = {};
  for (unsigned split = 0; split < 2; ++split)
    for (unsigned c = 0; c < (128u << split); ++c) {
      uint32_t a = hist[c][0] + (split ? 0 : hist[c + 128][0]),
               b = hist[c][1] + (split ? 0 : hist[c + 128][1]);
      unsigned p = a + b ? uint64_t(a) * 4096 / (a + b) : 4096;
      if (a && b) p = std::clamp(p, 1u, 4095u);
      candidates[split][c] = p;
      costs[split] += p < 128 ? 8 : 16;
      if (a) costs[split] += a * std::log2(4096. / p);
      if (b) costs[split] += b * std::log2(4096. / (4096 - p));
    }
  unsigned split = costs[1] < costs[0], nc = 128u << split, kmask = (256u << split) - 1;
  auto& probs = candidates[split];
  Bytes out{uint8_t(split)};
  for (unsigned c = 0; c < nc; ++c) varint(out, probs[c]);
  std::vector<PresenceTable> tables(256u << split);
  std::vector<uint8_t> used(256u << split, 0);
  uint32_t state[4] = {1u << 23, 1u << 23, 1u << 23, 1u << 23};
  Bytes bytes;
  for (size_t t = trace.size(); t-- > 0;) {
    unsigned s = trace[t] >> 9, key = trace[t] & kmask;
    if (!used[key]) {
      used[key] = 1;
      tables[key].build(key, probs);
    }
    auto& m = tables[key];
    uint32_t f = m.freq[s], &x = state[t & 3];
    require(f, "presence frequency");
    while (x >= ((1u << 23 >> 12) << 8) * f) {
      bytes.push_back(uint8_t(x));
      x >>= 8;
    }
    x = x / f * 4096 + x % f + m.start[s];
  }
  for (auto x : state) u32(out, x);
  out.insert(out.end(), bytes.rbegin(), bytes.rend());
  return out;
}
inline Bytes decode_presence(Reader& r, uint32_t count, View profiles, View refs) {
  size_t planes = profiles.size(), width = (size_t(count) + 7) / 8;
  unsigned split = r.byte();
  require(split <= 1, "presence model");
  std::array<uint16_t, 256> probs{};
  for (unsigned c = 0; c < (128u << split); ++c) {
    unsigned x = r.var();
    require(x <= 4096, "presence probability");
    probs[c] = x;
  }
  uint32_t state[4];
  for (auto& x : state) {
    x = r.word();
    require(x >= (1u << 23) && x < (1u << 31), "presence state");
  }
  Bytes masks(width * planes, 0);
  std::vector<PresenceTable> tables(256u << split);
  std::vector<uint8_t> used(256u << split, 0);
  size_t t = 0;
  for (size_t j = 0; j < planes; ++j) {
    size_t src = refs[j] ? (j - refs[j]) * width : 0;
    unsigned history = 0, cls = split && profiles[j] == FloatLow ? 256 : 0;
    for (size_t byte = 0; byte < width; ++byte) {
      unsigned above = refs[j] ? ((byte ? masks[src + byte - 1] >> 7 : 0) |
          (unsigned(masks[src + byte]) << 1) |
          (byte + 1 < width ? unsigned(masks[src + byte + 1]) << 9 : 0)) : 0;
      unsigned value = 0;
      unsigned pairs = std::min<size_t>(4, (count - byte * 8 + 1) / 2);
      for (unsigned k = 0; k < pairs; ++k) {
        unsigned key = history | ((above & 15) << 4) | cls;
        if (!used[key]) {used[key] = 1; tables[key].build(key, probs);}
        auto& m = tables[key];
        uint32_t &x = state[t++ & 3], slot = x & 4095;
        unsigned s = m.symbol(slot);
        x = m.freq[s] * (x >> 12) + slot - m.start[s];
        while (x < (1u << 23)) x = (x << 8) | r.byte();
        value |= s << (k * 2);
        history = ((history << 2) | reverse2(s)) & 15;
        above >>= 2;
      }
      masks[j * width + byte] = uint8_t(value);
    }
  }
  for (auto x : state) require(x == (1u << 23), "presence terminal state");
  return masks;
}
