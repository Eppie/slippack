#pragma once
#include "binary_io.h"

namespace slippack {
// Dictionary IDs and their bytes are permanent archive-format dependencies.
constexpr uint32_t kGeckoDictionaryV1 = 1;
View gecko_dictionary(uint32_t id);
Buf gecko_dictionary_encode(View input);
Bytes gecko_dictionary_decode(View tokens, uint32_t id, size_t output_size);
} // namespace slippack
