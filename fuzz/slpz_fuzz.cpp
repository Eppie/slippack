// libFuzzer harnesses for slpz, sharing the real implementation by including
// the whole translation unit (SLPZ_FUZZ guards out its main()).
//
//   FUZZ_TARGET=1  parse: call parse_slp on the raw input (crash/OOB finder).
//   FUZZ_TARGET=2  roundtrip: pack the input as 1..N .slp files, extract, and
//                  assert byte-identity. Catches SILENT corruption that
//                  `slpz t` cannot (it only proves decode-doesn't-crash), plus
//                  the cross-file-pool path where the corrupt-file OOB lived.
//
// Build: fuzz/build.sh  (clang++ -fsanitize=fuzzer,address).
#define SLPZ_FUZZ 1
#include "../slpz.cpp"

#include <cstdlib>
#include <cstring>
#include <string>

#ifndef FUZZ_TARGET
#error "define FUZZ_TARGET=1 (parse), 2 (roundtrip), 3 (dictionary records), or 4 (predictor records)"
#endif

// ── Target 1: parse_slp ─────────────────────────────────────────────────────
#if FUZZ_TARGET == 1
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  ParsedSlp out;
  parse_slp(std::span<const uint8_t>(data, size), out);  // bool ignored; ASAN watches
  return 0;
}
#endif

// ── Target 2: round-trip oracle ─────────────────────────────────────────────
#if FUZZ_TARGET == 2
namespace {
// One absolute temp workdir per process. We do NOT chdir (that would break
// libFuzzer's own relative corpus-path handling); inputs use absolute paths,
// and the extract output path is derived exactly as cmd_extract does:
//   outDir / fs::path(storedName).relative_path(). All paths are keys into the
// in-memory FS (see SLPZ_FUZZ block in slpz.cpp) — nothing touches disk.
const fs::path g_in = "in", g_out = "out", g_arc = "arc.slpz";
bool init_dir() {
  // slpz's cmd_compress/cmd_extract print progress to stdout; silence it so it
  // doesn't pollute the fuzzer log. libFuzzer's own stats go to stderr.
  return freopen("/dev/null", "w", stdout) != nullptr;
}

// Split the fuzz buffer into 1..4 sub-files. Layout: u8 nFiles; then per file
// u16 len (little-endian) + bytes, each clamped to what remains. A buffer that
// doesn't fit the scheme still yields one file (the whole remainder), so a raw
// real-.slp seed round-trips as a single file.
std::vector<std::span<const uint8_t>> split(const uint8_t* data, size_t size) {
  std::vector<std::span<const uint8_t>> files;
  if (size == 0) { files.emplace_back(data, (size_t)0); return files; }
  // A genuine .slp seed passes through whole, so mutated real replays exercise
  // the full parse→transform→reconstruct round-trip as a single file (the
  // header-byte split scheme below would otherwise shred them into raw blobs).
  if (size >= 11 && memcmp(data, kSlpMagic, 11) == 0) {
    files.emplace_back(data, size);
    return files;
  }
  size_t pos = 0;
  int n = 1 + (data[pos++] & 3);  // 1..4
  for (int i = 0; i < n && pos < size; i++) {
    size_t len;
    if (i == n - 1 || pos + 2 > size) {
      len = size - pos;  // last file (or no room for a length) takes the rest
    } else {
      len = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
      pos += 2;
      len = std::min(len, size - pos);
    }
    files.emplace_back(data + pos, len);
    pos += len;
  }
  if (files.empty()) files.emplace_back(data, size);
  return files;
}
}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  if (!init_dir()) { perror("slpz_fuzz init"); _Exit(1); }
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto files = split(data, size);
  memfs_clear();  // drop the prior iteration's blobs; the FS never grows
  std::vector<fs::path> inputs;
  for (size_t i = 0; i < files.size(); i++) {
    fs::path p = g_in / ("f" + std::to_string(i) + ".slp");
    write_file(p, files[i]);
    inputs.push_back(p);
  }

  // threads=1 keeps it in-process/deterministic. Level is tunable via
  // SLPZ_FUZZ_LEVEL (default 3 = LZMA mix; 1-2 = brotli, ~2x faster/exec but
  // fuzzes the brotli plane path instead of LZMA). Compress never die()s on
  // file content (bad parses fall back to raw zstd); an extract die() therefore
  // signals a real archive-handling bug.
  static const int level = [] {
    const char* e = getenv("SLPZ_FUZZ_LEVEL");
    return e ? atoi(e) : 3;
  }();
  cmd_compress(g_arc, inputs, level, 1);
  cmd_extract(g_arc, g_out, 1, /*testOnly=*/false, /*hashMode=*/false);

  for (size_t i = 0; i < files.size(); i++) {
    auto got = read_file(g_out / inputs[i].relative_path());
    if (got.size() != files[i].size() ||
        (got.size() && memcmp(got.data(), files[i].data(), got.size()) != 0)) {
      fprintf(stderr,
              "ROUND-TRIP MISMATCH file %zu: in=%zu bytes out=%zu bytes\n", i,
              files[i].size(), got.size());
      abort();  // silent corruption — the bug class slpz t misses
    }
  }
  return 0;
}
#endif

// Target 3: v12 field dictionary transformations and record framing. Small
// bounded inputs keep mutations focused on dictionary values and indexes.
#if FUZZ_TARGET == 3
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 8) return 0;
  g_tuned = g_custom = true;
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
  if (restored[0].vals != f.vals) abort();
  Buf records;
  for (unsigned i = 0; i < count; ++i) {
    auto value = f.vals[data[4 + i % (size - 4)] % nvals];
    records.put_bytes(&value, width);
  }
  Zctx z;
  Buf packed;
  pack_records(z, records, count, width, 3, packed, nullptr);
  Reader r{packed.data(), packed.data() + packed.size()};
  std::vector<uint8_t> output;
  unpack_records(z, r, count, width, output);
  if (r.fail || r.p != r.end || output != records) abort();
  Reader scanner{packed.data(), packed.data() + packed.size()};
  auto span = skip_packed(scanner, width);
  if (scanner.fail || span.size() != packed.size()) abort();
  return 0;
}
#endif

// Target 4: numeric predictors composed with real custom record packets.
// Both the older XOR predictor and v13's signed residuals are exercised.
#if FUZZ_TARGET == 4
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t length) {
  if (length < 4) return 0;
  g_tuned = g_custom = true;
  g_v13_predictors = data[2] & 1;
  const uint32_t widths[] = {28, 29, 51, 59, 71, 72, 76, 84};
  uint32_t width = widths[data[1] & 7], count = 1 + data[0];
  Buf original;
  original.resize(size_t(width) * count);
  for (size_t i = 0; i < original.size(); ++i) original[i] = data[3 + i % (length - 3)];
  auto records = original;
  motion_predict(records.data(), count, width, false);
  Zctx z;
  Buf packed;
  pack_records(z, records, count, width, 3, packed, nullptr, true, 0x38);
  Reader r{packed.data(), packed.data() + packed.size()};
  std::vector<uint8_t> restored;
  unpack_records(z, r, count, width, restored);
  if (r.fail || r.p != r.end || restored != records) abort();
  motion_predict(restored.data(), count, width, true);
  if (restored != original) abort();
  input_predict(restored.data(), count, width, false);
  input_predict(restored.data(), count, width, true);
  if (restored != original) abort();
  return 0;
}
#endif
