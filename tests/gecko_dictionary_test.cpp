#include "../gecko_dictionary.h"
#include "../record_codec.h"
#include "../platform.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <thread>

using namespace slippack;
static size_t checks = 0;
static void check(bool ok, const char *why) {
  if (!ok) {
    fprintf(stderr, "Gecko dictionary test: %s\n", why);
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
  check(false, "malformed input accepted");
}
static Buf packet(View tokens, uint32_t size, uint32_t id = 1) {
  Buf result{{1, 0, 2}};
  result.put_u32(id);
  result.put_u32(size);
  custom_frame(tokens, result);
  return result;
}

int main() {
  auto dict = gecko_dictionary(1);
  check(dict.size() == 136904, "frozen dictionary size");
  check(sha1(dict) == "effd9d734c18fbc11f048deca012724afd69ebbf", "frozen dictionary identity");
  rejects([] { gecko_dictionary(0); });
  rejects([] { gecko_dictionary(2); });
  // Exercise simultaneous first use of the immutable encoder index.
  std::array<Buf, 8> encoded;
  std::vector<std::thread> threads;
  for (auto &result : encoded) {
    threads.emplace_back([&result, dict] { result = gecko_dictionary_encode(dict); });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  for (auto &result : encoded) {
    check(result == encoded[0], "thread-independent dictionary matching");
    check(gecko_dictionary_decode(result, 1, dict.size()) == Bytes(dict.begin(), dict.end()),
          "complete dictionary reconstruction");
  }
  check(encoded[0].size() < dict.size() / 8, "known payload references");
  std::mt19937 random(81091);
  for (size_t size : {0u, 1u, 7u, 8u, 11u, 12u, 516u, 65537u}) {
    Bytes input(size);
    for (auto &byte : input) {
      byte = random();
    }
    auto tokens = gecko_dictionary_encode(input);
    check(gecko_dictionary_decode(tokens, 1, size) == input, "unknown bytes and empty input");
  }
  // Copy instructions with short edits, insertions, unaligned slices, nonzero
  // packet padding, and multiple pooled lists. No semantic reconstruction may
  // discard bytes from any of those regions.
  Bytes input;
  for (unsigned i = 0; i < 80; ++i) {
    auto chunk = dict.subspan((i * 1393) % (dict.size() - 512), 512);
    append(input, chunk);
    input[input.size() - 1] ^= uint8_t(i);
    input.push_back(1);
    input.push_back(17);
    input.push_back(0x3d);
    input.push_back(i % 7 == 0);
  }
  for (int level : {1, 3, 7, 12}) {
    Buf packed, legacy;
    pack_records(input, 80, 516, level, legacy, nullptr);
    pack_records(input, 80, 516, level, packed, nullptr, 0x10);
    check(packed.size() <= legacy.size(), "dictionary fallback preserves size option");
    check(packed[0] == 1 && packed[1] == 0 && packed[2] == 2, "dictionary packet selected");
    Reader scanner(packed);
    check(skip_packed(scanner, 516, 80).size() == packed.size(), "extended packet scanner");
    scanner.done();
    Reader reader(packed);
    OutputBytes restored;
    unpack_records(reader, 80, 516, restored);
    reader.done();
    check(std::equal(input.begin(), input.end(), restored.begin(), restored.end()),
          "record padding, edits, and pool boundaries");
  }
  // Force both extensions independently of the encoder's size decisions.
  Buf literal{{1, 0, 1}};
  custom_frame(input, literal);
  Buf copy;
  varint(copy, 0);
  varint(copy, 100);
  varint(copy, narrow(dict.size() - 100));
  auto reference = packet(copy, 100);
  for (const auto &packed : {literal, reference}) {
    auto expected = packed[2] == 1 ? input : Bytes(dict.end() - 100, dict.end());
    Reader r(packed);
    Bytes output;
    unpack_records(r, expected.size(), 1, output);
    check(output == expected, "forced extension");
    for (size_t cut = 0; cut < packed.size(); ++cut) {
      rejects([&] {
        Reader short_packet(View(packed.data(), cut));
        skip_packed(short_packet, 1, expected.size());
      });
    }
    rejects([&] {
      Reader r(packed);
      skip_packed(r, 1, expected.size() + 1);
    });
    rejects([&] {
      Reader r(packed);
      r.output_limit = expected.size() - 1;
      skip_packed(r, 1, expected.size());
    });
  }
  for (auto bad : {packet(copy, 100, 2), packet(copy, 1)}) {
    rejects([&] {
      Reader r(bad);
      skip_packed(r, 1);
    });
  }
  auto unknown = reference;
  unknown[2] = 3;
  rejects([&] {
    Reader r(unknown);
    skip_packed(r, 1);
  });
  // Token decoder rejects all overreads, output overruns, zero progress,
  // overflowing varints, and trailing data, including empty-output packets.
  for (Bytes malformed : {Bytes{}, Bytes{0}, Bytes{0, 0}, Bytes{101}, Bytes{0, 101, 0},
                          Bytes{0, 1, 255, 255, 255, 255, 15}, Bytes{255, 255, 255, 255, 16}}) {
    rejects([&] { gecko_dictionary_decode(malformed, 1, 100); });
  }
  copy.push_back(0);
  rejects([&] { gecko_dictionary_decode(copy, 1, 100); });
  rejects([&] { gecko_dictionary_decode(copy, 1, 0); });
  printf("Gecko dictionary: %zu checks passed\n", checks);
}
