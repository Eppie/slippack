// Exercise dictionary transforms, vector lookup boundaries, framing, and
// malformed metadata in the actual record decoder (including its scanner).
#include "../record_codec.h"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <random>
using namespace slpz;

static size_t checks = 0;
static void check(bool ok, const char *why) {
  if (!ok) {
    fprintf(stderr, "record test: %s\n", why);
    abort();
  }
  ++checks;
}

template <class F> static void rejects(F fn) {
  try {
    fn();
  } catch (const Error &) {
    ++checks;
    return;
  }
  check(false, "malformed metadata accepted");
}

int main() {
  std::mt19937 random(718091);
  for (unsigned width : {2u, 4u}) {
    for (unsigned nvals : {1u, 2u, 15u, 16u, 17u, 63u, 64u, 65u, 255u, 256u}) {
      DictField field{0, uint8_t(width), {}, {}};
      for (unsigned v = 0; v < nvals; ++v) {
        field.vals.push_back(random() & (width == 2 ? 0xffffu : 0xffffffffu));
      }
      for (unsigned count : {1u, 7u, 15u, 16u, 17u, 31u, 64u, 257u, 1025u}) {
        field.idx.resize(count);
        for (auto &v : field.idx) {
          v = random() % nvals;
        }
        std::vector<uint8_t> expected(count * width), actual(expected.size());
        for (unsigned b = 0; b < width; ++b) {
          for (unsigned i = 0; i < count; ++i) {
            expected[b * count + i] = uint8_t(field.vals[field.idx[i]] >> (b * 8));
          }
        }
        if (width == 2) {
          expand_dictionary<2>(field, field.idx.data(), count, actual.data());
        } else {
          expand_dictionary<4>(field, field.idx.data(), count, actual.data());
        }
        check(actual == expected, "dictionary expansion");
        std::vector<DictField> sorted{field};
        sort_dictionaries(sorted);
        for (unsigned i = 0; i < count; ++i) {
          check(sorted[0].vals[sorted[0].idx[i]] == field.vals[field.idx[i]], "sort/remap");
        }
      }
      for (uint8_t transform = 1; transform <= 4; ++transform) {
        std::vector<DictField> fields{field}, restored{field};
        auto bytes = dictionary_forward(fields, transform);
        std::fill(restored[0].vals.begin(), restored[0].vals.end(), 0);
        dictionary_reverse(bytes, transform, restored);
        check(restored[0].vals == field.vals, "dictionary transform");

        // Force this transform in a complete record packet; selection policy
        // must not decide whether a decoder branch gets tested.
        Buf packet;
        packet.put_u8(1);
        packet.put_u8(width | 128 | (transform << 4));
        packet.put_u8(0);
        packet.put_u8(0);
        packet.put_u8(nvals - 1);
        small_compress(bytes, 3, packet, slpz_custom::Discrete);
        if (!implicit_dictionary(field)) {
          packet.put_u8(0);
        }
        packet.put_u8(2); // custom layout
        std::array<uint8_t, 1> profiles{slpz_custom::Discrete};
        if (implicit_dictionary(field)) {
          custom_planes_frame({}, field.idx.size(), {}, 3, packet);
        } else {
          custom_planes_frame(field.idx, field.idx.size(), profiles, 3, packet);
        }
        Reader scanner{packet.data(), packet.data() + packet.size()};
        auto span = skip_packed(scanner, width);
        check(span.size() == packet.size(), "dictionary scanner");
        Reader reader{packet.data(), packet.data() + packet.size()};
        std::vector<uint8_t> records;
        unpack_records(reader, field.idx.size(), width, records);
        check(reader.p == reader.end, "dictionary framing");
        for (size_t i = 0; i < field.idx.size(); ++i) {
          check(memcmp(records.data() + i * width, &field.vals[field.idx[i]], width) == 0,
                "forced transform record output");
        }
        if (nvals == 17) {
          auto fail = [&](Buf broken) {
            rejects([&] {
              Reader r{broken.data(), broken.data() + broken.size()};
              std::vector<uint8_t> out;
              unpack_records(r, field.idx.size(), width, out);
            });
          };
          auto broken = packet;
          broken[1] = width | 0xf0;
          fail(broken); // unknown transform
          broken = packet;
          broken[2] = width;
          fail(broken); // out-of-bounds field
          broken = packet;
          broken[10] ^= 1;
          fail(broken); // wrong dictionary output size
          broken = packet;
          broken.resize(9);
          fail(broken); // truncated metadata frame
        }
      }
      if (nvals < 256 && !implicit_dictionary(field)) {
        auto broken = field.idx;
        broken.back() = nvals;
        rejects([&] {
          std::vector<uint8_t> out(broken.size() * width);
          if (width == 2) {
            expand_dictionary<2>(field, broken.data(), broken.size(), out.data());
          } else {
            expand_dictionary<4>(field, broken.data(), broken.size(), out.data());
          }
        });
      }
    }
  }

  // Mixed fields, 2-byte arithmetic wrap, and reset at field boundaries.
  std::vector<DictField> fields{{0, 2, {65535, 0, 32768, 1}, {}},
                                {2, 4, {0xffffffffu, 0, 0x7fc01234u, 1}, {}}};
  for (uint8_t transform = 1; transform <= 4; ++transform) {
    auto packed = dictionary_forward(fields, transform), original = packed;
    auto result = fields;
    for (auto &f : result) {
      std::fill(f.vals.begin(), f.vals.end(), 0);
    }
    dictionary_reverse(packed, transform, result);
    check(result[0].vals == fields[0].vals && result[1].vals == fields[1].vals, "field resets");
    check(packed == original, "immutable dictionary input");
  }
  // Two fields whose starting offsets differ but whose spans overlap.
  rejects([&] {
    const uint8_t bad[] = {2, 2, 0, 0, 0, 12, 34, 2, 1, 0, 0, 56, 78};
    Reader r{bad, bad + sizeof(bad)};
    std::vector<uint8_t> out;
    unpack_records(r, 64, 4, out);
  });
  printf("record codec: %zu checks passed\n", checks);
}
