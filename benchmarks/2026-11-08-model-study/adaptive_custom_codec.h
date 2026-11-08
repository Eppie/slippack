#pragma once

// SLPZ custom payload codec. No compression-library dependency.
// Four interleaved byte-rANS states, semantic literal contexts, explicit
// constant/sparse/run modes, and a bounded lazy LZ parser.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

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
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
inline void require(bool ok, const char* why) {
  if (!ok) throw Error(why);
}
inline uint32_t narrow(size_t n) {
  require(n <= UINT32_MAX, "custom input too large");
  return uint32_t(n);
}
inline void u32(Bytes& b, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) b.push_back(uint8_t(n >> (8 * i)));
}
inline void varint(Bytes& b, uint32_t n) {
  while (n >= 128) {
    b.push_back(uint8_t(n | 128));
    n >>= 7;
  }
  b.push_back(uint8_t(n));
}
inline void append(Bytes& b, View v) { b.insert(b.end(), v.begin(), v.end()); }
struct Reader {
  View data;
  size_t pos = 0;
  size_t remaining() const { return data.size() - pos; }
  uint8_t byte() {
    require(remaining() != 0, "truncated custom data");
    return data[pos++];
  }
  uint32_t word() {
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= uint32_t(byte()) << (8 * i);
    return n;
  }
  uint32_t var() {
    uint32_t n = 0;
    for (unsigned s = 0; s <= 28; s += 7) {
      uint8_t c = byte();
      require(s < 28 || c < 16, "custom varint overflow");
      n |= uint32_t(c & 127) << s;
      if (!(c & 128)) return n;
    }
    throw Error("custom varint overflow");
  }
  View take(size_t n) {
    require(n <= remaining(), "truncated custom span");
    auto v = data.subspan(pos, n);
    pos += n;
    return v;
  }
  void done() const { require(remaining() == 0, "trailing custom data"); }
};

struct Model {
  std::array<uint32_t, 256> hist{};
  std::array<uint16_t, 256> freq{}, start{};
  void normalize() {
    uint64_t total = 0;
    for (auto n : hist) total += n;
    if (!total) {
      freq[0] = 4096;
      return;
    }
    int sum = 0;
    for (unsigned s = 0; s < 256; ++s)
      if (hist[s]) {
        freq[s] = uint16_t(std::max<uint64_t>(1, uint64_t(hist[s]) * 4096 / total));
        sum += freq[s];
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

#include "adaptive_ans.h"

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
  for (auto& m : models) {
    m.normalize();
    unsigned used = 0;
    for (auto f : m.freq) used += f != 0;
    varint(result, used);
    for (unsigned s = 0; s < 256; ++s)
      if (m.freq[s]) {
        result.push_back(uint8_t(s));
        varint(result, m.freq[s]);
      }
  }
  constexpr uint32_t low = 1u << 23;
  uint32_t states[4] = {low, low, low, low};
  Bytes emitted;
  emitted.reserve(input.size());
  for (size_t i = input.size(); i-- > 0;) {
    auto& m = models[contexts.empty() ? 0 : contexts[i]];
    uint32_t f = m.freq[input[i]], &x = states[i & 3];
    uint32_t limit = ((low >> 12) << 8) * f;
    while (x >= limit) {
      emitted.push_back(uint8_t(x));
      x >>= 8;
    }
    x = (x / f) * 4096 + x % f + m.start[input[i]];
  }
  for (auto x : states) u32(result, x);
  result.insert(result.end(), emitted.rbegin(), emitted.rend());
  return result;
}
inline void channel(Bytes& out, View input, View contexts = {}) {
  Bytes best{0};
  append(best, input);
  if (!input.empty()) {
    auto candidate = ans_candidate(input, {});
    if (candidate.size() < best.size()) best = std::move(candidate);
    if (!contexts.empty() && input.size() >= 2048) {
      auto candidate2 = ans_candidate(input, contexts);
      if (candidate2.size() < best.size()) best = std::move(candidate2);
    }
  }
  if(input.size()>=512) {
    Bytes a{3};append(a,adaptive_encode(input,contexts,false));if(a.size()<best.size())best=std::move(a);
  }
  varint(out, narrow(best.size()));
  varint(out, narrow(input.size()));
  append(out, best);
}
inline Bytes unchannel(Reader& outer, size_t limit, View contexts = {}) {
  uint32_t packed = outer.var(), n = outer.var();
  require(n <= limit, "custom channel size");
  Reader r{outer.take(packed)};
  unsigned mode = r.byte();
  if (!mode) {
    auto raw = r.take(n);
    r.done();
    return Bytes(raw.begin(), raw.end());
  }
  if(mode==3){auto output=adaptive_decode(r,n,contexts);r.done();return output;}
  require(mode == 1 || mode == 2, "custom channel mode");
  unsigned nc = mode == 1 ? 1 : ProfileCount;
  require(mode == 1 || contexts.size() == n, "custom context shape");
  std::vector<Model> models(nc);
  Bytes lookup(nc * 4096);
  for (unsigned c = 0; c < nc; ++c) {
    auto& m = models[c];
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
  for (auto& x : states) {
    x = r.word();
    require(x >= (1u << 23) && x < (1u << 31), "custom ANS state");
  }
  Bytes output(n);
  for (size_t i = 0; i < n; ++i) {
    unsigned c = mode == 1 ? 0 : contexts[i];
    require(c < nc, "custom context ID");
    auto& m = models[c];
    uint32_t& x = states[i & 3];
    unsigned slot = x & 4095, s = lookup[c * 4096 + slot];
    output[i] = uint8_t(s);
    x = m.freq[s] * (x >> 12) + slot - m.start[s];
    while (x < (1u << 23)) x = (x << 8) | r.byte();
  }
  for (auto x : states) require(x == (1u << 23), "custom ANS terminal state");
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
    if (bits) data.push_back(uint8_t(pending));
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
inline void number(Bytes& codes, BitWriter& bits, uint32_t n) {
  if (n < 16) {
    codes.push_back(uint8_t(n));
    return;
  }
  unsigned k = 31 - std::countl_zero(n);
  codes.push_back(uint8_t(16 + k - 4));
  bits.put(n - (1u << k), k);
}
inline uint32_t unnumber(Reader& codes, BitReader& bits) {
  unsigned c = codes.byte();
  if (c < 16) return c;
  require(c <= 43, "custom number code");
  unsigned k = c - 16 + 4;
  return (1u << k) + bits.get(k);
}
inline uint8_t context(View profiles, uint32_t stride, size_t position, uint8_t previous) {
  uint8_t p = profiles.empty() ? uint8_t(Generic) : profiles[stride ? position / stride : 0];
  return p == Rng && (previous & 128) ? uint8_t(RngContinuation) : p;
}

struct Match {
  uint32_t length = 0, distance = 0;
  int score = 0;
};
struct Matcher {
  View data;
  std::vector<uint32_t> head, previous;
  unsigned depth;
  explicit Matcher(View v, unsigned search)
      : data(v), head(65536, UINT32_MAX), previous(v.size(), UINT32_MAX), depth(search) {}
  unsigned hash(size_t i) const {
    uint32_t x = uint32_t(data[i]) | uint32_t(data[i + 1]) << 8 | uint32_t(data[i + 2]) << 16;
    return (x * 2654435761u) >> 16;
  }
  void insert(uint32_t i) {
    if (size_t(i) + 3 > data.size()) return;
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
      if (x != y) break;
      n += 8;
    }
    while (n < max && data[a + n] == data[b + n]) ++n;
    return n;
  }
  Match find(uint32_t i, const std::array<uint32_t, 4>& recent) const {
    Match best;
    if (size_t(i) + 3 > data.size()) return best;
    auto consider = [&](uint32_t pos, bool repeat) {
      if (pos >= i || i - pos > (1u << 20)) return;
      uint32_t n = length(i, pos);
      if (n < 3) return;
      int cost = repeat ? 3 : int(32 - std::countl_zero(i - pos)) + 7;
      int score = int(std::min(n, 1u << 27)) * 5 - cost;
      if (score > best.score) best = {n, i - pos, score};
    };
    for (auto distance : recent)
      if (distance <= i) consider(i - distance, true);
    if (best.length >= 128) return best;
    uint32_t pos = head[hash(i)];
    for (unsigned d = 0; d < depth && pos != UINT32_MAX; ++d) {
      if (i - pos > (1u << 20)) break;
      consider(pos, false);
      if (best.length >= 128) break;
      pos = previous[pos];
    }
    return best;
  }
};
inline void move_recent(std::array<uint32_t, 4>& recent, uint32_t d) {
  unsigned found = 3;
  for (unsigned k = 0; k < 4; ++k)
    if (recent[k] == d) {
      found = k;
      break;
    }
  for (unsigned k = found; k > 0; --k) recent[k] = recent[k - 1];
  recent[0] = d;
}
inline Bytes lz_encode(View input, View profiles = {}, uint32_t stride = 0, unsigned depth = 8) {
  require(profiles.empty() ||
              (stride ? uint64_t(stride) * profiles.size() == input.size() : profiles.size() == 1),
          "custom field shape");
  for (auto p : profiles) require(p < ProfileCount, "custom profile");
  Bytes literals, residuals, contexts, litlengths, lengths, distances;
  BitWriter extra;
  Matcher matcher(input, depth);
  std::array<uint32_t, 4> recent{1, 2, 3, 4};
  uint32_t last = 0, matches = 0;
  auto literal_run = [&](uint32_t end) {
    number(litlengths, extra, end - last);
    for (uint32_t p = last; p < end; ++p) {
      literals.push_back(input[p]);
      residuals.push_back(input[p] ^ (p >= recent[0] ? input[p - recent[0]] : 0));
      contexts.push_back(context(profiles, stride, p, 0));
    }
  };
  for (uint32_t i = 0; size_t(i) + 3 <= input.size();) {
    Match m = matcher.find(i, recent);
    matcher.insert(i);
    if (m.length < 3) {
      ++i;
      continue;
    }
    if (depth >= 4 && m.length < 128) {
      auto next = matcher.find(i + 1, recent);
      if (next.score > m.score + 5) {
        ++i;
        continue;
      }
    }
    literal_run(i);
    number(lengths, extra, m.length - 3);
    unsigned rep = 4;
    for (unsigned k = 0; k < 4; ++k)
      if (recent[k] == m.distance) {
        rep = k;
        break;
      }
    if (rep < 4)
      distances.push_back(uint8_t(rep));
    else {
      unsigned k = 31 - std::countl_zero(m.distance);
      distances.push_back(uint8_t(k + 4));
      extra.put(m.distance - (1u << k), k);
    }
    move_recent(recent, m.distance);
    ++matches;
    uint32_t end = i + m.length;
    for (uint32_t p = i + 1; p < end; ++p) matcher.insert(p);
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
  channel(raw_literals, literals, literal_contexts);
  if (depth >= 4) channel(predicted_literals, residuals, literal_contexts);
  bool predict = !predicted_literals.empty() && predicted_literals.size() < raw_literals.size();
  payload.push_back(predict ? 1 : 0);
  append(payload, predict ? View(predicted_literals) : View(raw_literals));
  return payload;
}
inline Bytes lz_decode(Reader& r, uint32_t n, View profiles = {}, uint32_t stride = 0) {
  uint32_t matches = r.var();
  require(matches <= n / 3, "custom match count");
  auto ll = unchannel(r, size_t(matches) + 1), ml = unchannel(r, matches),
       ds = unchannel(r, matches);
  require(ll.size() == size_t(matches) + 1 && ml.size() == matches && ds.size() == matches,
          "custom token counts");
  auto extra = r.take(r.var());
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
    if (!profiles.empty())
      for (uint32_t j = 0; j < lit; ++j) contexts.push_back(context(profiles, stride, pos + j, 0));
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
  uint8_t predict = r.byte();
  require(predict <= 1, "custom literal predictor");
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
    } else if (t.literals)
      std::memcpy(output.data() + pos, literals.data() + lp, t.literals);
    pos += t.literals;
    lp += t.literals;
    if (t.length) {
      if (t.distance == 1)
        std::memset(output.data() + pos, output[pos - 1], t.length);
      else {
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

// Small specialized modes are selected by encoded byte count, including
// their headers. Raw storage is always an escape for incompressible input.
inline Bytes encode(View input, Profile profile = Generic, unsigned depth = 8) {
  narrow(input.size());
  Bytes best{0};
  append(best, input);
  if (input.empty()) return best;
  std::array<uint32_t, 256> hist{};
  uint32_t runs = 0;
  for (size_t i = 0; i < input.size(); ++i) {
    ++hist[input[i]];
    runs += i == 0 || input[i] != input[i - 1];
  }
  uint8_t base = uint8_t(std::max_element(hist.begin(), hist.end()) - hist.begin());
  if (hist[base] == input.size()) return Bytes{1, base};
  auto choose = [&](Bytes b) {
    if (b.size() < best.size()) best = std::move(b);
  };
  if (input.size() - hist[base] <= input.size() / 16) {
    Bytes gaps, values;
    size_t previous = 0;
    for (size_t i = 0; i < input.size(); ++i)
      if (input[i] != base) {
        varint(gaps, narrow(i - previous));
        values.push_back(input[i]);
        previous = i + 1;
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
      while (j < input.size() && input[j] == input[i]) ++j;
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
  if (input.size() >= 16) {
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
  } else if (mode == 4) {
    Bytes p{uint8_t(profile)};
    output = lz_decode(r, n, p);
  } else
    throw Error("custom payload mode");
  r.done();
  return output;
}

// Preserve semantic plane boundaries. Very cheap planes get explicit modes;
// remaining planes share an LZ dictionary with field-conditioned literals.
inline Bytes encode_planes(View data, uint32_t count, View profiles, unsigned depth = 8) {
  require(uint64_t(count) * profiles.size() == data.size(), "custom plane dimensions");
  Bytes result;
  varint(result,65536);
  varint(result, narrow(profiles.size()));
  varint(result, count);
  Bytes rich, rich_profiles;
  for (size_t j = 0; j < profiles.size(); ++j) {
    require(profiles[j] < ProfileCount, "custom profile");
    auto plane = data.subspan(j * count, count);
    Bytes special;
    if (count) {
      uint32_t counts[256] = {};
      uint32_t runs = 0;
      for (uint32_t i = 0; i < count; ++i) {
        counts[plane[i]]++;
        runs += i == 0 || plane[i] != plane[i - 1];
      }
      uint32_t max = *std::max_element(counts, counts + 256);
      if (max == count || count - max <= count / 128 || runs <= count / 128)
        special = encode(plane, Profile(profiles[j]), depth);
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
    Bytes winner=lz_encode(rich,rich_profiles,count,depth);uint8_t mode=0;
    Bytes contexts;contexts.reserve(rich.size());for(auto profile:rich_profiles)contexts.insert(contexts.end(),count,profile);
    for(bool order2:{false,true}){auto a=adaptive_encode(rich,contexts,order2);if(a.size()<winner.size()){winner=std::move(a);mode=1;}}
    result.push_back(mode);append(result,winner);
  }
  return result;
}
inline Bytes decode_planes(View packed, uint32_t expected) {
  Reader r{packed};
  uint32_t planes=r.var();bool extended=planes==65536;if(extended)planes=r.var();uint32_t count=r.var();
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
    uint32_t n=narrow(uint64_t(count)*profiles.size());
    unsigned mode=extended?r.byte():0;require(mode<=1,"custom plane coder");Bytes rich;
    if(mode==0)rich=lz_decode(r,n,profiles,count);
    else{Bytes contexts;contexts.reserve(n);for(auto p:profiles)contexts.insert(contexts.end(),count,p);rich=adaptive_decode(r,n,contexts);}
    for (size_t j = 0; j < indexes.size(); ++j)
      std::copy_n(rich.data() + j * count, count, output.data() + size_t(indexes[j]) * count);
  }
  r.done();
  return output;
}
}  // namespace slpz_custom
