#include "../custom_codec.h"
#include <cstdio>
#include <random>
#include <numeric>
using namespace slippack_custom;
int main() {
  std::mt19937 rng(92118);
  size_t checks = 0;
  auto reject = [&](View data) {
    try {
      std::array<uint16_t, 256> decoded{};
      Reader r{data};
      read_compact_model(r, decoded);
      throw std::logic_error("accepted malformed model");
    } catch (const Error &) {
      ++checks;
    }
  };
  for (unsigned used : {1u, 2u, 3u, 15u, 16u, 31u, 32u, 33u, 127u, 128u, 129u, 255u, 256u}) {
    for (unsigned trial = 0; trial < 64; ++trial) {
      std::array<uint8_t, 256> alphabet;
      std::iota(alphabet.begin(), alphabet.end(), 0);
      std::shuffle(alphabet.begin(), alphabet.end(), rng);
      std::array<uint16_t, 256> freq{}, decoded{};
      for (unsigned i = 0; i < used; ++i) {
        freq[alphabet[i]] = 1;
      }
      for (unsigned i = used; i < 4096; ++i) {
        ++freq[alphabet[trial % 3 == 0 ? 0 : trial % 3 == 1 ? i % used : rng() % used]];
      }
      Bytes packed;
      write_compact_model(packed, freq);
      Reader r{packed};
      decoded.fill(UINT16_MAX);
      read_compact_model(r, decoded);
      r.done();
      require(decoded == freq, "compact model roundtrip");
      ++checks;
      for (size_t cut = 0; cut < packed.size(); ++cut) {
        reject(View(packed).first(cut));
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
  for (unsigned mode : {4u, 5u}) {
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
  std::printf("Verified %zu model and CDF checks.\n", checks);
}
