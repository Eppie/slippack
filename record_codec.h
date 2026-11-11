#pragma once
#include "frames.h"
#include <atomic>

namespace slippack {
struct DictField {
  uint16_t offset;
  uint8_t width;              // 2 or 4 bytes
  std::vector<uint32_t> vals; // LE-packed field bytes; sorted by original byte order
  Buf idx;                    // index plane (count bytes)
};

inline bool implicit_dictionary(const DictField &f) {
  return f.vals.size() == 1;
}

// Sort by the original big-endian byte sequence, then remap record indexes.
// Adjacent dictionary entries and temporal index differences become smaller.
void sort_dictionaries(std::vector<DictField> &fields);
Buf dictionary_forward(const std::vector<DictField> &fields, uint8_t transform);
void dictionary_reverse(std::span<const uint8_t> data, uint8_t transform,
                        std::vector<DictField> &fields);
void pack_records(std::span<const uint8_t> recs, uint32_t count, uint32_t size, int level,
                  Buf &blob, std::atomic<uint64_t> *stat, uint8_t cmd = 0);
template <unsigned W>
void expand_dictionary(const DictField &f, const uint8_t *indexes, uint32_t count, uint8_t *output);
template <class RecordBuffer>
void unpack_records(Reader &r, uint32_t count, uint32_t size, RecordBuffer &records);
int cmd_selftest();

std::span<const uint8_t> skip_packed(Reader &r, uint32_t size, uint64_t count = UINT64_MAX);

} // namespace slippack
