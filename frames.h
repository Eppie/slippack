#pragma once
#include "format.h"
#include "codec_types.h"

namespace slippack {
size_t custom_frame(std::span<const uint8_t> in, Buf &out,
                    slippack_custom::Profile profile = slippack_custom::Generic, int level = 3);
void custom_planes_frame(std::span<const uint8_t> in, uint32_t count,
                         std::span<const uint8_t> profiles, int level, Buf &out);
std::vector<uint8_t> custom_unframe(Reader &r, bool planes = false);
void small_compress(std::span<const uint8_t> in, int level, Buf &blob,
                    slippack_custom::Profile profile = slippack_custom::Generic);
std::vector<uint8_t> small_decompress(Reader &r);

std::span<const uint8_t> skip_frame(Reader &r);
uint32_t frame_size(View frame);
} // namespace slippack
