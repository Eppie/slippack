// Standalone decoder fuzz target: no compression-library dependency.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "../custom_codec.h"
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 4) {
    return 0;
  }
  uint32_t n = (uint32_t(data[0]) | uint32_t(data[1]) << 8) & 65535;
  try {
    auto payload = slippack_custom::View(data + 4, size - 4);
    if (data[2] & 4) {
      if (payload.size() > 65535) {
        return 0;
      }
      unsigned planes = 1 + data[3] % 8, count = unsigned(payload.size()) / planes;
      slippack_custom::Bytes profiles;
      for (unsigned j = 0; j < planes; ++j) {
        profiles.push_back((data[3] + j) % slippack_custom::ProfileCount);
      }
      auto input = payload.first(size_t(count) * planes);
      auto encoded = slippack_custom::encode_planes(input, count, profiles);
      auto decoded = slippack_custom::decode_planes(encoded, uint32_t(input.size()));
      if (!std::equal(decoded.begin(), decoded.end(), input.begin(), input.end())) {
        std::abort();
      }
    } else if (data[2] & 2) {
      if (payload.size() > 65535) {
        return 0;
      }
      auto profile = slippack_custom::Profile(data[3] % slippack_custom::ProfileCount);
      auto encoded = slippack_custom::encode(payload, profile);
      auto decoded = slippack_custom::decode(encoded, uint32_t(payload.size()), profile);
      if (!std::equal(decoded.begin(), decoded.end(), payload.begin(), payload.end())) {
        std::abort();
      }
    } else if (data[2] & 1) {
      (void)slippack_custom::decode_planes(payload, n);
    } else {
      (void)slippack_custom::decode(
          payload, n, slippack_custom::Profile(data[3] % slippack_custom::ProfileCount));
    }
  } catch (const slippack_custom::Error &) {
  }
  return 0;
}
