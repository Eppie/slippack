# Linux x86 encoding study

Level 3, one worker, AMD Ryzen 9 7950X under WSL2, Clang 20.1.8. The full
corpus contains 1,518 pcloud bot-match replays (5,987,689,510 original bytes)
from 57 matchup directories. Both one replay per archive and all replays in
one archive use the same inputs and stored basenames.

## Method and baseline

The baseline is the frozen optimized Clang build from immediately before this
optimization round. It already includes the earlier x86 SIMD and scratch-buffer
work. It is not the original GCC build. Source and library hashes identify this
otherwise uncommitted baseline in `provenance.json`.

Each executable links one codec library and the same separately compiled GCC
benchmark driver object. Inputs and reference packets reside in RAM. There is
one warmup followed by three randomly interleaved measured repetitions per
layout; tables report medians and decimal MB/s of original data. File loading,
exact byte comparison, output fingerprints, and final output-buffer disposal
are outside timers. Both baseline and candidate load reference packets through
the same code path. The encoding timer includes archive construction and the
encoder's internal allocation and copying.

The source-only candidate uses the baseline's `-O3 -march=native` flags. The
additional optimized build uses Clang IR PGO and ThinLTO, linked with LLD 20.
Its profile was trained on 128 pcloud replays (482,035,301 bytes), disjoint from
the 112-file screening sample. Those training files are included in the full
1,518-file corpus. This is development data, not an independent holdout for the
whole compressor. Results are specific to this workload and machine.

## Encoding results

| Layout | Build | Packed MB | Ratio | Encode MB/s | Gain | Decode after encode MB/s |
|---|---|---:|---:|---:|---:|---:|
| Separate | Before | 202.232 | 29.61:1 | 306.9 | +0.00% | 724.0 |
| Separate | Source only | 197.573 | 30.31:1 | 415.7 | +35.45% | 725.6 |
| Separate | Source + PGO + ThinLTO | 197.573 | 30.31:1 | 432.0 | +40.76% | 737.8 |
| One archive | Before | 183.763 | 32.58:1 | 420.1 | +0.00% | 581.2 |
| One archive | Source only | 183.762 | 32.58:1 | 424.3 | +1.01% | 587.2 |
| One archive | Source + PGO + ThinLTO | 183.762 | 32.58:1 | 444.2 | +5.74% | 594.1 |

Separate archives shrink by 2.30%. The combined archive shrinks by 712 bytes;
it is not byte-identical. All replay bytes restore exactly.

`full-results.json` contains every timing sample, size, binary hash, archive
comparison result, and replay verification result. `summary.csv` provides the
same aggregate columns for each build and archive layout.

A secondary check on the older 66-replay set (79.68 MB) also restores all bytes
exactly. Separate archives change by -8.720% in size; the combined
archive changes by -0.009%. These short timing runs are recorded in
`outside-results.json`; the larger pcloud run is the primary speed measurement.

## Decoding and compatibility

| Archive layout | Before decode MB/s | Source-only MB/s | PGO + ThinLTO MB/s |
|---|---:|---:|---:|
| Separate archives | 650.1 | 661.5 | 656.5 |
| One archive | 789.3 | 783.5 | 773.3 |

This separate 112-file measurement performs no encoding or encoder
initialization in its timing processes. Running decoding immediately after
encoding changes allocator state and produced inconsistent decoder comparisons;
the full run also records those timings, but they are not evidence of a decoder
algorithm speedup. Fresh-process results show roughly unchanged decoding speed.

The frozen older decoder also reconstructs every new archive in this sample
exactly. The encoder uses existing v1 record extensions and custom raw frames;
it introduces no new archive format or dictionary ID.

## Encoder rules and build

- Reject a record dictionary trial as soon as it exceeds the maximum distinct
  value count that the existing acceptance rule permits. Stop trials when the
  descriptor count reaches its format limit.
- Try built-in Gecko dictionary coding first for code commands. At level 3,
  accept it immediately when the complete record packet is at most one quarter
  of the original bytes, avoiding the ordinary and contiguous compression
  candidates. Otherwise retain the existing candidate comparison. This is a
  candidate-selection heuristic; it can change bytes and does not guarantee the
  smallest of all three candidates for every possible input.
- Store a singleton non-code record as an existing contiguous raw packet. This
  avoids generating a separate constant-plane packet for every byte, including
  the 760-byte game-start record.
- Use a separate Linux x86 PGO profile and ThinLTO for the fastest build. The
  default flags are the same as on other platforms.

These three selection rules apply on every architecture, so arm64 and x86-64
builds write the same archive bytes. The SIMD kernels and the backward-filled
context-coder buffer are compiled under `SLIPPACK_SSE2` and its AVX-512
refinements; they change speed, not output. The
[arm64 study](../2026-11-14-encoder-parity/README.md) measures the rules on
Apple hardware and compares archives across architectures.

The optimized build passes `make check`, including codec, model, predictor,
archive API, frozen v1 fixture, all four compression levels, and extraction
integration tests. Focused record and dictionary checks also passed with
ASan/UBSan and a generic x86-64 build during development. See `validation.json`.

## Reproduction

Use the Linux build command in the root README. The checked-in x86 profile has
compiler, target, and source provenance in
[`profiles/linux-x86-level3.json`](../../profiles/linux-x86-level3.json).
`tools/profile_build.py check --cxx clang++ --profile
profiles/linux-x86-level3.proftext` validates that profile before building.

Compile the common driver once, then link each frozen library separately:

```sh
g++ -std=c++20 -O3 -I. -c \
  benchmarks/2026-11-14-linux-x86/cpp_runner.cpp -o build/runner.o
g++ build/runner.o /path/to/baseline/libslippack.a -lcrypto -pthread \
  -o build/baseline-runner
g++ build/runner.o /path/to/source-only/libslippack.a -lcrypto -pthread \
  -o build/source-runner
clang++ -flto=thin --ld-path=/usr/bin/ld.lld-20 build/runner.o \
  build/linux-x86/libslippack.a -lcrypto -pthread -o build/optimized-runner
python3 benchmarks/2026-11-14-linux-x86/benchmark.py \
  --manifest /path/to/replays.txt --baseline build/baseline-runner \
  --candidate build/source-runner --variant pgo_thin=build/optimized-runner \
  --output build/measurement --reps 3
```

Without `--references`, the controller prepares baseline reference archives in
a separate process before any timed runs. With `--references`, use a directory
created by the same driver in `write` mode. Replay restoration is always checked
exactly. Different compressed bytes are reported separately and are allowed in
this study. `decode_runner.cpp` and `decode_benchmark.py` repeat decoding without
running the encoder and test the old reader against the new packets.
