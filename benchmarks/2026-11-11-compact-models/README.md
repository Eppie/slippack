# Faster level 3 compression with compact probability models

The baseline is the preceding profile-guided release,
`888e0bee0a981141f8e3525ee63f933a07faa231`. Both builds use Apple Clang 17,
`-O3 -fprofile-use`, and the macOS linker flag `-Wl,-no_deduplicate`.
The new profile was regenerated after the codec changes using the same 64
spaced training inputs. This comparison measures an additional improvement
over the previous compiler-profiling result.

## Changes

- Probability headers use a sorted symbol list or bitmap, an implicit first
  frequency, and Rice coding when it saves bytes. Context selection estimates
  the new header costs. The writer uses entropy channel modes 5 and 6; the
  semantic ANS helper also supports mode 7.
- Level 3 searches two match candidates with literal prediction. Record planes
  use greedy parsing; opaque frames retain lazy lookahead, which prevents
  large increases in attached data. Other levels retain their search policies.
  Search depth, lookahead, and prediction are explicit internal options.
- Dictionary gathering reads contiguous transposed columns, detects repeated
  values in SIMD blocks, and uses a smaller index table. The presence encoder
  emits bytes backward into a bounded buffer already in final output order.
- Decoding uses SIMD presence lookup, caches repeated presence models, bypasses
  history for a single model, and fetches next-model IDs from a compact side
  array. Filling complete CDF intervals keeps that array cheap to construct.

The archive version remains 1. The format details are in
[CUSTOM_FORMAT.md](../../CUSTOM_FORMAT.md).

## Method

Apple M4 Max, macOS 15.5, Apple Clang 17 (`clang-1700.0.13.5`). The complete
521-file corpus contains 1,309,548,357 original bytes. Each replay receives its
own archive. One worker processes files sequentially; eight workers process
independent files, each using one archive worker. Inputs are loaded into RAM.
Each setting gets one warmup and nine measured repetitions, randomly interleaved
between the two builds. I/O, byte comparisons, and hashing are outside timers.
Both libraries link the exact same compiled benchmark-driver object.

Every repetition verifies reconstructed bytes. Compressed sizes and SHA-1
fingerprints must remain stable within each variant across repetitions and
worker counts. The driver records all timing samples and source, binary, and
input hashes, then rechecks those hashes after the run. This comparison uses
`--allow-byte-changes`; the driver's default still requires identical output
between builds.

Power mode and AC/battery source are polled once per second. The completed
`all-521.json` run stayed in high power mode on battery power, with no observed
changes. Subsecond transitions cannot be excluded. The machine remained in
normal desktop use. The corpus includes the 64 compiler-training inputs and
was examined during development; it is not an independent held-out set.

## Results

Rates use original decimal MB divided by arithmetic mean elapsed seconds:

| Workers | Encode before | Encode after | Encode gain | Decode before | Decode after |
|---|---:|---:|---:|---:|---:|
| 1 | 334.8 | 372.6 | 11.3% | 943.0 | 950.7 |
| 8 | 2286.3 | 2562.0 | 12.1% | 6886.7 | 6971.2 |

The observed averages meet the target of at least 10% faster encoding, smaller
total output, and no average decoding slowdown at both worker counts. Decoding
throughput is 0.8% higher with one worker and 1.2% higher with eight; these are
small differences. Every timing sample, including outliers, is retained.

The separate archives shrink from **54,382,292 to 53,784,864 bytes** (1.10%).
499 files shrink and 22 grow. The maximum relative increase is **1.1131%**;
the maximum absolute increase, in a different file, is **669 bytes**.
`size-audit.json` records these checks. No existing archives were replaced.

## Validation

The ordinary build, profile-guided release, scalar build, and AddressSanitizer
plus UndefinedBehaviorSanitizer checks pass. Tests cover 4,958 custom-codec
checks, 16,849,134 probability-model/CDF checks, 111,137 record-codec checks,
2,462,719 predictor checks, 92 library-API checks, the frozen v1 fixture, and
archive/extraction behavior at four compression levels.

Fixed model fixtures independently check Rice bit order, single-model channels,
and alternating context leaves in modes 4 and 5. Truncation, invalid alphabets,
frequency sums, and padding are rejected. The SIMD symbol lookup is checked
against a linear CDF oracle for every 16-bit slot across 256 models, including
zero-frequency symbols. The existing modes 1, 2, and 4 have fixed decoding
fixtures. Scalar and SIMD paths restore identical bytes.

Actual CLI checks also compressed all 521 inputs into a single archive at
levels 1, 3, 7, and 9 and verified every member's SHA-1 against its source.
`pooled-validation.json` records these temporary archives and binary hashes:

| Level | Baseline bytes | Current bytes |
|---|---:|---:|
| 1 | 51,628,453 | 50,775,621 |
| 3 | 50,672,315 | 50,097,975 |
| 7 | 50,583,503 | 49,712,988 |
| 9 | 50,477,834 | 49,614,579 |

All temporary validation archives were removed after checking.

## Matched upstream comparison

`selected-120.json` compares the original 120-file set (313,007,679 bytes)
with upstream slpz 1.3.0, commit `9f4d952f444037edbbc49af11e5e963ecd4d399f`,
in fast mode (Zstd level 3). Its runner binary matches the hash in the preceding
published study. Some larger-corpus inputs are rejected by upstream slpz, so
this comparison uses only the matched set. There is one warmup and nine
randomly interleaved repetitions per setting. Power remained unchanged in
high power mode on battery power. Rates use arithmetic mean elapsed seconds.

| Tool | Workers | Packed bytes | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|---:|
| Slippack level 3 | 1 | 12,784,457 | 372.6 | 948.7 |
| slpz fast | 1 | 42,111,709 | 573.8 | 1118.2 |
| Slippack level 3 | 8 | 12,784,457 | 2441.8 | 6808.1 |
| slpz fast | 8 | 42,111,709 | 4026.0 | 7049.9 |

Slippack output is 69.6% smaller; slpz remains faster in both directions.
This improves the measured tradeoff and does not establish a general Pareto
frontier. The earlier four-tool comparison remains linked from the main README.

## Reproduction

On the measured macOS toolchain, with a local manifest of replay paths:

```sh
git worktree add --detach build/compact-baseline 888e0be
make -C build/compact-baseline release
make release-check
c++ -std=c++20 -O3 -pthread -c benchmarks/2026-11-10-single-file/cpp_runner.cpp \
  -o build/compact-common.o
c++ -std=c++20 -O3 -pthread -Wl,-no_deduplicate build/compact-common.o \
  build/compact-baseline/build/release/libslippack.a -o build/compact-baseline-runner
c++ -std=c++20 -O3 -pthread -Wl,-no_deduplicate build/compact-common.o \
  build/release/libslippack.a -o build/compact-candidate-runner
python3 benchmarks/2026-11-10-encode-speed/benchmark.py --allow-byte-changes \
  --baseline build/compact-baseline-runner --candidate build/compact-candidate-runner \
  --baseline-commit 888e0bee0a981141f8e3525ee63f933a07faa231 \
  --manifest /path/to/all-521.txt --output /tmp/all-521.json \
  --levels 3 --threads 1,8 --repeats 9 --power-mode 2
```

The committed profile is tied to the recorded compiler and sources. Other
compilers need newly trained profiles, or ordinary builds for both variants.
See `build-info.json` and `baseline-provenance.json` for the exact artifact
hashes used here. Manifest order is the report's input order; no replay payloads
or local source paths are published.
