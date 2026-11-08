// Standalone decoder fuzz target: no compression-library dependency.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "../custom_codec.h"
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 4) return 0;
  uint32_t n = (uint32_t(data[0]) | uint32_t(data[1]) << 8) & 65535;
  try {
    auto payload = slpz_custom::View(data + 4, size - 4);
    if (data[2] & 2) {
      if (payload.size() > 65535) return 0;
      auto profile = slpz_custom::Profile(data[3] % slpz_custom::ProfileCount);
      auto encoded = slpz_custom::encode(payload, profile);
      auto decoded = slpz_custom::decode(encoded, uint32_t(payload.size()), profile);
      if (!std::equal(decoded.begin(), decoded.end(), payload.begin(), payload.end())) std::abort();
    } else if (data[2] & 1)
      (void)slpz_custom::decode_planes(payload, n);
    else
      (void)slpz_custom::decode(payload, n,
                                slpz_custom::Profile(data[3] % slpz_custom::ProfileCount));
  } catch (const slpz_custom::Error&) {
  }
  return 0;
}
