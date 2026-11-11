# V13 follow-up: remove empty streams and avoid redundant work

This improves the custom codec beyond commit `c03652c`, without a new format
version. Both old and optimized binaries were measured in the same randomized
schedule. The headline result concerns level 3; not every preset improves all
three timing/size medians.

## Level 3

Apple M4 Max, 16 threads, `-O3`, nine warmed repetitions. Times include process
startup and input reads; decode uses `slpz t` and excludes output-file writes.
Every source hash was checked. No compilation or fuzzing ran during these
final timing rounds. Desktop load changed between experiments, so absolute
times from separate runs should not be used to rank presets.

| Dataset | Smaller archive | Less compression time | Less decompression time |
|---|---:|---:|---:|
| Summit sample (140) | 1.86% | 14.18% | 2.86% |
| Mixed (103) | 9.37% | 12.66% | 3.78% |
| Additional Summit (279) | 1.94% | 16.16% | 9.40% |

Combined size falls from **65,307,402 to 62,898,815 bytes**
(3.69% smaller). These are medians on the tested corpora,
not promises for every replay, hardware configuration, or operating condition.

| Dataset | Codec | Archive bytes | Encode seconds | Decode seconds |
|---|---|---:|---:|---:|
| Summit sample (140) | previous_custom | 17,894,563 | 0.244825 | 0.084370 |
| Summit sample (140) | custom | 17,561,851 | 0.210099 | 0.081957 |
| Summit sample (140) | tuned | 18,217,717 | 0.276454 | 0.106296 |
| Mixed (103) | previous_custom | 15,571,355 | 0.248805 | 0.081176 |
| Mixed (103) | custom | 14,112,375 | 0.217294 | 0.078109 |
| Mixed (103) | tuned | 15,016,540 | 0.276534 | 0.101591 |
| Additional Summit (279) | previous_custom | 31,841,484 | 0.661112 | 0.192990 |
| Additional Summit (279) | custom | 31,224,589 | 0.554269 | 0.174853 |
| Additional Summit (279) | tuned | 32,810,653 | 0.716485 | 0.232222 |

The sets contain 521 replays and one provenance TSV (522 files,
1,309,561,719 input bytes). They have all been examined in earlier rounds and
are not unseen holdouts. No filenames, tournament identities, or pretrained
corpus tables determine encoding behavior.

## What changed

1. **Remove absent event streams.** The event-size declaration table names
   commands that sometimes never occur. The encoder nevertheless wrote their
   selectors and plane descriptors, including wide 9464-byte declarations in
   modern replays. Omitting zero-count streams saves 1,450,260 bytes on mixed
   by itself. Original declarations remain verbatim, truncated tails remain
   untouched, and actual command-only events with zero-width payloads survive.
   This also helps legacy/tuned, which receive the same fix for comparisons.
2. **Avoid losing controller-packet trials.** Among large packets with at
   least four controller planes, masked encoding won all 193 mixed and 248
   sample comparisons against direct LZ. Levels below 9 skip that direct
   trial; level 9 retains it. This is a heuristic, and different data can favor
   the other representation.
3. **Build only the history buckets the model uses.** The split search has 18
   intervals, but built 256 histograms per active class. Aggregating into 18
   gives the same split decisions with 93% less histogram storage. The isolated
   change produces byte-identical archives on sample and mixed.
4. **Improve probability rounding.** Round normalized counts to nearest before
   correcting the sum, rather than transferring all fractional remainders to
   the most common symbol. This saves another roughly 8–9 KB on sample/mixed.
5. **Check common mask symbols first.** Decode all-zero/all-one pairs before
   mixed pairs. The focused mixed test cut decode time 8.0% at eight threads
   and 14.4% at one thread; final end-to-end comparisons are the table above.
   The mask representation and state transitions are unchanged.

## Other presets and the LZMA comparison

Five warmed mixed-set repetitions for each additional preset:

| Level | Before bytes | After bytes | Less encode time | Less decode time |
|---|---:|---:|---:|---:|
| 1 | 15,802,949 | 14,344,022 | 5.40% | -1.44% |
| 7 | 15,515,902 | 14,057,206 | 14.83% | 2.75% |
| 9 | 15,455,081 | 13,996,712 | -0.29% | 6.00% |

Negative values are regressions in the measured median: level 1 decode was
1.44% slower, and level 9 encode was essentially flat (0.29% slower). Small
changes are sensitive to desktop noise. The strong claim is the level-3
improvement on every measured set, not uniform domination at every preset.

Tuned receives the same unused-stream fix and numeric predictors. At level 3,
custom is now smaller and has lower encode/decode medians than tuned on all
three sets. On mixed at level 9:

| Codec | Bytes | Encode seconds | Decode seconds |
|---|---:|---:|---:|
| custom | 13,996,712 | 0.347543 | 0.094099 |
| tuned | 14,021,253 | 1.776231 | 0.115422 |

Custom is **24,541 bytes smaller (0.175%)**, takes **80.43% less compression
time (5.11× throughput)**, and **18.47% less decompression time**. This closes
the previously reported size gap on this mixed set. The size margin is narrow;
it does not prove that every individual replay or every general-purpose
compressor is beaten. Tuned is the existing zstd/LZMA selection backend at
this level, using the same improved preprocessing.

## Single-thread check

Five warmed mixed-set repetitions at level 3:

| Codec | Encode seconds | Decode seconds |
|---|---:|---:|
| previous_custom | 2.080537 | 0.507705 |
| custom | 1.814451 | 0.455878 |

The optimized binary takes 12.79% less encode time and 10.21% less decode time,
with the same 9.37% size reduction.

## Experiments retained

`exploratory-results.json` contains the normalization/split-cost sweep, the
isolated changes, and decoder trials. The different decoder comparison orders
matter: branchless symbol selection, explicit unrolling, and deriving frequency
from interval endpoints lost to checking the common extremes first. Those
results concern these implementations, not the general ideas.

`controller-trial-sizes.json` contains the per-packet direct/masked sizes and
profile counts. A zero direct size means the pre-existing float shortcut
already skipped that trial. The table supports the controller shortcut, but
other corpora may prefer a different policy.

## Validation and compatibility

- `make check`: selftest; 3,430 payload checks; 111,292 record checks; 2,118,231
  predictor checks. Updated archive integration covers 12 backend/level
  combinations and 12 fixtures, including unused large declarations,
  declaration-only replays, actual zero-width events, and truncated input.
- All 522 benchmark files match decoded hashes. The old v13 reader also
  validates all new custom and tuned archives across those sets.
- New readers validate v9, v10, v11, v12, and original v13 archives. The original
  v9 reader accepts freshly written legacy output. Native/scalar custom archives
  are byte-identical and cross-decode correctly.
- Fatal zstd/LZMA/brotli encoder tripwires produce the exact custom mixed
  archive. No external payload compressor was introduced.
- Payload tests pass ASan/UBSan. Custom-payload fuzzing completed 129,655 runs
  in 61 seconds (peak reported RSS 588 MB); full custom-archive roundtrip
  fuzzing completed 257,367 runs in 61 seconds (404 MB), with no findings.
  These are short smoke tests, not sustained fuzzing campaigns.

The format stays v13 for custom/tuned and v9 for legacy. No reader upgrade is
required for the new v13 archives. Detailed checks, binary/source hashes, and
all timing samples are in `validation.json` and the benchmark JSON files.

## Reproduction

Build the previous binary separately from commit `c03652c`. Run from the repo:

The historical driver and its command-line options are archived at commit
`5cdb2bf`: [compare_custom.py](https://github.com/Eppie/slippack/blob/5cdb2bf/benchmarks/compare_custom.py).

For the additional mixed experiments, use levels 1/7/9 with five repetitions;
for the single-thread check, use level 3, `--threads 1`, and five repetitions.
See `fuzz/README.md` for standalone payload and full archive fuzzing.
