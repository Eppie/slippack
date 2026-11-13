#include "frames.h"
#include "custom_codec.h"

namespace slippack {
namespace {
slippack_custom::LzOptions lz_options(int level, bool planes = false) {
  unsigned depth = level >= 9 ? 24 : level >= 6 ? 8 : level >= 4 ? 4 : 2;
  if (level == 3 && planes) {
    return {2, false, true, 2, false};
  }
  // Level 3 keeps lookahead for opaque data, where it prevents large size
  // regressions. Transformed record planes use greedy parsing with prediction.
  return {depth, level >= 3 && !(level == 3 && planes), level >= 3};
}
} // namespace
size_t custom_frame(View in, Buf &out, slippack_custom::Profile profile, int level) {
  narrow(in.size());
  auto packed = slippack_custom::encode(in, profile, lz_options(level));
  out.put_u32(narrow(packed.size() + 1));
  out.put_u32(narrow(in.size()));
  out.put_u8(profile);
  out.put_bytes(packed.data(), packed.size());
  return packed.size() + 1;
}
void custom_planes_frame(View in, uint32_t count, View profiles, int level, Buf &out) {
  narrow(in.size());
  auto packed = slippack_custom::encode_planes(in, count, profiles, lz_options(level, true));
  out.put_u32(narrow(packed.size()));
  out.put_u32(narrow(in.size()));
  out.put_bytes(packed.data(), packed.size());
}
Bytes custom_unframe(Reader &r, bool planes) {
  uint32_t comp = r.u32(), orig = r.u32();
  require(orig <= r.output_limit, "frame exceeds output limit");
  auto packed = r.bytes(comp);
  if (planes) {
    return slippack_custom::decode_planes(packed, orig);
  }
  require(!packed.empty() && packed[0] < slippack_custom::ProfileCount, "corrupt custom profile");
  return slippack_custom::decode(packed.subspan(1), orig, slippack_custom::Profile(packed[0]));
}
void small_compress(View in, int level, Buf &blob, slippack_custom::Profile profile) {
  blob.put_u8(kCustom);
  custom_frame(in, blob, profile, level);
}
Bytes small_decompress(Reader &r) {
  require(r.u8() == kCustom, "corrupt archive (custom codec tag)");
  return custom_unframe(r);
}
View skip_frame(Reader &r) {
  const uint8_t *start = r.p;
  uint32_t comp = r.u32(), orig = r.u32();
  require(orig <= r.output_limit, "frame exceeds output limit");
  r.bytes(comp);
  return {start, size_t(r.p - start)};
}
uint32_t frame_size(View frame) {
  Reader r(frame);
  r.u32();
  return r.u32();
}
} // namespace slippack
