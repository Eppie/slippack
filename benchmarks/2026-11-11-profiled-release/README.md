# Profile-guided level 3 release

The baseline is `f11d8d5567ffb7207141eb4a9ac8cdc8f1e7ba4f`, built with
Apple Clang 17 and `-O3`. The candidate uses the same C++ sources and archive
format, compiled with a measured execution profile. Build it with
`make release`; run its full checks with `make release-check`.

## Build

[Clang profile-guided optimization](https://clang.llvm.org/docs/UsersManual.html#profile-guided-optimization)
uses execution counts from an instrumented program to guide compiler decisions.
The training tool builds the actual CLI with `-fprofile-generate` and atomic
counters, then runs level 3 compression and hash verification on 64 inputs
spaced evenly through the 521-file manifest. Each input gets its own temporary
archive, with one worker. The text profile and provenance are in
[`profiles/`](../../profiles/).

The release compiles with `-O3 -fprofile-use`, without link-time optimization.
On macOS it also disables the linker's deduplication pass. In a direct check,
the compiler produced identical codec objects from the exploratory and final
training profiles; enabling linker deduplication changed the executable and
reduced the encoding gain. `all-521-linker-dedup.json` preserves that earlier
nine-run result: 9.0% more encoding throughput with one worker and 5.7% with
eight. Disabling deduplication reproduced the faster exploratory runner byte
for byte. This is a measured build choice for this toolchain, not a general
recommendation to disable linker optimization.

`make release` validates the compiler version and target, codec source hashes,
and profile checksum. A mismatch requires regenerating the profile or using
the ordinary build. `make` remains available without profiles. The profile
contains function symbols and counts, with no replay payloads. Input hashes
and sizes are retained for provenance; training paths are omitted.

## Method

Apple M4 Max, macOS 15.5, Apple Clang 17 (`clang-1700.0.13.5`). All 521 replays
total 1,309,548,357 original bytes. Each replay gets its own archive. One worker
processes files sequentially; eight workers process independent files.
Inputs are loaded into RAM before timing. Each setting receives one warmup
and nine measured repetitions, randomly interleaved between baseline and
candidate. I/O, verification, and compressed fingerprints are outside timers.
Both executables link the exact same compiled benchmark driver; only the
codec library and the candidate's linker setting differ.

The benchmark records every timing sample, input hash, executable hash, and
codec source hash. Every run checks reconstructed bytes and compares compressed
sizes and SHA-1 fingerprints with the baseline. Power mode and AC/battery source
are polled once per second; an observed change rejects the run. Transitions
shorter than that polling interval cannot be excluded. The machine remained in
normal desktop use. These inputs were examined during development and include
the 64 compiler-training inputs; this is not an independent held-out estimate.

## Results

`all-521.json` records a completed run in high power mode on battery power,
with no observed power-source or mode changes. Rates below are original decimal
MB divided by median elapsed seconds.

| Workers | Encode before | Encode after | Encode gain | Decode before | Decode after |
|---|---:|---:|---:|---:|---:|
| 1 | 269.9 | 306.3 | 13.5% | 900.2 | 913.8 |
| 8 | 2150.3 | 2374.5 | 10.4% | 6823.6 | 6829.3 |

Using mean elapsed times instead, the single-worker encoding throughput gain
is 11.36%, with decoding throughput 2.09% higher. This meets the target of at
least 10% faster compression without increased size or slower average decoding
on the complete 521-file corpus. At eight workers, the mean encoding gain is
9.95%; mean decoding time is 0.32% longer, while its median is 0.08% shorter.
The eight-worker measurements do not independently clear the strict target
when judged by means. Small decode differences should not be treated as an
established speed improvement.

All 521 separate archives total **54,382,292 bytes**, identical to the baseline
at both worker counts in every repetition. No codec sources, format fields,
or existing archives changed, so this optimization requires no recompression.

## Matched upstream comparison

`selected-120.json` repeats the comparison on the original 120-file set
(313,007,679 bytes), including upstream slpz 1.3.0 at
`9f4d952f444037edbbc49af11e5e963ecd4d399f`, fast mode (Zstd level 3).
This run also stayed in high power mode on battery power. Upstream slpz rejects
some inputs in the larger corpus, so its comparison uses only the matched set.

| Tool | Workers | Packed bytes | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|---:|
| Slippack level 3, profiled release | 1 | 12,929,476 | 339.3 | 910.1 |
| slpz fast | 1 | 42,111,709 | 554.9 | 1081.3 |
| Slippack level 3, profiled release | 8 | 12,929,476 | 2159.4 | 6544.4 |
| slpz fast | 8 | 42,111,709 | 3899.3 | 7301.2 |

Slippack output is 69.3% smaller; slpz remains faster in both directions.
The study improves the measured tradeoff but does not establish a Pareto
frontier. The reports retain all samples, including timing outliers, rather
than discarding slow runs.

## Validation

Both `make release-check` and an isolated ordinary build's `make check` pass:
custom packets, record coding, predictor oracles, library API checks, the frozen
v1 fixture, and archive/extraction integration tests. Compiler/target mismatch,
stale source hashes, and altered profile checksums are rejected. Formatting and
Python syntax checks pass. `pooled-validation.json` separately compares actual
CLI output with the baseline byte for byte for 521-file archives at levels 1,
3, 7, and 9, and verifies every decoded member's SHA-1 against its source.
The level 3 pooled archive remains 50,672,315 bytes. All validation archives
are temporary; existing archives are untouched.
`build-info.json` pins build flags and artifact hashes, records profile-guard
checks, and evaluates the single-worker goal using arithmetic mean times.

## Reproduce

Use a separate checkout of the baseline to build its library with `make`.
In the current checkout:

```sh
make release-check
c++ -std=c++20 -O3 -pthread -c \
  benchmarks/2026-11-10-single-file/cpp_runner.cpp -o /tmp/runner-common.o
c++ -std=c++20 -O3 -pthread /tmp/runner-common.o \
  /path/to/baseline/build/libslippack.a -o /tmp/baseline-runner
c++ -std=c++20 -O3 -pthread -Wl,-no_deduplicate /tmp/runner-common.o \
  build/release/libslippack.a -o /tmp/candidate-runner
python3 benchmarks/2026-11-10-encode-speed/benchmark.py \
  --baseline /tmp/baseline-runner --candidate /tmp/candidate-runner \
  --baseline-commit f11d8d5567ffb7207141eb4a9ac8cdc8f1e7ba4f \
  --manifest /path/to/replays.txt --levels 3 --threads 1,8 \
  --repeats 9 --power-mode 2 --output /tmp/profiled-result.json
```

These commands use the tested macOS toolchain. On another toolchain, regenerate
the profile first; on Linux, omit the Apple linker option and link with
`-lcrypto`. No performance claim is made for those unmeasured configurations.
