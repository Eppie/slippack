// libFuzzer harnesses link the same core modules used by the CLI.
#include "../archive.h"
#include "../slp.h"
#include "../record_codec.h"
#include <cstdio>
using namespace slippack;

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {
// Input-derived choices that leave the meaning of the input bytes unchanged.
[[maybe_unused]] uint64_t choice_bits(const uint8_t *data, size_t size) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ data[i]) * 1099511628211ull;
  }
  return hash ^ hash >> 29;
}

// A listing and a selective decode must agree with a successful full decode:
// same names and sizes, and identical bytes for every selected file.
[[maybe_unused]] void
check_listing_and_selection(View archive, const DecodeOptions &base,
                            const std::vector<std::pair<std::string, Bytes>> &full, uint64_t bits) {
  auto listing = list_archive(archive, base.limits);
  if (listing.entries.size() != full.size()) {
    std::abort();
  }
  // The sink receives files in completion order, so match the listing by name.
  std::map<std::string, const Bytes *> byName;
  uint64_t total = 0;
  for (auto &file : full) {
    byName[file.first] = &file.second;
    total += file.second.size();
  }
  if (byName.size() != full.size()) {
    std::abort();
  }
  for (auto &entry : listing.entries) {
    auto found = byName.find(entry.name);
    if (found == byName.end() || found->second->size() != entry.size) {
      std::abort();
    }
  }
  if (listing.original_bytes != total) {
    std::abort();
  }
  auto options = base;
  for (size_t i = 0; i < full.size(); ++i) {
    if (bits >> (i % 61) & 1) {
      options.members.push_back(full[i].first);
    }
  }
  if (options.members.empty() && !full.empty()) {
    options.members.push_back(full[bits % full.size()].first);
  }
  size_t expected = options.members.size(), seen = 0;
  if (expected) {
    decode_archive(archive, options, [&](DecodedFile file) {
      auto found = byName.find(file.name);
      if (found == byName.end() || file.bytes.size() != found->second->size() ||
          !std::equal(file.bytes.begin(), file.bytes.end(), found->second->begin())) {
        std::abort();
      }
      ++seen;
    });
    if (seen != expected) {
      std::abort();
    }
  }
  // A name the archive does not hold is an error, never a silent success.
  options.members = {"fuzz-absent-member"};
  if (!byName.count(options.members[0])) {
    try {
      decode_archive(archive, options, [](DecodedFile) { std::abort(); });
      std::abort();
    } catch (const Error &) {
    }
  }
}
} // namespace

#ifndef FUZZ_TARGET
#error "define FUZZ_TARGET=1 (parse), 2 (roundtrip), 3 (dictionary), 4 (predictors), or 5 (archive)"
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
  if (const char *level = std::getenv("SLIPPACK_FUZZ_LEVEL")) {
    encode.level = std::atoi(level);
  }
  // Vary the shared-pool budget: default, no pooling, and two small budgets
  // that put some side streams in pools and others in per-file packets.
  uint64_t bits = choice_bits(data, size);
  const uint64_t budgets[] = {encode.pool_bytes, 0, 256, 65536};
  encode.pool_bytes = budgets[bits & 3];
  auto packed = encode_archive(inputs, encode);
  std::vector<std::pair<std::string, Bytes>> full(files.size());
  size_t decoded = 0;
  decode_archive(packed.bytes, {}, [&](DecodedFile file) {
    auto index = size_t(std::stoul(file.name.substr(5)));
    if (index >= files.size() || file.bytes.size() != files[index].size() ||
        !std::equal(file.bytes.begin(), file.bytes.end(), files[index].begin())) {
      std::abort();
    }
    full[index] = {file.name, Bytes(file.bytes.begin(), file.bytes.end())};
    ++decoded;
  });
  if (decoded != files.size()) {
    std::abort();
  }
  check_listing_and_selection(packed.bytes, {}, full, bits >> 2);
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
  std::vector<std::pair<std::string, Bytes>> full;
  bool decoded = false;
  try {
    decode_archive({data, size}, options, [&](DecodedFile file) {
      full.emplace_back(file.name, Bytes(file.bytes.begin(), file.bytes.end()));
    });
    decoded = true;
  } catch (const Error &) {
  } // Expected validation failures, never crashes/exits.
  uint64_t bits = choice_bits(data, size);
  if (decoded) {
    // Valid archives: listing and selection must agree with the full decode.
    check_listing_and_selection({data, size}, options, full, bits);
    return 0;
  }
  // Invalid archives: listing and selection may only fail with Error.
  try {
    auto listing = list_archive({data, size}, options.limits);
    for (size_t i = 0; i < listing.entries.size() && i < 64; ++i) {
      if (bits >> i & 1) {
        options.members.push_back(listing.entries[i].name);
      }
    }
    if (!options.members.empty()) {
      decode_archive({data, size}, options);
    }
  } catch (const Error &) {
  }
  return 0;
}
#endif
