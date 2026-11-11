// libFuzzer harnesses link the same core modules used by the CLI.
#include "../archive.h"
#include "../slp.h"
#include "../record_codec.h"
#include <cstdio>
using namespace slpz;

#include <cstdlib>
#include <cstring>
#include <string>

#ifndef FUZZ_TARGET
#error                                                                                             \
    "define FUZZ_TARGET=1 (parse), 2 (roundtrip), 3 (dictionary records), or 4 (predictor records)"
#endif

// ── Target 1: parse_slp ─────────────────────────────────────────────────────
#if FUZZ_TARGET == 1
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ParsedSlp out;
  parse_slp(std::span<const uint8_t>(data, size), out); // bool ignored; ASAN watches
  return 0;
}
#endif

// ── Target 2: round-trip oracle ─────────────────────────────────────────────
#if FUZZ_TARGET == 2
namespace {
// Split the fuzz buffer into 1..4 sub-files. Layout: u8 nFiles; then per file
// u16 len (little-endian) + bytes, each clamped to what remains. A buffer that
// doesn't fit the scheme still yields one file (the whole remainder), so a raw
// real-.slp seed round-trips as a single file.
std::vector<std::span<const uint8_t>> split(const uint8_t *data, size_t size) {
  std::vector<std::span<const uint8_t>> files;
  if (size == 0) {
    files.emplace_back(data, (size_t)0);
    return files;
  }
  // A genuine .slp seed passes through whole, so mutated real replays exercise
  // the full parse→transform→reconstruct round-trip as a single file (the
  // header-byte split scheme below would otherwise shred them into raw blobs).
  if (size >= 11 && memcmp(data, kSlpMagic, 11) == 0) {
    files.emplace_back(data, size);
    return files;
  }
  size_t pos = 0;
  int n = 1 + (data[pos++] & 3); // 1..4
  for (int i = 0; i < n && pos < size; i++) {
    size_t len;
    if (i == n - 1 || pos + 2 > size) {
      len = size - pos; // last file (or no room for a length) takes the rest
    } else {
      len = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
      pos += 2;
      len = std::min(len, size - pos);
    }
    files.emplace_back(data + pos, len);
    pos += len;
  }
  if (files.empty()) {
    files.emplace_back(data, size);
  }
  return files;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  auto files = split(data, size);
  std::vector<ArchiveInput> inputs;
  for (size_t i = 0; i < files.size(); ++i) {
    inputs.push_back({"file-" + std::to_string(i),
                      [view = files[i]] { return Bytes(view.begin(), view.end()); }});
  }
  EncodeOptions encode;
  if (const char *level = std::getenv("SLPZ_FUZZ_LEVEL")) {
    encode.level = std::atoi(level);
  }
  auto packed = encode_archive(inputs, encode);
  size_t decoded = 0;
  decode_archive(packed.bytes, {}, [&](DecodedFile file) {
    auto index = size_t(std::stoul(file.name.substr(5)));
    if (index >= files.size() || file.bytes.size() != files[index].size() ||
        !std::equal(file.bytes.begin(), file.bytes.end(), files[index].begin())) {
      std::abort();
    }
    ++decoded;
  });
  if (decoded != files.size()) {
    std::abort();
  }
  return 0;
}
#endif

// Target 3: field dictionary transformations and record framing. Small
// bounded inputs keep mutations focused on dictionary values and indexes.
#if FUZZ_TARGET == 3
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 8) {
    return 0;
  }
  unsigned width = (data[0] & 1) ? 4 : 2;
  unsigned nvals = std::min<size_t>(256, (size - 4) / width);
  unsigned count = 512 + data[1];
  DictField f{0, uint8_t(width), {}, {}};
  for (unsigned i = 0; i < nvals; ++i) {
    uint32_t value = 0;
    memcpy(&value, data + 4 + i * width, width);
    f.vals.push_back(value);
  }
  std::vector<DictField> fields{f}, restored{f};
  uint8_t transform = 1 + (data[2] & 3);
  auto dictionary = dictionary_forward(fields, transform);
  std::fill(restored[0].vals.begin(), restored[0].vals.end(), 0);
  dictionary_reverse(dictionary, transform, restored);
  if (restored[0].vals != f.vals) {
    abort();
  }
  Buf records;
  for (unsigned i = 0; i < count; ++i) {
    auto value = f.vals[data[4 + i % (size - 4)] % nvals];
    records.put_bytes(&value, width);
  }
  Buf packed;
  pack_records(records, count, width, 3, packed, nullptr);
  Reader r{packed.data(), packed.data() + packed.size()};
  std::vector<uint8_t> output;
  unpack_records(r, count, width, output);
  if (r.p != r.end || output != records) {
    abort();
  }
  Reader scanner{packed.data(), packed.data() + packed.size()};
  auto span = skip_packed(scanner, width);
  if (span.size() != packed.size()) {
    abort();
  }
  return 0;
}
#endif

// Target 4: numeric predictors composed with real custom record packets.
// Exercise current predictors across old and new Slippi record sizes.
#if FUZZ_TARGET == 4
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t length) {
  if (length < 4) {
    return 0;
  }
  const uint32_t widths[] = {28, 29, 51, 59, 63, 64, 65, 66, 67, 68, 71, 72, 76, 84};
  uint32_t width = widths[data[1] % std::size(widths)], count = 1 + data[0];
  Buf original;
  original.resize(size_t(width) * count);
  for (size_t i = 0; i < original.size(); ++i) {
    original[i] = data[3 + i % (length - 3)];
  }
  auto records = original;
  motion_predict(records.data(), count, width, false);
  Buf packed;
  pack_records(records, count, width, 3, packed, nullptr, 0x38);
  Reader r{packed.data(), packed.data() + packed.size()};
  std::vector<uint8_t> restored;
  unpack_records(r, count, width, restored);
  if (r.p != r.end || restored != records) {
    abort();
  }
  motion_predict(restored.data(), count, width, true);
  if (restored != original) {
    abort();
  }
  input_predict(restored.data(), count, width, false);
  input_predict(restored.data(), count, width, true);
  if (restored != original) {
    abort();
  }
  return 0;
}
#endif

// Target 5: hostile archive headers, frames, metadata and output paths.
#if FUZZ_TARGET == 5
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  DecodeOptions options;
  options.limits.archive_bytes = 1 << 20;
  options.limits.file_bytes = 1 << 20;
  options.limits.total_bytes = 4 << 20;
  options.limits.frame_bytes = 1 << 20;
  options.limits.pool_bytes = 1 << 20;
  options.limits.metadata_bytes = 1 << 20;
  options.limits.records = 65536;
  options.limits.files = 256;
  options.limits.pools = 256;
  try {
    decode_archive({data, size}, options);
  } catch (const Error &) {
  } // Expected validation failures, never crashes/exits.
  return 0;
}
#endif
