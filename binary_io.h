#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace slippack {
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
inline void require(bool ok, const char *why) {
  if (!ok) {
    throw Error(why);
  }
}
[[noreturn]] inline void fail(const std::string &why) {
  throw Error(why);
}
inline uint32_t narrow(size_t n) {
  require(n <= UINT32_MAX, "value exceeds v1's 32-bit field");
  return uint32_t(n);
}
inline void u32(Bytes &b, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) {
    b.push_back(uint8_t(n >> (8 * i)));
  }
}
inline void varint(Bytes &b, uint32_t n) {
  while (n >= 128) {
    b.push_back(uint8_t(n | 128));
    n >>= 7;
  }
  b.push_back(uint8_t(n));
}
inline void append(Bytes &b, View v) {
  if (!v.empty()) {
    b.insert(b.end(), v.begin(), v.end());
  }
}
struct Buf : Bytes {
  void put_u8(uint8_t v) { push_back(v); }
  void put_u32(uint32_t v) { slippack::u32(*this, v); }
  void put_u64(uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) {
      push_back(uint8_t(v >> (8 * i)));
    }
  }
  void put_bytes(const void *p, size_t n) {
    if (n) {
      auto *b = static_cast<const uint8_t *>(p);
      insert(end(), b, b + n);
    }
  }
};
// Read a 2- or 4-byte little-endian field from an already bounded buffer.
inline uint32_t little_word(const uint8_t *bytes, unsigned width) {
  uint32_t value = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8);
  if (width == 4) {
    value |= (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
  }
  return value;
}
// Borrows the input. Copies retain their independent cursor and output limit.
struct Reader {
  const uint8_t *p;
  const uint8_t *end;
  uint64_t output_limit = UINT32_MAX;
  Reader(const uint8_t *first, const uint8_t *last, uint64_t limit = UINT32_MAX)
      : p(first), end(last), output_limit(limit) {}
  Reader(View v) : p(v.data()), end(v.empty() ? v.data() : v.data() + v.size()) {}
  size_t remaining() const { return p == end ? 0 : size_t(end - p); }
  bool avail(size_t n) const { return n <= remaining(); }
  uint8_t byte() {
    require(avail(1), "corrupt data (truncated byte)");
    return *p++;
  }
  uint8_t u8() { return byte(); }
  uint32_t word() {
    auto b = take(4);
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
  }
  uint32_t u32() { return word(); }
  uint64_t u64() {
    uint64_t low = word();
    return low | (uint64_t(word()) << 32);
  }
  uint32_t var() {
    uint32_t n = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
      uint8_t c = byte();
      require(shift < 28 || c < 16, "corrupt data (varint overflow)");
      n |= uint32_t(c & 127) << shift;
      if (!(c & 128)) {
        return n;
      }
    }
    throw Error("corrupt data (varint overflow)");
  }
  View take(size_t n) {
    require(avail(n), "corrupt data (truncated span)");
    View result(p, n);
    if (n) {
      p += n;
    }
    return result;
  }
  View bytes(size_t n) { return take(n); }
  void done() const { require(p == end, "corrupt data (trailing bytes)"); }
};
inline uint32_t get_varint(View bytes, size_t &pos) {
  require(pos <= bytes.size(), "corrupt seed cursor");
  Reader r(bytes.subspan(pos));
  auto value = r.var();
  pos = bytes.size() - r.remaining();
  return value;
}
inline uint32_t be32(const uint8_t *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint16_t be16(const uint8_t *p) {
  return uint16_t(p[0]) << 8 | p[1];
}
// Destinations are filled completely before observation; avoid a redundant zero pass.
template <class T> struct OutputAllocator : std::allocator<T> {
  using value_type = T;
  OutputAllocator() = default;
  template <class U> OutputAllocator(const OutputAllocator<U> &) noexcept {}
  template <class U> void construct(U *p) { ::new (static_cast<void *>(p)) U; }
  template <class U, class... Args>
    requires(sizeof...(Args) > 0)
  void construct(U *p, Args &&...args) {
    std::construct_at(p, std::forward<Args>(args)...);
  }
  template <class U> bool operator==(const OutputAllocator<U> &) const noexcept { return true; }
};
using OutputBytes = std::vector<uint8_t, OutputAllocator<uint8_t>>;

} // namespace slippack
