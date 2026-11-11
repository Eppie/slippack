# Level 3 allocation and grouping changes

Compare `cf99bea9ee37cc00eb0fd14a6ba87bf9002400c4` with the current encoder.
The final changes preserve compressed bytes and use the same decoder and v1
format. Existing archives were not rewritten.

## Changes

- Partition each event stream into contiguous entity groups in one pass.
  Compression tasks borrow disjoint mutable ranges, apply prediction in place,
  and keep those ranges alive until the plan's last task finishes. Previously,
  each task scanned the entire stream and allocated its own group copy.
- Build raw packet/channel buffers only when compressed candidates lose.
- Skip literal context labels when all labels are the same. Such labels always
  select the existing merged model. Prepare residuals and histograms only at
  search depths that use them.
- Collect dictionary values in insertion order during hash lookup, avoiding a
  scan of the hash table when accepting each dictionary.

Alternative candidate layouts and simplified probability models were explored
but did not consistently improve speed while meeting the size requirement.
The [hardware profile](../2026-11-10-level3-profiling/README.md) records the
measurements that guided this work.

## Method and validation

Apple M4 Max, Apple Clang 17, C++20 `-O3`. Each replay is compressed into its own
archive. Inputs are loaded into RAM before timing. One worker processes the
files sequentially; eight workers process independent replays. Each setting
gets one warmup and nine measured repetitions, randomized and interleaved
between binaries. Throughput uses original decimal MB divided by median
elapsed time. File I/O, verification, and compressed fingerprints are outside
the timers. The machine remained in normal desktop use.

The benchmark polls macOS power mode and AC/battery source every second and
rejects runs with an observed change. The reports record the initial and final
settings. This cannot exclude a transition shorter than the polling interval.
Earlier exploratory batches spanning power-mode changes were discarded.

`selected-120.json` uses the 120-file comparison set (313,007,679 original
bytes), also testing upstream slpz fast on those same files. `additional-401.json`
uses the remaining 401 files (996,540,678 original bytes). Both sets were
previously examined during development. The larger set includes inputs rejected
by upstream slpz, so its comparison is limited to the matched 120-file set.

Every measured Slippack run verifies reconstructed bytes, compressed sizes,
and SHA-1 fingerprints against the baseline. Reports contain all timing samples,
input hashes, executable hashes, and source hashes. `pooled-validation.json`
separately records direct byte comparisons of 521-file archives at levels 1,
3, 7, and 9, plus reconstructed member hashes checked against the sources.

The full test suite and AddressSanitizer/UndefinedBehaviorSanitizer integration
tests pass. New tests cover uneven, interleaved groups; 1, 2, 17, and 255 groups;
the 256-group fallback; and identical output with one and eight workers.

## Results

All final runs used high power mode on AC power, with no observed power-state
changes. Rates are original MB/s; gains compare medians within the same run.

| Replays | Workers | Encode before | Encode after | Encode gain | Decode before | Decode after |
|---|---:|---:|---:|---:|---:|---:|
| 120 | 1 | 298.7 | 306.4 | 2.6% | 919.2 | 919.4 |
| 120 | 8 | 1869.8 | 2019.7 | 8.0% | 6565.3 | 6643.7 |
| 401 | 1 | 284.7 | 290.8 | 2.2% | 912.5 | 914.2 |
| 401 | 8 | 1985.7 | 2048.1 | 3.1% | 6788.3 | 6766.9 |

The encoding gains are modest and consistent across these four configurations.
Decode medians range from 1.2% faster to 0.3% slower with unchanged decoder code;
these small differences are within the run-to-run variation in the raw samples.
There is no compressed-size tradeoff: the separate archives total 12,929,476
bytes for 120 replays and 41,452,816 bytes for 401 replays, identical to baseline.
The combined 521-file archive is also unchanged at 50,672,315 bytes at level 3.

The fresh upstream comparison uses slpz 1.3.0 at
`9f4d952f444037edbbc49af11e5e963ecd4d399f`, fast mode (Zstd level 3), with the
same 120 files and timing method:

| Tool | Workers | Packed / original | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|---:|
| Slippack level 3 | 1 | 4.13% | 306.4 | 919.4 |
| slpz fast | 1 | 13.45% | 550.3 | 1047.6 |
| Slippack level 3 | 8 | 4.13% | 2019.7 | 6643.7 |
| slpz fast | 8 | 13.45% | 3925.6 | 7525.4 |

Slippack's 12,929,476 bytes are 69.3% smaller than slpz fast's 42,111,709 bytes.
Slpz remains faster at both encoding and decoding. This change improves
Slippack's existing size/speed tradeoff; it does not establish a Pareto frontier.

## Reproduction

Build the baseline and candidate libraries, then link the same runner against
each, preserving the archive's object order:

```sh
git worktree add --detach build/level3-memory-base cf99bea9
make -C build/level3-memory-base build/libslippack.a
make build/libslippack.a
c++ -std=c++20 -O3 -pthread benchmarks/2026-11-10-single-file/cpp_runner.cpp \
  build/level3-memory-base/build/libslippack.a -o build/level3-memory-baseline
c++ -std=c++20 -O3 -pthread benchmarks/2026-11-10-single-file/cpp_runner.cpp \
  build/libslippack.a -o build/level3-memory-candidate
python3 benchmarks/2026-11-10-encode-speed/benchmark.py \
  --baseline build/level3-memory-baseline --candidate build/level3-memory-candidate \
  --baseline-commit cf99bea9ee37cc00eb0fd14a6ba87bf9002400c4 \
  --manifest /path/to/selected-120.txt --levels 3 --threads 1,8 \
  --repeats 9 --power-mode 2 --output /tmp/selected-120.json
```

Repeat with the additional manifest. Add `--slpz /path/to/replay-codec-benchmark`
to include the upstream Rust runner described in the
[single-file study](../2026-11-10-single-file/README.md). Add `-lcrypto` to C++
link commands on Linux and omit `--power-mode`. Manifests list one absolute
input path per line, ordered as the report's `inputs` entries.
