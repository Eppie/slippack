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

For the faster profile-guided build, use `make release` and `make release-check`.
This installs `./slippack` and builds `build/release/libslippack.a`. The included
profile was measured with Apple Clang 17 (`clang-1700.0.13.5`) on arm64 macOS.
The release target checks the compiler, target, source hashes, and profile
checksum before building. Plain `make` needs no profile; use `make clean && make`
to switch an existing release build back to it.

After changing codec sources or compilers, regenerate the profile using a list
of local replay paths, then build again:

```sh
python3 tools/profile_build.py train --manifest /path/to/replays.txt --limit 64
make release-check
```

Training compresses and verifies up to 64 evenly spaced inputs at level 3,
using temporary archives. It leaves the input files and existing archives
untouched. The stored profile contains function execution counts and symbols;
its provenance contains source and input hashes, without replay contents.
See the [profiled release study](benchmarks/2026-11-11-profiled-release/README.md)
for the build settings, measurements, and limitations.

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
| 3 (default) | 2, with literal prediction; lazy parsing for opaque data |
| 4–5 | 4, with lazy parsing |
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
`benchmarks/2026-11-09-speed/`; those earlier benchmark sets were examined during
development, so those results are not independent held-out estimates.

## Encoding speed

The current build (candidate 8) **meets the additional 10% compression-speed
goal on independent validation** against `3d39721`, at level 3. The 512 reserved
files come from 16 source families excluded from tuning and compiler training.
All 27 repetitions from three launches are retained, using one replay per
archive with RAM inputs on an Apple M4 Max on AC power.

| Workers | Encode throughput gain | Encode time reduction | Decode throughput gain |
|---|---:|---:|---:|
| 1 | 12.67% | 11.25% | 0.72% |
| 8 | 12.89% | 11.42% | 0.82% |

Total output falls from 64,217,482 to 64,132,365 bytes (**0.133% smaller**).
The separate 1,408-file test excluding the original 521 and the full
1,929-file development test also pass. All correctness and sleep checks pass;
the installed CLI matches the frozen tested binary. See the
[independent result](benchmarks/2026-11-12-generalization/candidate8-awake1-holdout-replication-summary.json)
and [completion audit](benchmarks/2026-11-12-generalization/candidate8-completion-audit.json).
These are measured averages: eight-worker launch gains range from 7.01% to
19.96%, and 105 of the 512 files grow even though aggregate size decreases.

Earlier candidates did not meet the goal. One improved compression throughput
11.0–11.4% on 905 development files, but only 5.0% with one worker and 8.0% with
eight on 512 previously unused files from eight other source collections.
Holdout output was 0.188% smaller; average decoding throughput was 0.18% and
0.06% lower. These held-out results take precedence over the tuning result.
See the [generalization study](benchmarks/2026-11-12-generalization/README.md)
for all samples, limitations, source breakdowns, and passing correctness checks.
Development then expanded to 1,417 files across 17 source collections, with
another 512 files from eight different collections reserved for final validation.
Candidate 5 also corrects scratch-buffer reuse and avoids state/table reloads
in presence-mask encoding. A fixed three-launch replication on the expanded
development set includes all 27 repetitions, including the initial failed run.
Compression throughput improves 9.65% with one worker and 10.98% with eight
(elapsed time falls 8.80% and 9.89%). Output is 0.176% smaller and decoding is
about 1.1% faster. It passes full correctness checks, but the single-worker
result remains below target.
Candidate 6 adds smaller presence-encoder tables and has passed normal, scalar,
sanitizer, and optimized release correctness checks. Its fixed 27-repetition
development test improves compression throughput 10.64% / 11.00% with one/eight
workers (elapsed time falls 9.62% / 9.91%), with 0.176% smaller total output
and faster decoding. Independent validation on another 512 files falls short:
compression throughput improves 8.24% / 11.52% (elapsed time falls 7.62% / 10.33%),
total output is 0.204% smaller, and decoding is 0.75% / 0.78% faster. All 27
repetitions are retained in the [independent result](benchmarks/2026-11-12-generalization/candidate6-holdout-replication-summary.json).
The single-worker target remains unmet. These 512 files are now retired into
development; any subsequent measurements on them are not independent evidence.

Candidate 7 reuses dictionary index-buffer storage between trials and passes
normal, scalar, sanitizer, and optimized-release correctness checks. It fails
the fixed 27-repetition test on all 1,408 development files outside the original
521: compression throughput improves 9.68% / 10.08%, while elapsed time falls
8.83% / 9.16%. Output is 0.161% smaller and decoding is 0.93% / 0.99% faster.
The single-worker target remains unmet, so qualification stopped before the
full 1,929-file gate. This left 512 files from 16 further source families
unused and reserved for independent validation. All samples are retained in
the [outside-original report](benchmarks/2026-11-12-generalization/candidate7-outside521-replication-summary.json).

Candidate 8 groups four presence symbols while keeping their coder states
independent in the main loop. It passes full correctness checks and produces
identical packets to candidate 7, including pooled archives at four levels.
It passes the fixed 27-repetition test on all 1,408 development files outside
the original 521: compression throughput improves 12.70% / 12.87% with one/eight
workers, while elapsed time falls 11.27% / 11.40%. Total output is 0.161% smaller,
and decoding throughput improves 1.01% / 1.17%. All three sleep guards passed;
see the [outside-original report](benchmarks/2026-11-12-generalization/candidate8-awake1-outside521-replication-summary.json).
An earlier attempt was stopped during full-development qualification after a
937-second host sleep contaminated a baseline measurement. All original
reports are retained in the [incident record](benchmarks/2026-11-12-generalization/candidate8-sleep-incident.json).
The complete replacement sequence uses the same frozen build and a
[new protocol](benchmarks/2026-11-12-generalization/candidate8-awake1-replication-protocol.json)
with explicit sleep prevention and detection. The full 1,929-file gate also
passes: compression throughput improves 12.25% / 11.72%, elapsed time falls
10.91% / 10.49%, total output is 0.183% smaller, and decoding is 0.89% / 0.96%
faster. All 27 repetitions are retained in the
[full-development report](benchmarks/2026-11-12-generalization/candidate8-awake1-development-replication-summary.json).
Independent validation on the 512 reserved files from 16 additional source
families also passes with the same frozen build, as summarized above.

The preceding release at `3d39721` improves level 3 encoding throughput by
**11.3% with one worker and 12.1% with eight** over the preceding profile-guided
release at `888e0be`. Separate archives total **53,784,864 bytes**, down from
54,382,292 (**1.1% smaller**). Average decoding throughput is 0.8% and 1.2% higher,
respectively.

These are nine-run interleaved averages on 521 replays (1.31 GB), one replay per
archive, on an Apple M4 Max in high power mode on battery power. Eight workers
process independent files. Rates are original decimal MB/s:

| Workers | Encode before | Encode after | Encode gain | Decode before | Decode after |
|---|---:|---:|---:|---:|---:|
| 1 | 334.8 | 372.6 | 11.3% | 943.0 | 950.7 |
| 8 | 2286.3 | 2562.0 | 12.1% | 6886.7 | 6971.2 |

Compact probability headers make room for a faster level 3 match search.
Contiguous dictionary reads and reused decoder model lookups reduce memory work.
499 files shrink; 22 grow slightly, with a largest increase of 1.12% and a largest
absolute increase of 669 bytes. Existing archives were left untouched.
See the [method, validation, and raw results](benchmarks/2026-11-11-compact-models/README.md).
The corpus includes the compiler-training inputs and was examined during
development, so these are not independent held-out estimates.

On the matched 120-file set, that preceding release encodes at
372.6 MB/s versus 573.8 MB/s for slpz fast, and decodes at
948.7 versus 1118.2 MB/s (nine-run single-worker averages).
It produces 12.78 MB versus 42.11 MB, or 69.6% smaller output.
Slpz remains faster in both directions.

The earlier [profile-guided build study](benchmarks/2026-11-11-profiled-release/README.md),
[allocation and grouping study](benchmarks/2026-11-10-level3-memory/README.md),
[histogram and plane-screening study](benchmarks/2026-11-10-encode-speed/README.md),
and [hardware profile](benchmarks/2026-11-10-level3-profiling/README.md) document
preceding work.

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
