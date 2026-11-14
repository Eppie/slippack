// Bounded malformed token/record decoding plus an exact encode/decode oracle.
#include "../gecko_dictionary.h"
#include "../record_codec.h"
#include <algorithm>
#include <cstdlib>

using namespace slippack;
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 65536) {
    return 0;
  }
  View input(data, size);
  auto tokens = gecko_dictionary_encode(input);
  auto restored = gecko_dictionary_decode(tokens, 1, size);
  if (!std::equal(input.begin(), input.end(), restored.begin(), restored.end())) {
    abort();
  }
  if (size >= 2) {
    unsigned output_size = data[0] | unsigned(data[1]) << 8;
    try {
      gecko_dictionary_decode(input.subspan(2), 1, output_size);
    } catch (const Error &) {
    }
    try {
      Reader r(input.subspan(2));
      r.output_limit = 65536;
      Bytes output;
      unpack_records(r, output_size, 1, output);
    } catch (const Error &) {
    }
  }
  return 0;
}
