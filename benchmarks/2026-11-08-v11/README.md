# Specialized custom codec benchmark — v11

Apple M4 Max, 16 threads, `-O3`, level 3. Nine warmed repetitions with
backend order randomized each round. Times include process startup and input
reading. Decode is `slpz t`: reconstruction included, output-file writes
excluded. All file hashes were verified before timing repetitions.

| Dataset | Codec | Archive bytes | Encode seconds | Decode seconds |
|---|---|---:|---:|---:|
| Summit 11, 140 files | legacy | 22,436,101 | 0.5089 | 0.1107 |
| Summit 11, 140 files | tuned | 21,577,020 | 0.2702 | 0.1100 |
| Summit 11, 140 files | custom | 21,002,920 | 0.2513 | 0.0911 |
| GOML 2019 + Bair 2026, 103 files | legacy | 20,210,993 | 0.4611 | 0.1028 |
| GOML 2019 + Bair 2026, 103 files | tuned | 17,998,650 | 0.2465 | 0.0939 |
| GOML 2019 + Bair 2026, 103 files | custom | 17,902,407 | 0.2356 | 0.0830 |
| Additional Summit 11, 279 files | legacy | 41,291,606 | 1.0024 | 0.1934 |
| Additional Summit 11, 279 files | tuned | 39,575,172 | 0.4891 | 0.2004 |
| Additional Summit 11, 279 files | custom | 38,096,827 | 0.4693 | 0.1649 |

Relative to tuned LZMA at level 3:

| Dataset | Smaller archive | Less encode time | Less decode time |
|---|---:|---:|---:|
| Summit 11, 140 files | 2.66% | 7.01% | 17.19% |
| GOML 2019 + Bair 2026, 103 files | 0.53% | 4.41% | 11.53% |
| Additional Summit 11, 279 files | 3.74% | 4.04% | 17.69% |

The three sets contain 521 replays and one small provenance TSV, totaling
1,309,561,719 source bytes. The additional 279 replays were initially
held out, then used to identify an encoding regression and validate the SIMD
fix. They are additional validation data, not a still-unseen test set.

Tuned and custom use identical parsing, field dictionaries, per-plane
transforms, motion/input predictors, and input ordering. Tuned LZMA uses a
block-sized encoder dictionary. Custom payloads use no external compressor;
the CLI keeps the libraries to read older formats. A separate build that
makes every zstd/LZMA/brotli encoding call fatal produced the byte-identical
mixed custom archive.

## Scope and the high-compression comparison

This is a level-3, M4 Max result, not superiority over every LZMA preset or
every CPU/corpus. The ARM exception pack/scatter paths have a portable scalar
fallback; scalar and native archives were verified byte-identical. Timing
samples and binary/input-list hashes are in the adjacent JSON files.
The compression-speed margins are modest, so retain repeat measurements
when evaluating other machines or workloads.

On mixed at level 9 (three warmed repetitions), tuned LZMA produced
17,019,658 bytes in 1.1608 s and decoded in 0.0969 s. Custom produced
17,832,856 bytes in 0.2542 s and decoded in 0.0781 s: custom is 4.78% larger,
while LZMA takes about 4.6 times as long to encode. The custom high tier does
not yet win on size. See `level9-mixed.json`.

## Validation

- 3,430 standalone checks, both native and scalar; native ASan/UBSan passed.
- Archive hashes and extraction at levels 1/3/9 for all three backends,
  including empty, invalid, truncated, in-progress, and exceptional-float data.
- Every source file in all three benchmark sets matches its decoded hash.
- The new reader decoded v9 (419 files), v10 custom, and v10 tuned archives.
  The old reader decoded legacy output and rejected v11 cleanly.
- LibFuzzer plus ASan/UBSan: 80,004 runs in 61 seconds, no findings, 610 MB
  peak reported RSS. This is a smoke run, not a sustained campaign.

The default backend is unchanged. Use `--codec custom` to write v11.
`CUSTOM_FORMAT.md` documents the representation. Earlier experiments and
source fragments are retained in `../2026-11-08-model-study/`.

## Reproduction

```sh
make check
python3 benchmarks/compare_custom.py --root /path/to/summit \
  --list benchmarks/2026-11-08-v11/sample-files.txt --repeats 9 --threads 16
python3 benchmarks/compare_custom.py --root /path/to/mixed \
  --list benchmarks/2026-11-08-v11/mixed-files.txt --repeats 9 --threads 16
python3 benchmarks/compare_custom.py --root /path/to/summit \
  --list benchmarks/2026-11-08-v11/holdout-files.txt --repeats 9 --threads 16
```
