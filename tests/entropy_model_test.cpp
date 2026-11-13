#include "../custom_codec.h"
#include <cstdio>
#include <random>
#include <numeric>
using namespace slippack_custom;
static size_t check_implicit_contexts() {

  size_t checks = 0;
  auto frame = [](View body, unsigned n) {
    Bytes out;
    varint(out, narrow(body.size()));
    varint(out, n);
    append(out, body);
    return out;
  };
  std::mt19937 rng(176443);
  for (bool allzero : {false, true}) {
    for (unsigned n : {0u, 1u, 2u, 17u, 257u}) {
      Bytes body{8, ProfileCount};
      for (unsigned c = 0; c < ProfileCount; ++c) {
        if (!allzero && (c == 1 || c == 4)) {
          append(body, Bytes{1, 255, 1, uint8_t(c == 1 ? 42 : 255)});
        } else {
          body.push_back(0);
        }
      }
      for (unsigned lane = 0; lane < 4; ++lane) {
        u32(body, 1u << 23);
      }
      auto bytes = frame(body, n);
      Bytes contexts(n), expected(n);
      for (unsigned i = 0; i < n; ++i) {
        auto c = uint8_t(rng() % ProfileCount);
        contexts[i] = c;
        expected[i] = allzero ? 0 : c == 1 ? 42 : c == 4 ? 255 : 0;
      }
      Reader r{bytes};
      require(unchannel(r, n, contexts) == expected, "implicit zero tree fixture");
      r.done();
      ++checks;
      for (size_t cut = 0; cut < bytes.size(); ++cut) {
        try {
          Reader bad{View(bytes).first(cut)};
          (void)unchannel(bad, n, contexts);
          throw std::logic_error("accepted truncation");
        } catch (const Error &) {
          ++checks;
        }
      }
      if (n) {
        contexts[n / 2] = ProfileCount;
        try {
          Reader bad{bytes};
          (void)unchannel(bad, n, contexts);
          throw std::logic_error("accepted invalid semantic class");
        } catch (const Error &) {
          ++checks;
        }
      }
    }
  }
  for (Bytes body : {Bytes{4, 8, 0}, Bytes{5, 8, 0}, Bytes{8, 1, 0}}) {
    auto bytes = frame(body, 0);
    try {
      Reader bad{bytes};
      (void)unchannel(bad, 0);
      throw std::logic_error("accepted forbidden zero tree");
    } catch (const Error &) {
      ++checks;
    }
  }
  Bytes input(8192), labels(8192), packed;
  for (unsigned i = 0; i < input.size(); ++i) {
    labels[i] = i < 4096 ? 1 : 2;
    input[i] = (i < 4096 ? 0 : 128) + (rng() % 16);
  }
  channel(packed, input, labels);
  Reader shape{packed};
  auto body_size = shape.var();
  require(shape.var() == input.size(), "channel length");
  Reader body{shape.take(body_size)};
  require(body.byte() == 8 && body.byte() == ProfileCount, "semantic context selection");
  unsigned defaults = 0;
  for (unsigned c = 0; c < ProfileCount; ++c) {
    unsigned count = body.byte();
    defaults += count == 0;
    for (unsigned leaf = 0; leaf < count; ++leaf) {
      body.byte();
      std::array<uint16_t, 256> freq{};
      read_compact_model(body, freq, 1024);
    }
  }
  require(defaults == 6, "unused classes not implicit");
  Reader r{packed};
  require(unchannel(r, input.size(), labels) == input, "sparse semantic class roundtrip");
  r.done();
  ++checks;
  return checks;
}
static size_t check_packed_presence_tables() {
  std::mt19937 random(735581);
  size_t checks = 0, zero = 0, full = 0;
  for (unsigned pattern = 0; pattern < 68; ++pattern) {
    std::array<uint16_t, 256> probabilities;
    for (auto &p : probabilities) {
      p = pattern == 0   ? 4096
          : pattern == 1 ? 0
          : pattern == 2 ? 1
          : pattern == 3 ? 4095
          : pattern < 20 ? (random() % 2 ? 0 : 4096)
          : pattern < 36 ? (random() % 5 ? 4096 : random() % 4097)
          : pattern < 52 ? (random() % 5 ? 0 : random() % 4097)
                         : random() % 4097;
    }
    for (unsigned key = 0; key < 2048; ++key) {
      NibbleTable reference;
      NibbleEncoderTable packed;
      reference.build(key, probabilities);
      packed.build(key, probabilities);
      for (unsigned symbol = 0; symbol < 16; ++symbol) {
        unsigned frequency = reference.frequency(symbol);
        uint32_t entry = packed.entries[symbol];
        if (!frequency) {
          require(entry == UINT32_MAX, "packed zero-frequency marker");
          ++zero;
        } else {
          require(entry != UINT32_MAX, "valid packed frequency has invalid marker");
          require((entry & 65535) + 1 == frequency, "packed presence frequency");
          require(entry >> 16 == reference.start(symbol), "packed presence CDF");
          require((entry == 65535) == (frequency == 65536), "packed full-frequency marker");
          full += frequency == 65536;
        }
        ++checks;
      }
    }
  }
  require(zero && full, "missing packed frequency boundary coverage");
  return checks + 1;
}

int main() {
  std::mt19937 rng(92118);
  size_t checks = 0;
  auto reject = [&](View data, unsigned scale = 4096) {
    try {
      std::array<uint16_t, 256> decoded{};
      Reader r{data};
      read_compact_model(r, decoded, scale);
      throw std::logic_error("accepted malformed model");
    } catch (const Error &) {
      ++checks;
    }
  };
  for (unsigned scale : {1024u, 4096u}) {
    for (unsigned used : {1u, 2u, 3u, 15u, 16u, 31u, 32u, 33u, 127u, 128u, 129u, 255u, 256u}) {
      for (unsigned trial = 0; trial < 64; ++trial) {
        std::array<uint8_t, 256> alphabet;
        std::iota(alphabet.begin(), alphabet.end(), 0);
        std::shuffle(alphabet.begin(), alphabet.end(), rng);
        std::array<uint16_t, 256> freq{}, decoded{};
        for (unsigned i = 0; i < used; ++i) {
          freq[alphabet[i]] = 1;
        }
        for (unsigned i = used; i < scale; ++i) {
          ++freq[alphabet[trial % 3 == 0 ? 0 : trial % 3 == 1 ? i % used : rng() % used]];
        }
        Bytes packed;
        write_compact_model(packed, freq, scale);
        Reader r{packed};
        decoded.fill(UINT16_MAX);
        read_compact_model(r, decoded, scale);
        r.done();
        require(decoded == freq, "compact model roundtrip");
        ++checks;
        for (size_t cut = 0; cut < packed.size(); ++cut) {
          reject(View(packed).first(cut), scale);
        }
      }
    }
  }
  for (const Bytes &malformed :
       {Bytes{0}, Bytes{129, 2}, Bytes{2, 42, 42}, Bytes{2, 42, 41}, Bytes{2, 0, 1, 13},
        Bytes{2, 0, 1, 12, 0}, Bytes{2, 0, 1, 12, 128, 32}, Bytes{3, 0, 1, 2, 12, 128, 16, 128, 16},
        Bytes{2, 0, 1, 0, 255}, Bytes(33, 0)}) {
    reject(malformed);
  }
  // A fixed byte fixture checks the writer's symbol order and Rice bit order.
  std::array<uint16_t, 256> peaked{};
  peaked[0] = 4094;
  peaked[1] = peaked[255] = 1;
  Bytes peaked_header;
  write_compact_model(peaked_header, peaked);
  require(peaked_header == Bytes({3, 0, 1, 255, 0, 3}), "fixed compact model bytes");
  ++checks;
  // An explicit Rice remainder has four padding bits. None may be set.
  Bytes padded{2, 0, 255, 10, 254, 15};
  std::array<uint16_t, 256> balanced{};
  Reader balanced_reader{padded};
  read_compact_model(balanced_reader, balanced);
  balanced_reader.done();
  require(balanced[0] == 2048 && balanced[255] == 2048, "fixed Rice model");
  padded.back() |= 128;
  reject(padded);
  // Two deterministic leaves alternate symbols across the context boundary.
  // These fixed headers are independent of the current encoder's choices.
  for (unsigned mode : {4u, 5u, 8u}) {
    for (unsigned count : {0u, 1u, 2u, 3u, 4u, 5u, 15u, 16u, 17u, 33u}) {
      Bytes body{uint8_t(mode), 1, 2};
      for (unsigned leaf = 0; leaf < 2; ++leaf) {
        body.push_back(leaf ? 255 : 127);
        body.push_back(1);
        body.push_back(leaf ? 42 : 200);
        if (mode == 4) {
          varint(body, 4096);
        }
      }
      for (unsigned lane = 0; lane < 4; ++lane) {
        u32(body, 1u << 23);
      }
      Bytes framed;
      varint(framed, narrow(body.size()));
      varint(framed, count);
      append(framed, body);
      Reader r{framed};
      Bytes expected(count);
      for (unsigned i = 0; i < count; ++i) {
        expected[i] = i % 2 ? 42 : 200;
      }
      require(unchannel(r, count) == expected, "fixed alternating context leaves");
      r.done();
      ++checks;
    }
  }
  // Independent, fixed v1 models exercise all previously emitted channel modes.
  for (unsigned mode : {1u, 2u, 4u}) {
    for (bool semantic : {false, true}) {
      if ((mode == 1 && semantic) || (mode == 2 && !semantic)) {
        continue;
      }
      unsigned classes = semantic ? ProfileCount : 1;
      Bytes body{uint8_t(mode)}, contexts(33), expected(33);
      if (mode == 4) {
        body.push_back(uint8_t(classes));
      }
      for (unsigned c = 0; c < classes; ++c) {
        if (mode == 4) {
          body.push_back(1);
          body.push_back(255);
        }
        body.push_back(1);
        body.push_back(uint8_t(c + 42));
        body.push_back(128);
        body.push_back(32); // varint 4096
      }
      for (unsigned lane = 0; lane < 4; ++lane) {
        u32(body, 1u << 23);
      }
      for (unsigned i = 0; i < expected.size(); ++i) {
        contexts[i] = uint8_t(i % classes);
        expected[i] = uint8_t(contexts[i] + 42);
      }
      Bytes framed;
      varint(framed, narrow(body.size()));
      varint(framed, narrow(expected.size()));
      append(framed, body);
      Reader r{framed};
      require(unchannel(r, expected.size(), contexts) == expected, "fixed v1 channel fixture");
      r.done();
      ++checks;
    }
  }
  // Independently constructed 10-bit CDF: symbols 0/255 have weights 1/4, 3/4.
  // These frozen bytes exercise renormalization and terminal-state validation.
  for (unsigned mode : {8u, 9u, 10u}) {
    Bytes body{uint8_t(mode)};
    unsigned classes = mode == 10 ? ProfileCount : 1;
    if (mode == 8) {
      append(body, Bytes{1, 1, 255});
    }
    for (unsigned c = 0; c < classes; ++c) {
      append(body, Bytes{2, 0, 255, 12, 128, 6});
    }
    append(body,
           Bytes{51, 204, 179, 0, 0, 51, 243, 44, 232, 217, 134, 0, 94, 216, 134, 0, 0, 0, 0});
    Bytes framed;
    varint(framed, narrow(body.size()));
    varint(framed, 33);
    append(framed, body);
    Bytes expected(33), labels(33);
    for (unsigned i = 0; i < 33; ++i) {
      expected[i] = i % 3 ? 255 : 0;
      labels[i] = i % classes;
    }
    Reader r{framed};
    require(unchannel(r, 33, labels) == expected, "fixed ten-bit channel");
    r.done();
    for (size_t cut = 0; cut < framed.size(); ++cut) {
      try {
        Reader bad{View(framed).first(cut)};
        (void)unchannel(bad, 33, labels);
        throw std::logic_error("accepted truncated ten-bit channel");
      } catch (const Error &) {
        ++checks;
      }
    }
    ++checks;
  }
  for (unsigned n : {1u, 4u, 257u}) {
    Bytes framed;
    varint(framed, 2);
    varint(framed, n);
    append(framed, Bytes{3, 213});
    Reader r{framed};
    require(unchannel(r, n) == Bytes(n, 213), "fixed constant channel");
    r.done();
    ++checks;
  }
  for (const Bytes &bad : {Bytes{2, 0, 3, 42}, Bytes{1, 1, 3}, Bytes{3, 1, 3, 42, 0}}) {
    try {
      Reader r{bad};
      (void)unchannel(r, 1);
      throw std::logic_error("accepted malformed constant channel");
    } catch (const Error &) {
      ++checks;
    }
  }
  // Force all six presence header forms, independent of encoder selection.
  for (unsigned mode = 0; mode < 6; ++mode) {
    unsigned nc = 128u << (mode & 1), coding = mode >> 1;
    std::array<uint16_t, 256> probs;
    probs.fill(4096);
    probs[15] = 0;
    probs[31] = 4095;
    probs[47] = 123;
    Bytes body{uint8_t(mode)}, extras;
    if (coding) {
      body.resize(1 + nc / (coding == 1 ? 4 : 8), 0);
    }
    for (unsigned c = 0; c < nc; ++c) {
      unsigned p = probs[c];
      if (!coding) {
        varint(body, p);
      } else if (coding == 1) {
        unsigned tag = p == 4096 ? 0 : p == 0 ? 1 : p == 4095 ? 2 : 3;
        body[1 + c / 4] |= tag << (2 * (c & 3));
        if (tag == 3) {
          varint(extras, p);
        }
      } else if (p != 4096) {
        body[1 + c / 8] |= 1u << (c & 7);
        varint(extras, p);
      }
    }
    append(body, extras);
    for (unsigned lane = 0; lane < 4; ++lane) {
      u32(body, 1u << 23);
    }
    Bytes labels{Generic, FloatLow, Controller, FloatLow}, refs(4, 0);
    Reader r{body};
    require(decode_presence4(r, 17, labels, refs) == Bytes(12, 0), "fixed presence header");
    r.done();
    ++checks;
    for (size_t cut = 0; cut < body.size(); ++cut) {
      try {
        Reader bad{View(body).first(cut)};
        (void)decode_presence4(bad, 17, labels, refs);
        throw std::logic_error("accepted truncated presence header");
      } catch (const Error &) {
        ++checks;
      }
    }
  }
  for (unsigned trial = 0; trial < 256; ++trial) {
    std::array<uint16_t, 256> probs;
    for (auto &p : probs) {
      p = trial < 64 ? ((rng() & 1) ? 0 : 4096) : rng() % 4097;
    }
    NibbleDecoderTable model;
    model.build(rng() % 2048, probs);
    for (unsigned slot = 0; slot < 65536; ++slot) {
      unsigned expected = 0;
      while (expected < 15 && model.start(expected + 1) <= slot) {
        ++expected;
      }
      require(model.symbol(slot) == expected, "presence CDF oracle");
      require(model.frequency(expected) && slot >= model.start(expected) &&
                  slot - model.start(expected) < model.frequency(expected),
              "presence interval");
      ++checks;
    }
  }
  checks += check_implicit_contexts();
  checks += check_packed_presence_tables();
  std::printf("Verified %zu model and CDF checks.\n", checks);
}
