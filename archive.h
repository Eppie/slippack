#pragma once
#include "binary_io.h"
#include <array>
#include <functional>
#include <string_view>

namespace slippack {
// All limits are per operation. Wave size is an admission target, not an RSS
// limit: the input, pools, codec scratch, and sink-owned files also occupy RAM.
struct DecodeLimits {
  uint64_t archive_bytes = 8ull << 30;
  uint64_t file_bytes = 16ull << 30;
  uint64_t total_bytes = 1ull << 40;
  uint64_t frame_bytes = UINT32_MAX;
  uint64_t pool_bytes = 2ull << 30;
  uint64_t metadata_bytes = 512ull << 20;
  uint32_t files = 1000000, pools = 65536, name_bytes = 4096;
  uint32_t records = 32000000;
};
using PhaseObserver = std::function<void(std::string_view, double)>;
struct EncodeOptions {
  int level = 3, threads = 1;
  bool collect_stats = false;
  size_t admission_files = 64;
  PhaseObserver on_phase;
};
struct DecodeOptions {
  int threads = 1;
  bool compute_hashes = false;
  uint64_t wave_bytes = 2ull << 30;
  DecodeLimits limits;
  PhaseObserver on_phase;
};
struct Statistics {
  std::array<uint64_t, 256> by_command{};
  uint64_t order = 0, suffix = 0, tail = 0, selector = 0, misc = 0;
  uint64_t seeds = 0, seed_hits = 0, seed_misses = 0;
};
// Names are relative archive paths, independent of filesystem source paths.
// Each load callback runs once, possibly concurrently with other inputs. It
// returns owned bytes; everything it captures must outlive encode_archive.
struct ArchiveInput {
  std::string name;
  std::function<Bytes()> load;
};
struct EncodeResult {
  Bytes bytes;
  uint64_t original_bytes;
  Statistics statistics;
};
struct DecodedFile {
  std::string name;
  OutputBytes bytes;
};
struct EntryInfo {
  std::string name;
  uint64_t size;
  std::string sha1;
};
struct DecodeResult {
  std::vector<EntryInfo> entries;
  uint64_t original_bytes = 0;
};
// The sink may run concurrently and receives ownership of each complete file.
// Exceptions from loaders, sinks, or workers propagate after workers join.
// A throwing sink may have already accepted other files. No callbacks survive
// the call, and no global mutable operation state is used.
using FileSink = std::function<void(DecodedFile)>;
EncodeResult encode_archive(std::span<const ArchiveInput> inputs,
                            const EncodeOptions &options = {});
DecodeResult decode_archive(View archive, const DecodeOptions &options = {}, FileSink sink = {});
// Canonical, portable relative paths; throws on traversal or ambiguous names.
std::string archive_name(std::string_view name);
} // namespace slippack
