#pragma once
#include "archive.h"
#include <filesystem>
#include <memory>

namespace slippack {
enum class Overwrite { Refuse, Replace };
Bytes read_file(const std::filesystem::path &path, uint64_t limit = UINT32_MAX);
// Read-only view of a file's bytes, paged in on access. Reading a few entries
// of a large archive costs time proportional to the bytes touched. The file
// must not be truncated while mapped.
class MappedFile {
  const uint8_t *data_ = nullptr;
  size_t size_ = 0;

public:
  explicit MappedFile(const std::filesystem::path &path, uint64_t limit = UINT32_MAX);
  ~MappedFile();
  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;
  View bytes() const { return {data_, size_}; }
};
// Writes a complete sibling temporary file, then atomically installs it.
void write_file(const std::filesystem::path &path, View data, Overwrite policy);
// Maps a CLI source path to a safe stored name. Outside/absolute sources use
// their basename; relative paths inside the working directory keep subfolders.
std::string source_name(const std::filesystem::path &source);
class ExtractionWriter {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  ExtractionWriter(const std::filesystem::path &root, Overwrite policy);
  ~ExtractionWriter();
  ExtractionWriter(const ExtractionWriter &) = delete;
  ExtractionWriter &operator=(const ExtractionWriter &) = delete;
  void submit(DecodedFile file); // concurrent; bounded queue with backpressure
  void finish();                 // joins writers and propagates errors; destructor cancels
};
} // namespace slippack
