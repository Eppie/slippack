# Encoding speed, 2026-11-10

Compare the encoder at `7ca9b4e` with the current byte-counting and plane-screening
changes. The v1 format, compression settings, dictionary sorting, and decoder
are unchanged.

The encoder counts byte frequencies in eight independent lanes so repeated
values do not serialize updates to one counter. Before trying sparse/run
plane packets, it finds a possible dominant value in a bounded prefix and
counts exceptions and runs in blocks of 64 bytes. It stops once both counts
exceed the existing thresholds. A qualifying dominant value must be the
majority of that prefix, so this makes exactly the same selection as the old
full histogram.

## Method

Apple M4 Max, Apple Clang 17, C++20, `-O3`; nine measured repetitions after one
warmup per setting, randomized interleaving of the two binaries. Inputs are
preloaded into RAM. Each replay gets its own archive; eight workers process
eight independent replays. Timings include full encoding and reconstruction,
and exclude file I/O, startup, byte verification, and compressed fingerprints.

`selected-120.json` uses the same 120 replays (313,007,679 bytes) as the four-tool
comparison, at levels 1, 3, and 9 with one and eight workers. `additional-401.json`
uses the other 401 replays from the existing study at level 3. Both sets have
been used in earlier project studies. Reports include input hashes, executable
hashes, source hashes, all timing samples, and per-file compressed fingerprints.

Every run checks decoded bytes against the source. The driver additionally
requires identical compressed sizes and SHA-1 fingerprints between both
encoders and worker counts at each level. `combined-validation.json` separately
records direct byte comparisons of one 521-replay archive at levels 1, 3, 7,
and 9; all bytes matched.

## Results

Throughput is original MB per second, using each setting's median elapsed time.
The machine was in normal desktop use; the raw samples include scheduling
variability. Compare the interleaved binaries within this run rather than
absolute throughput from separate experiments.

| Replays | Level | Workers | Encode before | Encode after | Encode gain | Decode before | Decode after |
|---|---:|---:|---:|---:|---:|---:|---:|
| 120 | 1 | 1 | 233.0 | 298.5 | 28.1% | 882.3 | 880.5 |
| 120 | 3 | 1 | 215.4 | 247.9 | 15.1% | 892.2 | 903.1 |
| 120 | 9 | 1 | 146.7 | 160.4 | 9.3% | 845.0 | 874.3 |
| 120 | 1 | 8 | 1671.8 | 1882.1 | 12.6% | 6467.9 | 6384.8 |
| 120 | 3 | 8 | 1645.0 | 1848.9 | 12.4% | 6548.0 | 6580.2 |
| 120 | 9 | 8 | 1180.7 | 1294.4 | 9.6% | 6634.3 | 6531.3 |
| 401 | 3 | 1 | 216.5 | 267.6 | 23.6% | 874.8 | 893.5 |
| 401 | 3 | 8 | 1695.9 | 2053.5 | 21.1% | 6773.8 | 6727.6 |

At default level 3, encoding throughput increased 12.4–23.6% across these
configurations. Decoding medians stayed within 2.1% of the baseline. Compressed
sizes and fingerprints were identical in every case.

The 120 inputs also fit in one 12,055,073-byte archive at level 3, versus
12,929,476 bytes for separate archives: 3.85% of the original input and 6.76%
smaller than separate storage. This size is unchanged by the optimization.

## Reproduction

Build the baseline library at the pinned commit, then compile both runners from
the same current harness:

```sh
git worktree add --detach build/encode-speed-base 7ca9b4e
make -C build/encode-speed-base build/libslippack.a
make build/libslippack.a
c++ -std=c++20 -O3 -pthread benchmarks/2026-11-10-single-file/cpp_runner.cpp \
  build/encode-speed-base/build/libslippack.a -o build/encode-speed-baseline
c++ -std=c++20 -O3 -pthread benchmarks/2026-11-10-single-file/cpp_runner.cpp \
  build/libslippack.a -o build/encode-speed-candidate
python3 benchmarks/2026-11-10-encode-speed/benchmark.py \
  --baseline build/encode-speed-baseline --candidate build/encode-speed-candidate \
  --manifest /path/to/selected-120.txt --output /tmp/selected-120.json
python3 benchmarks/2026-11-10-encode-speed/benchmark.py \
  --baseline build/encode-speed-baseline --candidate build/encode-speed-candidate \
  --manifest /path/to/additional-401.txt --output /tmp/additional-401.json --levels 3
```

Add `-lcrypto` to the runner link commands on Linux. A manifest contains one
absolute source path per line, in the order recorded in the report's `inputs`.
The selected set is the first 40 valid replays from each of the three existing
study lists after sorting names by SHA-256, as in the four-tool benchmark driver.
The additional set contains the remaining valid replays in original list order.
