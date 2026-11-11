// Included inside namespace slpz_custom after Reader/Model definitions.
// Adaptive binary models with four interleaved rANS states. Encoding records
// the forward probability trace and emits it backwards; decoding learns the
// same models forwards. All probability updates use integer arithmetic.
struct AdaptiveBits {
  struct Node {
    uint32_t key = 0;
    uint16_t p = 2048;
    uint8_t count = 0;
  };
  std::vector<uint16_t> zero, one;
  std::vector<uint8_t> seen;
  std::vector<Node> two;
  bool order2;
  uint32_t history = 0;
  uint8_t last_profile = 255;
  uint16_t *p0 = nullptr, *p1 = nullptr;
  uint8_t *n1 = nullptr;
  Node *p2 = nullptr;
  explicit AdaptiveBits(bool second)
      : zero(ProfileCount * 256, 2048), one(ProfileCount * 65536, 0), seen(ProfileCount * 65536, 0),
        two(second ? 1u << 18 : 0), order2(second) {}
  unsigned probability(uint8_t profile, unsigned prefix) {
    if (profile != last_profile) {
      history = 0;
      last_profile = profile;
    }
    p0 = &zero[unsigned(profile) * 256 + prefix];
    size_t key1 = (unsigned(profile) * 256 + (history & 255)) * 256 + prefix;
    p1 = &one[key1];
    n1 = &seen[key1];
    if (!*p1) {
      *p1 = *p0;
    }
    unsigned weight = std::min<unsigned>(*n1, 8);
    unsigned p = (*p1 * weight + *p0 * (8 - weight) + 4) >> 3;
    if (order2) {
      uint32_t key = (uint32_t(profile) << 24) | (history << 8) | prefix;
      uint32_t hash = (key ^ (key >> 13)) * 0x85ebca6bu;
      hash ^= hash >> 16;
      p2 = &two[hash & ((1u << 18) - 1)];
      if (!p2->count || p2->key != key) {
        p2->key = key;
        p2->p = uint16_t(p);
        p2->count = 0;
      }
      unsigned w = std::min<unsigned>(p2->count, 12);
      p = (p2->p * w + p * (16 - w) + 8) >> 4;
    }
    return std::clamp(p, 1u, 4095u);
  }
  static void adjust(uint16_t &p, unsigned bit, unsigned shift) {
    int next = bit ? int(p) - std::max(1, int(p) >> shift)
                   : int(p) + std::max(1, (4096 - int(p)) >> shift);
    p = uint16_t(std::clamp(next, 1, 4095));
  }
  void update(unsigned bit) {
    adjust(*p0, bit, 6);
    adjust(*p1, bit, 5);
    if (*n1 < 255) {
      ++*n1;
    }
    if (order2) {
      adjust(p2->p, bit, 4);
      if (p2->count < 255) {
        ++p2->count;
      }
    }
  }
  void byte(uint8_t b) { history = ((history << 8) | b) & 65535; }
};
inline Bytes adaptive_encode(View input, View contexts, bool order2) {
  require(contexts.empty() || contexts.size() == input.size(), "adaptive contexts");
  AdaptiveBits model(order2);
  std::vector<uint16_t> trace(input.size() * 8);
  for (size_t i = 0; i < input.size(); ++i) {
    unsigned prefix = 1;
    uint8_t c = contexts.empty() ? 0 : contexts[i];
    require(c < ProfileCount, "adaptive profile");
    for (unsigned k = 0; k < 8; ++k) {
      unsigned bit = (input[i] >> (7 - k)) & 1;
      trace[i * 8 + k] = uint16_t(model.probability(c, prefix));
      model.update(bit);
      prefix = prefix * 2 + bit;
    }
    model.byte(input[i]);
  }
  uint32_t states[4] = {1u << 23, 1u << 23, 1u << 23, 1u << 23};
  Bytes emitted;
  emitted.reserve(input.size());
  for (size_t i = trace.size(); i-- > 0;) {
    unsigned bit = (input[i >> 3] >> (7 - (i & 7))) & 1;
    uint32_t p = trace[i], f = bit ? 4096 - p : p, start = bit ? p : 0, &x = states[i & 3];
    uint32_t limit = ((1u << 23 >> 12) << 8) * f;
    while (x >= limit) {
      emitted.push_back(uint8_t(x));
      x >>= 8;
    }
    x = (x / f) * 4096 + x % f + start;
  }
  Bytes out{uint8_t(order2)};
  for (auto x : states) {
    u32(out, x);
  }
  out.insert(out.end(), emitted.rbegin(), emitted.rend());
  return out;
}
inline Bytes adaptive_decode(Reader &r, uint32_t n, View contexts) {
  require(contexts.empty() || contexts.size() == n, "adaptive contexts");
  unsigned method = r.byte();
  require(method <= 1, "adaptive method");
  AdaptiveBits model(method);
  uint32_t states[4];
  for (auto &x : states) {
    x = r.word();
    require(x >= (1u << 23) && x < (1u << 31), "adaptive state");
  }
  Bytes out(n);
  for (size_t i = 0; i < n; ++i) {
    unsigned prefix = 1;
    uint8_t c = contexts.empty() ? 0 : contexts[i];
    require(c < ProfileCount, "adaptive profile");
    for (unsigned k = 0; k < 8; ++k) {
      uint32_t p = model.probability(c, prefix), &x = states[(i * 8 + k) & 3], slot = x & 4095;
      unsigned bit = slot >= p;
      uint32_t f = bit ? 4096 - p : p, start = bit ? p : 0;
      x = f * (x >> 12) + slot - start;
      while (x < (1u << 23)) {
        x = (x << 8) | r.byte();
      }
      model.update(bit);
      prefix = prefix * 2 + bit;
    }
    out[i] = uint8_t(prefix);
    model.byte(out[i]);
  }
  for (auto x : states) {
    require(x == (1u << 23), "adaptive terminal state");
  }
  return out;
}
