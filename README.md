# Slippack — Slippi replay compressor

Losslessly compresses one or many `.slp` replays into an archive. It uses Slippi event
structure, cross-frame and controller prediction, field dictionaries, shared
presence masks, and a custom rANS/LZ payload codec. Extraction restores the
original file bytes, including metadata, malformed input, and unknown events.

The executable reads and writes **v1 custom archives**. The codec is
self-contained. No zstd, LZMA, or Brotli libraries are required.

## Build and use

A C++20 compiler is required. macOS uses the system CommonCrypto implementation
for SHA-1; Linux requires OpenSSL development headers and `libcrypto`.

```sh
make
./slippack c replay.slp                 # creates replay.slpc beside the input
./slippack c first.slp second.slp       # creates archive.slpc in this directory
./slippack c -o out.slpc -l 3 -T 16 file1.slp file2.slp
./slippack c -o out.slpc --overwrite @listfile
./slippack x out.slpc -d outdir       # extract, recreating stored paths
./slippack t out.slpc                 # decode in memory without writing files
./slippack hash out.slpc              # SHA-1 per file in shasum format
./slippack selftest
make check
```

Without `-o`, a single input uses its path with the extension replaced by `.slpc`;
multiple inputs create `archive.slpc` in the current directory. List-file entries
follow the same rule after expansion. Use `-o` to choose another filename.

List files contain one input path per line. `-T` sets the thread count
(default: available hardware threads). `-v` prints compressed bytes by section.
Use `--` before filenames that start with a dash.

Source paths and archive names are separate. Relative paths retain their
subdirectories; absolute paths and paths outside the working directory use
basenames. Harmless `./` prefixes already stored in archives are preserved.
Duplicate names and file/directory conflicts are rejected.

Both creation and extraction refuse to overwrite existing files by default.
Pass `--overwrite` to replace regular files. Outputs are written to sibling
temporary files and installed atomically; extraction never follows symlinks
inside the selected destination. Archive members containing absolute paths,
parent traversal, backslashes, drive prefixes, NULs, or line breaks are rejected.
The destination root chosen by the caller is resolved once, including platform
aliases such as macOS `/var`. Each file is atomic; a failed multi-file extraction
can leave earlier completed files in place. Atomic installation does not promise
power-loss durability.

Decoding checks counts, sizes, and record shapes before allocating. Default
limits are 8 GiB of archive input, 16 GiB per decoded file, 1 TiB total decoded
output, just under 4 GiB per frame/record packet (the v1 frame maximum), 2 GiB of
shared pool output, and 512 MiB
for descriptors plus admitted side-stream metadata. The CLI exposes
`--max-archive-mib`, `--max-file-mib`, `--max-output-mib`, `--max-frame-mib`,
`--max-pool-mib`, and `--max-metadata-mib`. `--wave-mib` changes the default
2 GiB decoded-output admission target. These are validation and scheduling
limits, not a process RSS cap: input, pool buffers, record pointers, codec
scratch, and queued writes also occupy memory. Additional count limits are
explicit in `DecodeLimits` in [archive.h](archive.h).

`-l` controls the custom LZ search effort:

| Levels | Search candidates |
|---|---:|
| 1–2 | 2 |
| 3–5 (default 3) | 4, with lazy parsing |
| 6–8 | 8 |
| 9–12 | 24 |

## How it works

1. Parse the UBJSON wrapper and self-described Slippi event payload sizes.
   Preserve the header, metadata, unparsed tail, and event order.
2. Split events by command and player/follower or item group. Pool repeated
   side-event records across files.
3. Predict pre-frame values from the preceding post-frame state, fold RNG
   seeds into bounded LCG step counts, and predict positions and controller
   inputs. Older Slippi replay record sizes remain supported.
4. Replace low-cardinality fields with sorted dictionary indexes. Constant
   dictionaries need no index plane.
5. Transpose records into byte planes and choose plain, subtract-delta, or
   XOR-delta transforms. Encode semantic planes using constant/sparse/run
   packets, shared presence masks, and the custom entropy/LZ codec.
6. Store opaque input in custom frames. Large attachments are chunked into
   1 MiB tasks and may use a reversible PowerPC branch transform.

Encoding pipelines file planning into a shared task queue; decoding assembles
files as their dependencies complete. NEON accelerates transforms on Apple
Silicon, with scalar paths on other architectures. Float predictors require
IEEE binary32 round-to-nearest; fast-math builds are unsupported.

See [CUSTOM_FORMAT.md](CUSTOM_FORMAT.md) for the format and predictor details.

## Code structure and library API

The build produces `build/libslippack.a` alongside the CLI. [archive.h](archive.h)
is the application interface: `encode_archive` accepts named input loaders and
returns owned archive bytes and statistics; `decode_archive` borrows archive
bytes, returns ordered entry information, and optionally sends owned decoded
files to a sink. Loaders and sinks may run concurrently. They must keep captures
alive for the call and synchronize shared state. No callbacks outlive a call.
Failures throw `slippack::Error` (or propagate callback/allocation exceptions) after
workers join; the CLI alone translates failures into diagnostics and exit codes.

| Module | Responsibility |
|---|---|
| `slippack.cpp` | CLI parsing, reporting, connecting loaders and sinks |
| `archive.h/.cpp` | v1 container, validation, scheduling, per-operation options/stats |
| `file_io.h/.cpp` | bounded writer queue, safe directory traversal, atomic file output |
| `slp.h/.cpp` | Slippi parsing, RNG folding, replay predictors, PPC transform |
| `record_codec.h/.cpp` | transpose/delta kernels, dictionary coding, shared packet parser |
| `frames.h/.cpp` | frame envelopes around the payload codec |
| `custom_codec.h`, `mask_codec.h`, `static_context.h` | self-contained payload coding headers |
| `binary_io.h`, `codec_types.h` | checked little-endian reading, varints, shared codec types |
| `parallel.h`, `platform.h/.cpp` | joining workers/error propagation, SHA-1 and allocator helpers |

Internal task payloads explicitly identify their work. Borrowed spans remain
valid until their owning plan's final task completes. Statistics and budgets
belong to each operation; concurrent calls and repeated calls after an error
are tested. Predictor operations establish round-to-nearest and restore the
caller's floating-point mode. The optimized kernels retain scalar alternatives.

## Validation and benchmarks

Use `make format` to format all C++ code and insert braces around control-flow
bodies, or `make check-format` to check without editing. These targets require
clang-format 15 or newer; set `CLANG_FORMAT=/path/to/clang-format` if needed.

`make check` covers custom packets, dictionary framing, scalar predictor
oracles, exception propagation, concurrent/repeated library calls, path and
symlink confinement, atomic overwrites, limits, exact extraction, unsupported-format
rejection, and a frozen v1 fixture with independently generated expected contents.
Fuzzing instructions are in
[fuzz/README.md](fuzz/README.md).

The `benchmarks/` directory and [EXPERIMENTS.md](EXPERIMENTS.md) record historical
experiments. Their backend selections and version comparisons describe the
commits used for those studies. Earlier speed studies are under
`benchmarks/2026-11-09-speed/`; all benchmark sets were examined during
development, so those results are not independent held-out estimates.

## Encoding speed

The latest changes improve default-level encoding throughput by **2–8%** over
`cf99bea9`, with identical compressed output. Nine-run interleaved medians on
an Apple M4 Max in high power mode on AC power, one replay per archive:

| Inputs | Workers | Before MB/s | After MB/s | Encode gain |
|---|---:|---:|---:|---:|
| 120 replays | 1 | 298.7 | 306.4 | 2.6% |
| 120 replays | 8 | 1869.8 | 2019.7 | 8.0% |
| 401 replays | 1 | 284.7 | 290.8 | 2.2% |
| 401 replays | 8 | 1985.7 | 2048.1 | 3.1% |

The encoder partitions records into entity groups once and avoids unnecessary
copies, literal labels, and dictionary scans. The decoder code and v1 format
are unchanged; decode medians stayed within 1.2% of baseline. See the
[method, validation, and raw results](benchmarks/2026-11-10-level3-memory/README.md)
and [hardware profiling results](benchmarks/2026-11-10-level3-profiling/README.md).
The earlier histogram and plane-screening improvements are documented in the
[previous speed study](benchmarks/2026-11-10-encode-speed/README.md).

In the fresh 120-file single-worker comparison, Slippack level 3 encodes at
306.4 MB/s versus 550.3 MB/s for slpz fast. Slippack produces 12.9 MB versus
42.1 MB, or 69.3% smaller output. Slpz remains faster at encoding and decoding.

## Single-replay comparison

This four-tool comparison used Slippack at `9bffdb5`, before the encoding
optimizations above. On an Apple M4 Max, 120 individually compressed replays
(313 MB), one worker, five-run medians, with inputs in RAM and exact round-trip
verification:

| Matched set | Tool / setting | Packed size / original | Encode MB/s | Decode MB/s |
|---|---|---:|---:|---:|
| All 120 | Slippack level 3 (default) | 4.13% | 215.6 | 858.9 |
| All 120 | [slpz](https://github.com/AlexanderHarrison/slpz) --fast (default) | 13.45% | 531.2 | 1027.4 |
| All 120 | slpz --small | 12.20% | 109.2 | 1164.8 |
| slippc: 98 | Slippack level 3 | 4.03% | 220.9 | 894.5 |
| slippc: 98 | [slippc](https://github.com/pcrain/slippc) level 6 | 5.33% | 6.6 | 335.3 |
| Peppi: 28 | Slippack level 3 | 4.41% | 216.1 | 803.9 |
| Peppi: 28 | [Peppi](https://docs.rs/peppi/2.1.2/peppi/#peppi-format), uncompressed Arrow | 91.67% | 659.9 | 1919.5 |
| Peppi: 28 | Peppi, internal Zstd | 17.69% | 257.3 | 501.8 |
| Peppi: 28 | Peppi + whole-file Zstd 3 | 11.36% | 382.6 | 1072.2 |
| Peppi: 28 | Peppi + whole-file Zstd 12 | 10.07% | 126.5 | 1108.8 |

Compare rows within the same matched set. The slippc set contains the 98 files
within its supported replay versions. Peppi 2.1.2 restored 28 files byte-for-byte; its rows use only those
files. Peppi's uncompressed row measures format conversion, and its Arrow
representation also serves analysis use cases.

Slippack's default output was 69% smaller than slpz's fast preset, while slpz
encoded about 2.5 times faster and decoded about 1.2 times faster. On slippc's
matched set, Slippack produced 24% smaller files, encoded about 33 times faster,
and decoded about 2.7 times faster. On Peppi's matched set, Slippack produced
61% smaller files than whole-file Peppi + Zstd 3, with about 1.8 times slower
encoding and 1.3 times slower decoding.

See [the full method and matched comparisons](EXPERIMENTS.md#2026-11-10-single-replay-comparison-with-slpz-slippc-and-peppi)
for every tested level, single/eight-worker results, Peppi coverage details,
and reproduction instructions. These are codec timings; disk I/O and process
startup are excluded. [Raw results](benchmarks/2026-11-10-single-file/results.json)
include source hashes and all timing samples.
