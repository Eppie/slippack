# SLPZ v15 optimization round

The target for this round is a further 10% reduction in archive bytes,
compression elapsed time, and decompression elapsed time relative to v14
commit `f8e4e02`. The legacy v9 default is unchanged.

## Results

Across all 522 replays, relative to the preceding v14 custom codec at level 3:

| Metric | v14 | v15 | Reduction |
|---|---:|---:|---:|
| Archive bytes | 56,405,346 | 50,700,480 | 10.11% |
| Compression time | 0.730751 s | 0.644707 s | 11.77% |
| Decompression time | 0.247779 s | 0.221209 s | 10.72% |

The time percentages are reductions in elapsed time, not increases in throughput.

| Set | Files | Size reduction | Compression-time reduction | Decompression-time reduction |
|---|---:|---:|---:|---:|
| sample | 140 | 9.31% | 10.09% | 8.95% |
| mixed | 103 | 10.11% | 11.14% | 9.72% |
| holdout | 279 | 10.58% | 13.02% | 12.24% |

The 10% target applies to each combined metric, not to every individual replay or set.

## Method

Apple M4 Max, 16 threads, custom level 3, identical `-std=c++20 -O3` builds.
Each set gets a warmup and 31 measured runs per binary; binary order is
interleaved with a fixed random seed. Compression includes input reads and
archive writes. Decompression uses `t`, reconstructing all output bytes without
writing extracted files. Hash verification runs outside the timed measurements.
Aggregate timings sum each set's median, matching the preceding comparison.
`results.json` contains every observation and the executable hashes.

The 522 files comprise 140 sample, 103 mixed, and 279 additional replays;
`holdout-files.txt` retains its historical filename. All three sets have been
examined during this and previous development rounds. They are not an unseen
validation set. File-size improvements are deterministic; short elapsed-time
measurements vary with scheduling and desktop load.

## Changes

- Exact reversible mappings for common controller fractions and integer
  counters, plus paired-axis controller history.
- Animation entry/step prediction by action state and two-sample attack
  velocity prediction. Arbitrary float bit patterns remain lossless.
- Complete numeric fields get dictionary priority. One-value dictionaries
  omit their index planes entirely in v15.
- Four-bit rANS presence symbols retain the binary probability model's
  precision. Reverse encoding reconstructs history without a trace buffer.
- Fixed-width dictionary loads, repeated-value lookup caching, sampled
  sparse-mode screening, and incremental entropy scores reduce encoder work.
- Dictionary expansion uses bounded blocks; final replay reconstruction
  avoids an intermediate reordered copy. Packed literal lookup entries
  reduce decoder dependencies. Mask exceptions restore straight into final
  planes, and joint-mask CDF starts/frequencies share cache lines. Custom opaque
  payloads use 1 MiB chunks to avoid long trailing decode tasks.
- Native two-axis velocity prediction, grouped large-palette gathers, and
  deterministic mask shortcuts reduce decoder instructions. RNG and cross-frame
  restoration are fused with the final record copy. Decode destinations skip
  zero initialization only where every byte is explicitly filled before use.

V15 changes predictor and constant-index semantics, so old readers reject it
at the header. The new reader retains v9–v14 compatibility. Both tuned and
custom writers use v15 semantics; custom payload compression remains wholly
independent of zstd, LZMA, and Brotli.

## Experiments retained for future work

`exploratory-results.json` and the sweep JSON files record intermediate
results. These are development probes, not the final timing comparison.

- Pooling whole predicted record groups saved about 5–6% of their packed
  size, but enlarged dictionaries and slowed encode/decode substantially.
- Pooling only exception values saved roughly 5–6% of that substream at
  batch sizes 16–32. Its processing time still increased, so it was not
  integrated. Bounded shared models remain a possible future direction.
- Additional old-format position predictors (least-squares fits, snapped
  acceleration, smoothed velocity) did not improve the tested sets.
- Changing LZ length/distance prices produced less than 0.1% improvement;
  external match-history literal contexts produced little or mixed benefit.
- Bytewise presence coding lost several percent in archive size. Bit
  shuffling was substantially worse. Four-bit joint coding at scale 65536
  preserved size far better than earlier low-precision joint models.
- Action-dependent prediction helped the animation counter strongly;
  predicting broad post-frame byte ranges from action-state caches did not.
- Extra field sweeps showed linear attack-velocity prediction was useful;
  applying it to percent or shield was worse.
- Reduced controller-cache sizes lost enough bytes to miss the size target.
- Smaller opaque chunks and decoded-size scheduling were measured separately.
  The 1 MiB custom chunks were retained; scheduling by decoded size was not.

The first 15-run confirmation (`first-confirmation.json`) met the size and
compression-time targets but reached only 8.71% less decompression time.
The second confirmation (`second-confirmation.json`, 31 runs) reached
9.42% less decompression time. Later vector and lookup-table improvements are
evaluated in the final results; neither earlier run is counted as a success.

## Validation

`validation.json` records source/executable hashes, all 522 source-file hash
matches, old-archive checks, scalar/vector cross-checks, and encoder tripwires
that abort if a custom archive calls an external compressor. `make check`
includes both explicit v14 and implicit v15 constant dictionaries, 63/64/65/66
byte controller boundaries, special float bit patterns, and forced four-bit
presence decoding at short and non-aligned lengths. ASan/UBSan fuzz targets
cover whole-archive round trips, predictor/record composition, and standalone
payload parsing and round trips. All 522 hashes are also checked with macOS
`MallocPreScribble=1`, so newly allocated decode destinations do not silently
rely on zero-filled pages. Modern velocity prediction is checked against an
independent scalar oracle on arbitrary float bit patterns.

## Reproduce

```sh
python3 benchmarks/2026-11-08-v15/benchmark.py \
  --baseline /path/to/v14/slpz --current ./slpz \
  --study /path/to/slpz-study --threads 16 --repeats 31 \
  --output benchmarks/2026-11-08-v15/results.json
```

The study directory contains `summit/` and `mixed/`; input lists are tracked
beside this report. Replay data and generated archives are not committed.
