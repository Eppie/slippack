#include "gecko_dictionary.h"
#include "gecko_dictionary_data.h"
#include <algorithm>
#include <array>

namespace slippack {
View gecko_dictionary(uint32_t id) {
  require(id == kGeckoDictionaryV1, "unsupported built-in Gecko dictionary");
  return kGeckoV1;
}

namespace {
constexpr unsigned kHashBits = 16, kCandidates = 4, kMinMatch = 12;
uint64_t word64(const uint8_t *p) {
  uint64_t word;
  memcpy(&word, p, sizeof(word));
  return word;
}
unsigned hash8(const uint8_t *p) {
  // Hash bytes in a fixed order, so archives do not depend on host endianness.
  uint64_t word = uint64_t(little_word(p, 4)) | (uint64_t(little_word(p + 4, 4)) << 32);
  return unsigned((word * UINT64_C(0xcf1bbcdcb7a56463)) >> (64 - kHashBits));
}
struct DictionaryIndex {
  std::array<std::array<uint32_t, kCandidates>, 1 << kHashBits> buckets{};
  DictionaryIndex() {
    View data = kGeckoV1;
    for (uint32_t i = 0; i + kMinMatch <= data.size(); i += 4) {
      auto &bucket = buckets[hash8(data.data() + i)];
      for (unsigned j = kCandidates - 1; j; --j) {
        bucket[j] = bucket[j - 1];
      }
      bucket[0] = i + 1; // zero denotes an empty slot
    }
  }
};
} // namespace

Buf gecko_dictionary_encode(View input) {
  static const DictionaryIndex index; // initialized once, safe across encoder threads
  View dict = kGeckoV1;
  Buf tokens;
  size_t pos = 0, literal = 0;
  while (pos + kMinMatch <= input.size()) {
    size_t best = 0;
    uint32_t offset = 0;
    for (uint32_t entry : index.buckets[hash8(input.data() + pos)]) {
      if (!entry) {
        continue;
      }
      uint32_t at = entry - 1;
      if (word64(input.data() + pos) != word64(dict.data() + at)) {
        continue;
      }
      size_t limit = std::min(input.size() - pos, dict.size() - at), length = 8;
      while (length + 8 <= limit &&
             word64(input.data() + pos + length) == word64(dict.data() + at + length)) {
        length += 8;
      }
      while (length < limit && input[pos + length] == dict[at + length]) {
        ++length;
      }
      if (length > best) {
        best = length;
        offset = at;
      }
    }
    if (best < kMinMatch) {
      ++pos;
      continue;
    }
    varint(tokens, narrow(pos - literal));
    append(tokens, input.subspan(literal, pos - literal));
    varint(tokens, narrow(best));
    varint(tokens, offset);
    pos += best;
    literal = pos;
  }
  if (literal < input.size()) {
    varint(tokens, narrow(input.size() - literal));
    append(tokens, input.subspan(literal));
    varint(tokens, 0);
  }
  return tokens;
}

Bytes gecko_dictionary_decode(View tokens, uint32_t id, size_t output_size) {
  auto dict = gecko_dictionary(id);
  Reader r(tokens);
  Bytes output;
  // Every token must make progress. Validate all bounds before copying bytes.
  while (output.size() < output_size) {
    uint32_t literals = r.var();
    require(literals <= output_size - output.size(), "Gecko literals exceed output");
    append(output, r.bytes(literals));
    uint32_t length = r.var();
    require(literals || length, "empty Gecko dictionary token");
    require(length <= output_size - output.size(), "Gecko match exceeds output");
    if (length) {
      uint32_t offset = r.var();
      require(offset <= dict.size() && length <= dict.size() - offset,
              "Gecko match exceeds dictionary");
      append(output, dict.subspan(offset, length));
    }
  }
  r.done();
  return output;
}
} // namespace slippack
