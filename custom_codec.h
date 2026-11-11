#pragma once
#include "codec_types.h"
#include "mask_codec.h"
#include "static_context.h"

namespace slippack_custom {

// Context IDs are derived from the reconstructed plane layout, never stored
// once per literal. A model is serialized only when its actual size wins.
inline Bytes ans_candidate(View input, View contexts) {
  const unsigned nc = contexts.empty() ? 1 : ProfileCount;
  require(contexts.empty() || contexts.size() == input.size(), "custom context shape");
  std::vector<Model> models(nc);
  for (size_t i = 0; i < input.size(); ++i) {
    unsigned c = contexts.empty() ? 0 : contexts[i];
    require(c < nc, "custom context ID");
    models[c].hist[input[i]]++;
  }
  Bytes result{uint8_t(nc == 1 ? 1 : 2)};
  for (auto &m : models) {
    m.normalize();
    unsigned used = 0;
    for (auto f : m.freq) {
      used += f != 0;
    }
    varint(result, used);
    for (unsigned s = 0; s < 256; ++s) {
      if (m.freq[s]) {
        result.push_back(uint8_t(s));
        varint(result, m.freq[s]);
      }
    }
  }
  constexpr uint32_t low = 1u << 23;
  uint32_t states[4] = {low, low, low, low};
  Bytes emitted;
  emitted.reserve(input.size());
  for (size_t i = input.size(); i-- > 0;) {
    auto &m = models[contexts.empty() ? 0 : contexts[i]];
    uint32_t f = m.freq[input[i]], &x = states[i & 3];
    uint32_t limit = ((low >> 12) << 8) * f;
    while (x >= limit) {
      emitted.push_back(uint8_t(x));
      x >>= 8;
    }
    x = (x / f) * 4096 + x % f + m.start[input[i]];
  }
  for (auto x : states) {
    u32(result, x);
  }
  result.insert(result.end(), emitted.rbegin(), emitted.rend());
  return result;
}
inline void channel(Bytes &out, View input, View contexts = {}) {
  Bytes best{0};
  append(best, input);
  if (!input.empty()) {
    auto candidate =
        input.size() >= 2048 ? context_encode(input, contexts) : ans_candidate(input, {});
    if (candidate.size() + (input.size() >= 2048) < best.size()) {
      if (input.size() >= 2048) {
        best = Bytes{4};
        append(best, candidate);
      } else {
        best = std::move(candidate);
      }
    }
  }
  varint(out, narrow(best.size()));
  varint(out, narrow(input.size()));
  append(out, best);
}
inline Bytes unchannel(Reader &outer, size_t limit, View contexts = {}) {
  uint32_t packed = outer.var(), n = outer.var();
  require(n <= limit, "custom channel size");
  Reader r{outer.take(packed)};
  unsigned mode = r.byte();
  if (!mode) {
    auto raw = r.take(n);
    r.done();
    return Bytes(raw.begin(), raw.end());
  }
  if (mode == 4) {
    auto output = context_decode(r, n, contexts);
    r.done();
    return output;
  }
  require(mode == 1 || mode == 2, "custom channel mode");
  unsigned nc = mode == 1 ? 1 : ProfileCount;
  require(mode == 1 || contexts.size() == n, "custom context shape");
  std::vector<Model> models(nc);
  Bytes lookup(nc * 4096);
  for (unsigned c = 0; c < nc; ++c) {
    auto &m = models[c];
    unsigned used = r.var();
    require(used >= 1 && used <= 256, "custom alphabet size");
    for (unsigned j = 0; j < used; ++j) {
      unsigned s = r.byte(), f = r.var();
      require(f && f <= 4096 && !m.freq[s], "custom frequency");
      m.freq[s] = uint16_t(f);
    }
    unsigned at = 0;
    for (unsigned s = 0; s < 256; ++s) {
      m.start[s] = uint16_t(at);
      require(at + m.freq[s] <= 4096, "custom model sum");
      std::memset(lookup.data() + c * 4096 + at, int(s), m.freq[s]);
      at += m.freq[s];
    }
    require(at == 4096, "custom model sum");
  }
  uint32_t states[4];
  for (auto &x : states) {
    x = r.word();
    require(x >= (1u << 23) && x < (1u << 31), "custom ANS state");
  }
  Bytes output(n);
  for (size_t i = 0; i < n; ++i) {
    unsigned c = mode == 1 ? 0 : contexts[i];
    require(c < nc, "custom context ID");
    auto &m = models[c];
    uint32_t &x = states[i & 3];
    unsigned slot = x & 4095, s = lookup[c * 4096 + slot];
    output[i] = uint8_t(s);
    x = m.freq[s] * (x >> 12) + slot - m.start[s];
    while (x < (1u << 23)) {
      x = (x << 8) | r.byte();
    }
  }
  for (auto x : states) {
    require(x == (1u << 23), "custom ANS terminal state");
  }
  r.done();
  return output;
}

struct BitWriter {
  Bytes data;
  uint64_t pending = 0;
  unsigned bits = 0;
  void put(uint32_t n, unsigned count) {
    pending |= uint64_t(n) << bits;
    bits += count;
    while (bits >= 8) {
      data.push_back(uint8_t(pending));
      pending >>= 8;
      bits -= 8;
    }
  }
  void finish() {
    if (bits) {
      data.push_back(uint8_t(pending));
    }
    bits = 0;
    pending = 0;
  }
};
struct BitReader {
  Reader source;
  uint64_t pending = 0;
  unsigned bits = 0;
  uint32_t get(unsigned count) {
    require(count <= 31, "custom bit width");
    while (bits < count) {
      pending |= uint64_t(source.byte()) << bits;
      bits += 8;
    }
    uint32_t n = uint32_t(pending) & uint32_t((uint64_t(1) << count) - 1);
    pending >>= count;
    bits -= count;
    return n;
  }
  void done() const {
    source.done();
    require(pending == 0, "custom padding");
  }
};
inline void number(Bytes &codes, BitWriter &bits, uint32_t n) {
  if (n < 16) {
    codes.push_back(uint8_t(n));
    return;
  }
  unsigned k = 31 - std::countl_zero(n);
  codes.push_back(uint8_t(16 + k - 4));
  bits.put(n - (1u << k), k);
}
inline uint32_t unnumber(Reader &codes, BitReader &bits) {
  unsigned c = codes.byte();
  if (c < 16) {
    return c;
  }
  require(c <= 43, "custom number code");
  unsigned k = c - 16 + 4;
  return (1u << k) + bits.get(k);
}
inline uint8_t context(View profiles, uint32_t stride, size_t position, uint8_t previous) {
  uint8_t p = profiles.empty() ? uint8_t(Generic)
                               : profiles[stride == 1 ? position
                                          : stride    ? position / stride
                                                      : 0];
  return p == Rng && (previous & 128) ? uint8_t(RngContinuation) : p;
}

constexpr int kMatchByteCost = 6;
struct Match {
  uint32_t length = 0, distance = 0;
  int score = 0;
};
struct Matcher {
  View data;
  std::vector<uint32_t> head;
  std::unique_ptr<uint32_t[]> previous;
  std::array<uint32_t, 4096> short_head;
  unsigned depth, hash_shift;
  explicit Matcher(View v, unsigned search)
      : data(v), head(std::bit_ceil(std::clamp<size_t>(v.size() * 2, 64, 65536)), UINT32_MAX),
        previous(new uint32_t[v.size()]), depth(search) {
    hash_shift = 32 - std::countr_zero(head.size());
    short_head.fill(UINT32_MAX);
  }
  unsigned hash(size_t i) const {
    uint32_t x;
    std::memcpy(&x, data.data() + i, 4);
    return (x * 2654435761u) >> hash_shift;
  }
  void insert(uint32_t i) {
    if (size_t(i) + 4 > data.size()) {
      return;
    }
    uint32_t key = uint32_t(data[i]) | uint32_t(data[i + 1]) << 8 | uint32_t(data[i + 2]) << 16;
    short_head[(key * 2654435761u) >> 20] = i;
    unsigned h = hash(i);
    previous[i] = head[h];
    head[h] = i;
  }
  uint32_t length(uint32_t a, uint32_t b) const {
    uint32_t n = 0, max = narrow(data.size() - a);
    while (n + 8 <= max) {
      uint64_t x, y;
      std::memcpy(&x, data.data() + a + n, 8);
      std::memcpy(&y, data.data() + b + n, 8);
      if (x != y) {
        return n + (std::endian::native == std::endian::little ? std::countr_zero(x ^ y)
                                                               : std::countl_zero(x ^ y)) /
                       8;
      }
      n += 8;
    }
    while (n < max && data[a + n] == data[b + n]) {
      ++n;
    }
    return n;
  }
  Match find(uint32_t i, const std::array<uint32_t, 4> &recent) const {
    Match best;
    if (size_t(i) + 4 > data.size()) {
      return best;
    }
    uint32_t prefix;
    std::memcpy(&prefix, data.data() + i, 4);
    auto consider = [&](uint32_t pos, bool repeat) {
      if (pos >= i || i - pos > (1u << 20)) {
        return;
      }
      uint32_t other;
      std::memcpy(&other, data.data() + pos, 4);
      constexpr uint32_t mask =
          std::endian::native == std::endian::little ? 0xffffffu : 0xffffff00u;
      if ((prefix ^ other) & mask) {
        return;
      }
      int cost = repeat ? 3 : int(32 - std::countl_zero(i - pos)) + 7;
      uint32_t need = std::max(3, (best.score + cost) / kMatchByteCost + 1);
      if (need > data.size() - i || data[i + need - 1] != data[pos + need - 1]) {
        return;
      }
      uint32_t n = length(i, pos);
      if (n < need) {
        return;
      }
      int score = int(std::min(n, 1u << 27)) * kMatchByteCost - cost;
      if (score > best.score) {
        best = {n, i - pos, score};
      }
    };
    for (auto distance : recent) {
      if (distance <= i) {
        consider(i - distance, true);
      }
      if (best.length >= 128) {
        break;
      }
    }
    uint32_t key = uint32_t(data[i]) | uint32_t(data[i + 1]) << 8 | uint32_t(data[i + 2]) << 16;
    auto short_pos = short_head[(key * 2654435761u) >> 20];
    if (short_pos < i && i - short_pos < 1024) {
      consider(short_pos, false);
    }
    if (best.length >= 128) {
      return best;
    }
    uint32_t pos = head[hash(i)];
    for (unsigned d = 0; d < depth && pos != UINT32_MAX; ++d) {
      if (i - pos > (1u << 20)) {
        break;
      }
      consider(pos, false);
      if (best.length >= 128) {
        break;
      }
      pos = previous[pos];
    }
    return best;
  }
};
inline void move_recent(std::array<uint32_t, 4> &recent, uint32_t d) {
  unsigned found = 3;
  for (unsigned k = 0; k < 4; ++k) {
    if (recent[k] == d) {
      found = k;
      break;
    }
  }
  for (unsigned k = found; k > 0; --k) {
    recent[k] = recent[k - 1];
  }
  recent[0] = d;
}
inline Bytes lz_encode(View input, View profiles = {}, uint32_t stride = 0, unsigned depth = 8) {
  require(profiles.empty() ||
              (stride ? uint64_t(stride) * profiles.size() == input.size() : profiles.size() == 1),
          "custom field shape");
  for (auto p : profiles) {
    require(p < ProfileCount, "custom profile");
  }
  Bytes literals, residuals, contexts, litlengths, lengths, distances;
  literals.reserve(input.size());
  residuals.reserve(input.size());
  contexts.reserve(input.size());
  litlengths.reserve(input.size() / 8 + 1);
  lengths.reserve(input.size() / 8);
  distances.reserve(input.size() / 8);
  std::array<uint32_t, 256> raw_hist{}, predicted_hist{};
  BitWriter extra;
  Matcher matcher(input, depth);
  std::array<uint32_t, 4> recent{1, 2, 3, 4};
  uint32_t last = 0, matches = 0;
  auto literal_run = [&](uint32_t end) {
    number(litlengths, extra, end - last);
    for (uint32_t p = last; p < end; ++p) {
      literals.push_back(input[p]);
      residuals.push_back(input[p] ^ (p >= recent[0] ? input[p - recent[0]] : 0));
      ++raw_hist[literals.back()];
      ++predicted_hist[residuals.back()];
      contexts.push_back(context(profiles, stride, p, 0));
    }
  };
  uint32_t misses = 0;
  for (uint32_t i = 0; size_t(i) + 3 <= input.size();) {
    Match m = matcher.find(i, recent);
    matcher.insert(i);
    if (m.length < 3) {
      ++misses;
      unsigned step = depth < 24 ? std::min(4u, 1 + (misses >> 6)) : 1;
      i += std::min<uint32_t>(step, narrow(input.size() - i));
      continue;
    }
    misses = 0;
    if (depth >= 4 && m.length < 128) {
      auto next = matcher.find(i + 1, recent);
      if (next.score > m.score + kMatchByteCost) {
        ++i;
        continue;
      }
    }
    literal_run(i);
    number(lengths, extra, m.length - 3);
    unsigned rep = 4;
    for (unsigned k = 0; k < 4; ++k) {
      if (recent[k] == m.distance) {
        rep = k;
        break;
      }
    }
    if (rep < 4) {
      distances.push_back(uint8_t(rep));
    } else {
      unsigned k = 31 - std::countl_zero(m.distance);
      distances.push_back(uint8_t(k + 4));
      extra.put(m.distance - (1u << k), k);
    }
    move_recent(recent, m.distance);
    ++matches;
    uint32_t end = i + m.length;
    for (uint32_t p = i + 1; p < end; ++p) {
      matcher.insert(p);
      if (m.length >= 64 && p == i + 16) {
        p = end - 17;
      }
    }
    i = end;
    last = i;
  }
  literal_run(narrow(input.size()));
  extra.finish();
  Bytes payload;
  varint(payload, matches);
  channel(payload, litlengths);
  channel(payload, lengths);
  channel(payload, distances);
  varint(payload, narrow(extra.data.size()));
  append(payload, extra.data);
  // LZ literals use the semantic class at their original plane position.
  // RNG side streams have their own independent class/model.
  Bytes raw_literals, predicted_literals;
  auto literal_contexts = profiles.empty() ? View{} : View(contexts);
  bool favor_prediction = depth >= 4 && context_cost(predicted_hist) < context_cost(raw_hist);
  if (!favor_prediction || depth >= 24) {
    channel(raw_literals, literals, literal_contexts);
  }
  if (favor_prediction || depth >= 24) {
    channel(predicted_literals, residuals, literal_contexts);
  }
  bool predict = !predicted_literals.empty() &&
                 (raw_literals.empty() || predicted_literals.size() < raw_literals.size());
  payload.push_back(predict ? 1 : 0);
  append(payload, predict ? View(predicted_literals) : View(raw_literals));
  return payload;
}
inline Bytes lz_decode(Reader &r, uint32_t n, View profiles = {}, uint32_t stride = 0) {
  uint32_t matches = r.var();
  require(matches <= n / 3, "custom match count");
  auto ll = unchannel(r, size_t(matches) + 1), ml = unchannel(r, matches),
       ds = unchannel(r, matches);
  require(ll.size() == size_t(matches) + 1 && ml.size() == matches && ds.size() == matches,
          "custom token counts");
  auto extra = r.take(r.var());
  uint8_t predict = r.byte();
  require(predict <= 1, "custom literal predictor");
  Reader peek = r;
  auto literal_packed = peek.var();
  peek.var();
  Reader header{peek.take(literal_packed)};
  unsigned literal_mode = header.byte();
  bool needs_contexts = literal_mode == 2 || (literal_mode == 4 && header.byte() != 1);
  Reader lr{ll}, mr{ml}, dr{ds};
  BitReader bits{{extra}};
  struct Token {
    uint32_t literals, length, distance;
  };
  std::vector<Token> tokens;
  tokens.reserve(size_t(matches) + 1);
  std::array<uint32_t, 4> recent{1, 2, 3, 4};
  size_t pos = 0, total_literals = 0;
  Bytes contexts;
  for (uint32_t i = 0; i <= matches; ++i) {
    uint32_t lit = unnumber(lr, bits);
    require(lit <= n - pos, "custom literal length");
    if (needs_contexts && !profiles.empty()) {
      for (uint32_t j = 0; j < lit; ++j) {
        contexts.push_back(context(profiles, stride, pos + j, 0));
      }
    }
    pos += lit;
    total_literals += lit;
    uint32_t length = 0, distance = 0;
    if (i < matches) {
      uint32_t code = unnumber(mr, bits);
      require(uint64_t(code) + 3 <= n - pos, "custom match length");
      length = code + 3;
      unsigned c = dr.byte();
      require(c <= 24, "custom distance code");
      distance = c < 4 ? recent[c] : (1u << (c - 4)) + bits.get(c - 4);
      require(distance && distance <= pos, "custom distance");
      move_recent(recent, distance);
      pos += length;
    }
    tokens.push_back({lit, length, distance});
  }
  require(pos == n, "custom decoded length");
  bits.done();
  lr.done();
  mr.done();
  dr.done();
  auto literals = unchannel(r, total_literals, contexts);
  require(literals.size() == total_literals, "custom literal count");
  Bytes output(n);
  pos = 0;
  size_t lp = 0;
  uint32_t previous_distance = 1;
  for (auto t : tokens) {
    if (predict) {
      for (uint32_t j = 0; j < t.literals; ++j) {
        size_t at = pos + j;
        output[at] =
            literals[lp + j] ^ (at >= previous_distance ? output[at - previous_distance] : 0);
      }
    } else if (t.literals) {
      std::memcpy(output.data() + pos, literals.data() + lp, t.literals);
    }
    pos += t.literals;
    lp += t.literals;
    if (t.length) {
      if (t.distance == 1) {
        std::memset(output.data() + pos, output[pos - 1], t.length);
      } else {
        size_t first = std::min(t.distance, t.length);
        std::memcpy(output.data() + pos, output.data() + pos - t.distance, first);
        for (size_t done = first; done < t.length;) {
          size_t take = std::min<size_t>(done, t.length - done);
          std::memcpy(output.data() + pos + done, output.data() + pos, take);
          done += take;
        }
      }
      pos += t.length;
      previous_distance = t.distance;
    }
  }
  return output;
}

inline bool is_constant(View input) {
  if (input.empty()) {
    return false;
  }
  const uint64_t word = uint64_t(input[0]) * 0x0101010101010101ULL;
  size_t i = 0;
  for (; i + 8 <= input.size(); i += 8) {
    uint64_t value;
    std::memcpy(&value, input.data() + i, 8);
    if (value != word) {
      return false;
    }
  }
  for (; i < input.size(); ++i) {
    if (input[i] != input[0]) {
      return false;
    }
  }
  return true;
}

// Independent lanes avoid a serial counter dependency on repeated bytes.
// Fill every histogram bin and return the number of adjacent-value runs.
inline uint32_t histogram_runs(View input, std::array<uint32_t, 256> &hist) {
  std::array<std::array<uint32_t, 256>, 8> lanes{};
  uint32_t runs = !input.empty();
  size_t i = 0;
  if (!input.empty()) {
    ++lanes[0][input[0]];
    i = 1;
  }
  for (; input.size() - i >= 8; i += 8) {
    for (unsigned k = 0; k < 8; ++k) {
      ++lanes[k][input[i + k]];
      runs += input[i + k] != input[i + k - 1];
    }
  }
  for (; i < input.size(); ++i) {
    ++lanes[0][input[i]];
    runs += input[i] != input[i - 1];
  }
  for (size_t b = 0; b < hist.size(); ++b) {
    hist[b] = 0;
    for (const auto &lane : lanes) {
      hist[b] += lane[b];
    }
  }
  return runs;
}

// Small specialized modes are selected by encoded byte count, including
// their headers. Raw storage is always an escape for incompressible input.
inline Bytes encode(View input, Profile profile = Generic, unsigned depth = 8) {
  narrow(input.size());
  if (is_constant(input)) {
    return Bytes{1, input[0]};
  }
  Bytes best{0};
  append(best, input);
  if (input.empty()) {
    return best;
  }
  std::array<uint32_t, 256> hist;
  uint32_t runs = histogram_runs(input, hist);
  uint8_t base = uint8_t(std::max_element(hist.begin(), hist.end()) - hist.begin());
  if (hist[base] == input.size()) {
    return Bytes{1, base};
  }
  auto choose = [&](Bytes b) {
    if (b.size() < best.size()) {
      best = std::move(b);
    }
  };
  if (input.size() - hist[base] <= input.size() / 16) {
    Bytes gaps, values;
    size_t previous = 0;
    for (size_t i = 0; i < input.size(); ++i) {
      if (input[i] != base) {
        varint(gaps, narrow(i - previous));
        values.push_back(input[i]);
        previous = i + 1;
      }
    }
    Bytes b{2, base};
    varint(b, narrow(values.size()));
    channel(b, gaps);
    channel(b, values);
    choose(std::move(b));
  }
  if (runs <= input.size() / 8) {
    Bytes lengths, values;
    for (size_t i = 0; i < input.size();) {
      size_t j = i + 1;
      while (j < input.size() && input[j] == input[i]) {
        ++j;
      }
      values.push_back(input[i]);
      varint(lengths, narrow(j - i - 1));
      i = j;
    }
    Bytes b{3};
    varint(b, runs);
    channel(b, lengths);
    channel(b, values);
    choose(std::move(b));
  }
  if (profile == Rng && input.size() >= 512) {
    Bytes b{5};
    channel(b, input);
    choose(std::move(b));
  }
  if (depth && input.size() >= 16) {
    Bytes p{uint8_t(profile)};
    Bytes b{4};
    append(b, lz_encode(input, p, 0, depth));
    choose(std::move(b));
  }
  return best;
}
inline Bytes decode(View packed, uint32_t n, Profile profile = Generic) {
  Reader r{packed};
  unsigned mode = r.byte();
  Bytes output;
  if (mode == 0) {
    auto raw = r.take(n);
    output.assign(raw.begin(), raw.end());
  } else if (mode == 1) {
    require(n != 0, "empty custom constant");
    output.assign(n, r.byte());
  } else if (mode == 2) {
    uint8_t base = r.byte();
    uint32_t count = r.var();
    require(count <= n, "custom sparse count");
    auto gaps = unchannel(r, uint64_t(count) * 5), values = unchannel(r, count);
    require(values.size() == count, "custom sparse values");
    Reader g{gaps};
    output.assign(n, base);
    uint64_t pos = 0;
    for (uint32_t i = 0; i < count; ++i) {
      pos += g.var();
      require(pos < n, "custom sparse position");
      output[size_t(pos++)] = values[i];
    }
    g.done();
  } else if (mode == 3) {
    uint32_t count = r.var();
    require(count && count <= n, "custom run count");
    auto lengths = unchannel(r, uint64_t(count) * 5), values = unchannel(r, count);
    require(values.size() == count, "custom run values");
    Reader l{lengths};
    output.reserve(n);
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t len = uint64_t(l.var()) + 1;
      require(len <= n - output.size(), "custom run length");
      output.insert(output.end(), size_t(len), values[i]);
    }
    l.done();
    require(output.size() == n, "custom run total");
  } else if (mode == 5) {
    output = unchannel(r, n);
    require(output.size() == n, "custom direct size");
  } else if (mode == 4) {
    Bytes p{uint8_t(profile)};
    output = lz_decode(r, n, p);
  } else {
    throw Error("custom payload mode");
  }
  r.done();
  return output;
}

inline uint8_t dominant_byte(View input) {
  size_t zeros = 0, i = 0;
#if defined(__aarch64__) && !defined(SLIPPACK_CUSTOM_SCALAR)
  for (; i + 16 <= input.size(); i += 16) {
    zeros += vaddvq_u8(vshrq_n_u8(vceqq_u8(vld1q_u8(input.data() + i), vdupq_n_u8(0)), 7));
  }
#endif
  for (; i < input.size(); ++i) {
    zeros += input[i] == 0;
  }
  if (zeros * 2 >= input.size()) {
    return 0;
  }
  std::array<uint32_t, 256> h{};
  for (auto b : input) {
    ++h[b];
  }
  return uint8_t(std::max_element(h.begin(), h.end()) - h.begin());
}
inline void restore_exceptions(View masks, View values, uint32_t count, View bases, Bytes &out,
                               std::span<const uint32_t> indexes = {}) {
  size_t width = (size_t(count) + 7) / 8, v = 0;
#if defined(__aarch64__) && !defined(SLIPPACK_CUSTOM_SCALAR)
  struct UnpackTable {
    std::array<std::array<uint8_t, 8>, 256> indexes;
    std::array<uint8_t, 256> counts{};
    UnpackTable() {
      for (unsigned m = 0; m < 256; ++m) {
        indexes[m].fill(255);
        for (unsigned k = 0; k < 8; ++k) {
          if (m & (1u << k)) {
            indexes[m][k] = counts[m]++;
          }
        }
      }
    }
  };
  static const UnpackTable table;
#endif
  for (size_t j = 0; j < bases.size() / 2; ++j) {
    size_t i = 0;
    uint8_t base = bases[j * 2];
    uint8_t *destination = out.data() + size_t(indexes.empty() ? j : indexes[j]) * count;
#if defined(__aarch64__) && !defined(SLIPPACK_CUSTOM_SCALAR)
    auto constant = vdup_n_u8(base), missing = vdup_n_u8(255);
    for (; i + 8 <= count; i += 8) {
      unsigned bits = masks[j * width + i / 8];
      auto ix = vld1_u8(table.indexes[bits].data()),
           expanded = vtbl1_u8(vld1_u8(values.data() + v), ix);
      auto merged = vbsl_u8(vceq_u8(ix, missing), constant, expanded);
      vst1_u8(destination + i, merged);
      v += table.counts[bits];
    }
#endif
    for (; i < count; ++i) {
      destination[i] = (masks[j * width + i / 8] >> (i % 8)) & 1 ? values[v++] : base;
    }
  }
}

inline void extract_exceptions(View plane, uint8_t base, Bytes &mask, Bytes &values) {
  size_t start = values.size(), used = start, i = 0;
  values.resize(start + plane.size() + 8);
#if defined(__aarch64__) && !defined(SLIPPACK_CUSTOM_SCALAR)
  struct PackTable {
    std::array<std::array<uint8_t, 8>, 256> indexes;
    std::array<uint8_t, 256> counts{};
    PackTable() {
      for (unsigned m = 0; m < 256; ++m) {
        indexes[m].fill(255);
        for (unsigned k = 0; k < 8; ++k) {
          if (m & (1u << k)) {
            indexes[m][counts[m]++] = uint8_t(k);
          }
        }
      }
    }
  };
  static const PackTable table;
  const uint8_t weights_data[8] = {1, 2, 4, 8, 16, 32, 64, 128};
  auto weights = vld1_u8(weights_data), constant = vdup_n_u8(base);
  for (; i + 8 <= plane.size(); i += 8) {
    auto v = vld1_u8(plane.data() + i);
    unsigned bits = vaddv_u8(vand_u8(vmvn_u8(vceq_u8(v, constant)), weights));
    mask[i / 8] = uint8_t(bits);
    auto packed = vtbl1_u8(v, vld1_u8(table.indexes[bits].data()));
    vst1_u8(values.data() + used, packed);
    used += table.counts[bits];
  }
#endif
  for (; i < plane.size(); ++i) {
    if (plane[i] != base) {
      mask[i / 8] |= 1u << (i % 8);
      values[used++] = plane[i];
    }
  }
  values.resize(used);
}

// Separate presence masks from exception values so neighboring byte planes
// can share the locations at which a float or controller field changed.
inline Bytes masked_encode(View rich, uint32_t count, View profiles, unsigned depth) {
  Bytes bases, masks, original, values, contexts, refs;
  size_t width = (size_t(count) + 7) / 8;
  values.reserve(rich.size() + 8);
  contexts.reserve(rich.size());
  masks.reserve(width * profiles.size());
  original.reserve(width * profiles.size());
  for (size_t j = 0; j < profiles.size(); ++j) {
    auto plane = rich.subspan(j * count, count);
    uint8_t base = dominant_byte(plane);
    bases.push_back(base);
    Bytes mask(width, 0);
    size_t start = values.size();
    extract_exceptions(plane, base, mask, values);
    contexts.insert(contexts.end(), values.size() - start, profiles[j]);
    unsigned best = 0;
    for (auto v : mask) {
      best += std::popcount(v);
    }
    unsigned ref = 0;
    for (unsigned back = 1; back <= std::min<size_t>(j, 8); ++back) {
      unsigned delta = 0;
      for (size_t i = 0; i < width; ++i) {
        delta += std::popcount(uint8_t(mask[i] ^ original[(j - back) * width + i]));
      }
      if (delta < best) {
        best = delta;
        ref = back;
      }
    }
    bases.push_back(ref);
    refs.push_back(ref);
    for (size_t i = 0; i < width; ++i) {
      masks.push_back(mask[i] ^ (ref ? original[(j - ref) * width + i] : 0));
    }
    append(original, mask);
  }
  Bytes m;
  if (count < 1024 || std::count(profiles.begin(), profiles.end(), FloatLow) < 4) {
    m = encode(masks, Generic, depth);
  }
  auto ma = encode_presence4(original, count, profiles, refs);
  bool binary = m.empty() || ma.size() < m.size();
  if (binary) {
    m = std::move(ma);
  }
  Bytes out = bases;
  out.push_back(binary ? 4 : 0);
  varint(out, narrow(m.size()));
  append(out, m);
  Bytes a = lz_encode(values, contexts, 1, depth);
  out.push_back(0);
  append(out, a);
  return out;
}
// Borrow a fully sized destination, optionally scattering into selected planes.
inline void masked_decode_into(Reader &r, uint32_t count, View profiles, Bytes &destination,
                               std::span<const uint32_t> indexes = {}) {
  require(indexes.empty() || indexes.size() == profiles.size(), "mask destination indexes");
  if (indexes.empty()) {
    require(uint64_t(count) * profiles.size() <= destination.size(), "mask destination size");
  } else {
    for (uint32_t index : indexes) {
      require((uint64_t(index) + 1) * count <= destination.size(), "mask destination plane");
    }
  }
  auto bases = r.take(profiles.size() * 2);
  size_t width = (size_t(count) + 7) / 8;
  unsigned binary = r.byte();
  require(binary == 0 || binary == 2 || binary == 4, "mask coder");
  auto data = r.take(r.var());
  Bytes refs;
  for (size_t j = 0; j < profiles.size(); ++j) {
    require(bases[j * 2 + 1] <= j, "mask reference");
    refs.push_back(bases[j * 2 + 1]);
  }
  Bytes mask;
  if (binary) {
    Reader m{data};
    mask = binary == 4 ? decode_presence4(m, count, profiles, refs)
                       : decode_presence(m, count, profiles, refs);
    m.done();
  } else {
    mask = decode(data, narrow(width * profiles.size()));
  }
  Bytes contexts;
  size_t total = 0;
  for (size_t j = 0; j < profiles.size(); ++j) {
    if (!binary && refs[j]) {
      for (size_t i = 0; i < width; ++i) {
        mask[j * width + i] ^= mask[(j - refs[j]) * width + i];
      }
    }
    if (count % 8) {
      require(!(mask[j * width + width - 1] >> (count % 8)), "mask padding");
    }
    unsigned nonzero = 0;
    for (size_t i = 0; i < width; ++i) {
      nonzero += std::popcount(mask[j * width + i]);
    }
    total += nonzero;
    contexts.insert(contexts.end(), nonzero, profiles[j]);
  }
  unsigned mode = r.byte();
  require(mode <= 1, "mask value coder");
  Bytes values = mode ? unchannel(r, total, contexts) : lz_decode(r, narrow(total), contexts, 1);
  require(values.size() == total, "mask values");
  values.resize(total + 8);
  restore_exceptions(mask, values, count, bases, destination, indexes);
}
// Owning convenience API for a compact plane layout.
inline Bytes masked_decode(Reader &r, uint32_t count, View profiles) {
  Bytes output(narrow(uint64_t(count) * profiles.size()));
  masked_decode_into(r, count, profiles, output);
  return output;
}

// Any symbol occupying all but limit bytes must be the majority in a
// prefix of 2*limit+1 bytes. Check that candidate and the run threshold in
// bounded blocks, stopping as soon as neither specialized mode can qualify.
inline bool sparse_or_few_runs(View input) {
  if (input.empty()) {
    return false;
  }
  const size_t limit = input.size() / 128;
  uint8_t candidate = 0;
  unsigned balance = 0;
  for (size_t i = 0; i < 2 * limit + 1; ++i) {
    if (!balance) {
      candidate = input[i];
    }
    if (input[i] == candidate) {
      ++balance;
    } else {
      --balance;
    }
  }
  size_t exceptions = input[0] != candidate, runs = 1;
  for (size_t i = 1; i < input.size();) {
    const size_t end = std::min(i + 64, input.size());
    for (; i < end; ++i) {
      exceptions += input[i] != candidate;
      runs += input[i] != input[i - 1];
    }
    if (exceptions > limit && runs > limit) {
      return false;
    }
  }
  return exceptions <= limit || runs <= limit;
}

// Preserve semantic plane boundaries. Very cheap planes get explicit modes;
// remaining planes share an LZ dictionary with field-conditioned literals.
inline Bytes encode_planes(View data, uint32_t count, View profiles, unsigned depth = 8) {
  require(uint64_t(count) * profiles.size() == data.size(), "custom plane dimensions");
  Bytes result;
  varint(result, 65536);
  varint(result, narrow(profiles.size()));
  varint(result, count);
  Bytes rich, rich_profiles;
  for (size_t j = 0; j < profiles.size(); ++j) {
    require(profiles[j] < ProfileCount, "custom profile");
    auto plane = data.subspan(j * count, count);
    Bytes special;
    if (is_constant(plane)) {
      special = Bytes{1, plane[0]};
    } else if (count) {
      // A cheap screen avoids full histograms for clearly rich planes.
      // Borderline planes still get the exact sparse/run-mode trial.
      bool possible = true;
      if (count >= 1024) {
        uint32_t hist[256] = {}, runs = 0;
        for (unsigned at : {0u, (count - 64) / 2, count - 64}) {
          for (unsigned i = 0; i < 64; ++i) {
            uint8_t v = plane[at + i];
            ++hist[v];
            runs += i == 0 || v != plane[at + i - 1];
          }
        }
        possible = *std::max_element(hist, hist + 256) >= 172 || runs <= 24;
      }
      if (possible && sparse_or_few_runs(plane)) {
        special = encode(plane, Profile(profiles[j]), 0);
      }
    }
    if (!special.empty() && special[0] >= 1 && special[0] <= 3) {
      result.push_back(1);
      varint(result, narrow(special.size()));
      append(result, special);
    } else {
      result.push_back(0);
      result.push_back(profiles[j]);
      append(rich, plane);
      rich_profiles.push_back(profiles[j]);
    }
  }
  if (!rich.empty()) {
    Bytes winner;
    uint8_t mode = 0;
    // Large float/controller packets favor the shared presence representation.
    // Retain the direct trial for controller packets at the deepest preset.
    bool mask_only =
        rich.size() >= 32768 &&
        (std::count(rich_profiles.begin(), rich_profiles.end(), FloatLow) >= 4 ||
         (depth < 24 && std::count(rich_profiles.begin(), rich_profiles.end(), Controller) >= 4));
    if (!mask_only) {
      winner = lz_encode(rich, rich_profiles, count, depth);
    }

    auto a = masked_encode(rich, count, rich_profiles, depth);
    if (winner.empty() || a.size() < winner.size()) {
      winner = std::move(a);
      mode = 3;
    }
    result.push_back(mode);
    append(result, winner);
  }
  return result;
}
inline Bytes decode_planes(View packed, uint32_t expected) {
  Reader r{packed};
  uint32_t planes = r.var();
  bool extended = planes == 65536;
  if (extended) {
    planes = r.var();
  }
  uint32_t count = r.var();
  require(uint64_t(planes) * count == expected && planes <= 65535, "custom plane dimensions");
  Bytes output(expected), profiles;
  std::vector<uint32_t> indexes;
  for (uint32_t j = 0; j < planes; ++j) {
    unsigned mode = r.byte();
    if (mode == 1) {
      auto part = r.take(r.var());
      auto v = decode(part, count);
      std::copy(v.begin(), v.end(), output.begin() + size_t(j) * count);
    } else {
      require(mode == 0, "custom plane mode");
      uint8_t p = r.byte();
      require(p < ProfileCount, "custom profile");
      profiles.push_back(p);
      indexes.push_back(j);
    }
  }
  if (count && !profiles.empty()) {
    uint32_t n = narrow(uint64_t(count) * profiles.size());
    unsigned mode = extended ? r.byte() : 0;
    require(mode == 0 || mode == 3, "custom plane coder");
    Bytes rich;
    if (mode == 0) {
      rich = lz_decode(r, n, profiles, count);
    } else if (mode == 3) {
      masked_decode_into(r, count, profiles, output, indexes);
    }
    if (mode == 0) {
      for (size_t j = 0; j < indexes.size(); ++j) {
        std::copy_n(rich.data() + j * count, count, output.data() + size_t(indexes[j]) * count);
      }
    }
  }
  r.done();
  return output;
}
} // namespace slippack_custom
