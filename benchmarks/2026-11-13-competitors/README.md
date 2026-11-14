# Matched single-replay codec comparison

This study compares Slippack commit `64bff11545ca90d34c6a9a833c459207077aa3c6`
with slpz `9f4d952f444037edbbc49af11e5e963ecd4d399f`, slippc
`8d79a665392e491edd62150687a956e45a7a0fe7`, and Peppi 2.1.2. The inputs and
matched subsets are those of the [2026-11-10 coverage study](../../EXPERIMENTS.md#2026-11-10-single-replay-comparison-with-slpz-slippc-and-peppi).
The upstream executables match the hashes recorded for that study's Peppi scope;
no upstream source or executable is changed. The Slippack runner links the
profile-guided library measured in the [dictionary study](../2026-11-13-gecko-dictionary/README.md).

## Method and coverage

Apple M4 Max, high-power mode (2), AC power throughout. Every setting uses one
warmup and three measured repetitions in a reproducibly shuffled order. Rates
are original decimal MB divided by median elapsed seconds. Each archive contains
one replay, with one worker per operation and inputs resident in RAM. Timings
include codec setup, transforms, allocations, and output construction. File I/O,
process startup, final buffer disposal, and verification are outside the timers.
The slpz runner reuses its Zstd context per worker. Peppi decoding includes
reconstructing the original `.slp` bytes from its Arrow representation.

The fixed sets are:

- **All:** 120 files / 313,007,679 original bytes, supported by Slippack and slpz.
- **slippc:** 98 files / 242,937,060 bytes, within its supported replay versions
  through 3.12.0. The 22 newer replays are not submitted to its fixed-layout codec.
- **Peppi:** 28 files / 74,297,116 bytes, with verified exact reconstruction in
  the coverage study. That study records 74 reconstruction panics and 18 byte
  mismatches among the other files. This refresh retains that fixed subset and
  the same upstream executable; it does not claim broader Peppi coverage.

These are development inputs, not an independent holdout. Compare tools only
within a matched set; the three sets differ in composition. The pinned versions
define the scope of the comparison. Short local timings remain sensitive to
desktop activity, allocation, and cache state.

## Results

Packed size includes complete file headers, names, and metadata. Smaller packed
percentages and higher MB/s are better.

| Set | Tool / setting | Packed bytes | Packed size | Encode MB/s | Decode MB/s |
|---|---|---:|---:|---:|---:|
| 120 | Slippack level 3 | 12,192,510 | 3.90% | 410.4 | 977.8 |
| 120 | slpz fast (Zstd 3) | 42,111,709 | 13.45% | 577.0 | 1,071.1 |
| 120 | slpz small (Zstd 12) | 38,187,718 | 12.20% | 118.3 | 1,220.1 |
| 98 | Slippack level 3 | 9,264,165 | 3.81% | 434.5 | 942.0 |
| 98 | slippc level 6 | 12,952,996 | 5.33% | 7.1 | 352.7 |
| 28 | Slippack level 3 | 3,111,433 | 4.19% | 375.1 | 907.2 |
| 28 | Peppi 2.1.2, uncompressed Arrow | 68,106,240 | 91.67% | 692.9 | 1,925.2 |
| 28 | Peppi 2.1.2, internal Zstd | 13,139,456 | 17.69% | 260.1 | 868.1 |
| 28 | Peppi 2.1.2 + whole-file Zstd 3 | 8,439,293 | 11.36% | 391.0 | 1,102.9 |
| 28 | Peppi 2.1.2 + whole-file Zstd 12 | 7,479,929 | 10.07% | 130.3 | 1,078.0 |

On the 120-file set, Slippack output is 71.0% smaller than slpz fast and 68.1%
smaller than slpz small. Slpz fast encodes about 1.4 times faster; Slippack encodes
about 3.5 times faster than slpz small. Slpz decodes faster at both settings.

Peppi's uncompressed Arrow row measures format conversion and serves as an
analysis-oriented reference. Internal Zstd compresses Arrow buffers. Whole-file
Zstd compresses the complete uncompressed `.slpp` file at the indicated level.

## Verification and reproduction

Every runner compares the reconstructed bytes with its source after every run.
All matched inputs pass. Per-file compressed sizes remain stable across repeats.
The driver checks input, executable, and Slippack source hashes before and after
measurement. It polls power source and mode once per second and observes no
changes. [results.json](results.json) contains all samples, hashes, versions,
per-file sizes, subset indexes, and completion status.

Build the Slippack benchmark runner from the release library:

```sh
make release-check
c++ -std=c++20 -O3 -pthread -Wl,-no_deduplicate \
  benchmarks/2026-11-10-single-file/cpp_runner.cpp build/release/libslippack.a \
  -o build/gecko-dictionary-v1/release-runner
python3 benchmarks/2026-11-13-competitors/benchmark.py \
  --ours build/gecko-dictionary-v1/release-runner \
  --upstream-work build/single-file-study \
  --output benchmarks/2026-11-13-competitors/results.json --repeats 3
```

The upstream work directory contains the pinned slippc/Rust executables and
`all.txt`, `slippc.txt`, and `peppi.txt` manifests from the coverage study. The
driver verifies their input hashes and upstream executable hashes before running.
The [original build script](../2026-11-10-single-file/build.sh) and locked Rust
dependencies document their toolchains; a different build requires a separate
provenance record. The linker flag above applies to the measured macOS build.
