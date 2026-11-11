# V14: quadratic motion, numeric inputs, and faster transforms

Compared with the custom codec at the start of this round (`5c1659d`, code from
`22e61aa`), the combined corpus has **10.32% smaller archives, 11.45% less compression time, and 13.99% less decompression time**.

These are measured improvements at level 3 on an Apple M4 Max with 16 threads.
They meet the further-10% target in each category **in aggregate**, not for every
dataset, replay, preset, or thread count. The single-thread mixed result below
does not meet that target for either timing.

## Measurements

Nine warmed repetitions per dataset, with previous custom, current custom, and
equally updated tuned backends in a randomized schedule. No compilation or
fuzzing ran during these final measurements. Times include process startup and
input/archive reads; decode uses `slpz t`, including reconstruction but excluding
output-file writes. Every decoded source hash was verified.

| Dataset | Smaller archive | Less compression time | Less decompression time |
|---|---:|---:|---:|
| Summit sample (140) | 10.66% | 9.71% | 15.25% |
| Mixed (103) | 6.38% | 3.89% | 13.69% |
| Additional Summit (279) | 11.92% | 14.98% | 13.55% |
| **Combined** | **10.32%** | **11.45%** | **13.99%** |

Combined size falls from **62,898,815 to 56,405,346 bytes**. Combined compression
time is the sum of the three dataset medians: **0.964463 → 0.854057 seconds**; decode
is **0.332932 → 0.286347 seconds**. The time reductions are not throughput percentages:
throughput rises 12.93% for compression and 16.27% for decompression.

| Dataset | Backend | Bytes | Encode seconds | Decode seconds |
|---|---|---:|---:|---:|
| Summit sample (140) | previous_custom | 17,561,851 | 0.220379 | 0.080539 |
| Summit sample (140) | custom | 15,690,443 | 0.198971 | 0.068255 |
| Summit sample (140) | tuned | 16,385,208 | 0.235590 | 0.092325 |
| Mixed (103) | previous_custom | 14,112,375 | 0.202404 | 0.072925 |
| Mixed (103) | custom | 13,212,487 | 0.194525 | 0.062939 |
| Mixed (103) | tuned | 14,108,409 | 0.231923 | 0.079094 |
| Additional Summit (279) | previous_custom | 31,224,589 | 0.541680 | 0.179467 |
| Additional Summit (279) | custom | 27,502,416 | 0.460561 | 0.155153 |
| Additional Summit (279) | tuned | 29,184,581 | 0.596218 | 0.201815 |

The corpus has 521 replays and one provenance TSV: 522 files and 1,309,561,719
raw bytes. All three sets have been examined during development; the additional
Summit set is not an unseen holdout. Encoding never selects behavior by filename
or tournament identity. Desktop load varies, so compare paired results within
a run rather than absolute medians across experiments.

## Retained changes

1. **Quadratic old-format positions.** For 29–71-byte post-frame records, predict
   the next position from the previous position, velocity, and acceleration.
   Preserve separately rounded binary32 operations and exact modular bit
   residuals. Records containing velocity fields keep their existing predictor.
   NEON computes both axes together; an independent scalar oracle verifies the
   encoded residuals, including exceptional float patterns.
2. **Numeric controller residuals.** Keep the raw-axis-indexed last-value tables,
   but encode signed modular bit differences instead of XOR for controller floats.
   Buttons and triggers retain XOR.
3. **Selective dictionaries.** Skip candidates containing one varying byte with
   at least 128 values; the other bytes already have explicit constant modes.
   Cache alphabet counts. Build constant dictionaries directly and map small
   one-byte alphabets without hashing every full record.
4. **Smaller transform probes.** Tuned/custom use three 256-byte sample windows
   rather than 4096-byte windows. Compared with the same new predictors using
   the longer probe, combined size rises only 0.24% (133,552 bytes).
5. **Partial vector tiles.** Vectorize the 1–15-byte record-width remainder in
   both transpose directions. Exact-width loads/stores avoid touching bytes
   outside partial tiles; selftest covers every remainder width.
6. **Avoid unnecessary lifecycle work.** Limit short parallel phases to the
   available task count. Release allocator caches between decode waves, without
   a redundant allocator-wide sweep after the final wave immediately before
   process exit. This helps the short multithreaded runs more than single-thread
   runs. The decode memory admission budget is unchanged.

Custom and tuned now write **v14** because predictor semantics changed. New
readers retain v9–v13 decoding. Old v13 readers reject v14 at the header. The
legacy default still writes v9; custom remains opt-in. No entropy format change
or external payload compressor was added.

## Single-thread mixed check

Five warmed repetitions at level 3:

| Backend | Encode seconds | Decode seconds |
|---|---:|---:|
| previous_custom | 1.764318 | 0.429949 |
| custom | 1.611400 | 0.428415 |
| tuned | 1.896443 | 0.630423 |

Compression time falls 8.67%; decompression is essentially flat (0.36% less
time). Size is still 6.38% smaller. This is why the headline is explicitly
limited to the combined 16-thread benchmark.

## Experiments retained or rejected

`exploratory-results.json` records the measured sweeps and unsuccessful speed
trials. These were used for selection, not as additional final timing samples.

- Polynomial orders 1–8: order 2 won for old positions; higher orders were
  progressively worse. Adaptive linear/quadratic choices also lost. Replacing
  the existing velocity predictor on newer records increased size.
- Dictionary thresholds 256/128/64/2: 128 and 64 were close; removing almost
  all one-varying-byte dictionaries was worse. Constant dictionaries remain useful.
- Numeric trigger residuals added little and were worse on mixed, so triggers
  retain XOR.
- Two-axis controller caches improved size further (roughly another 0.5–1.3%
  on the measured sample/mixed variants), but increased cache state. They were
  not integrated or claimed as measured speed improvements.
- Per-plane entropy coding of masked values lost to the shared LZ dictionary.
  Model headers were too small to offer the large anticipated savings.
- Prebuilt/unrolled presence tables, compact lookup tables, reciprocal division,
  and four independent mask sections did not provide a reliable end-to-end win.
  The four-section design changed size by only a few kilobytes, but was slower.
- Direct output assembly, blocked scalar transpose tails, and linked post-event
  lookup were not retained; their timing gains were inconsistent. Rectangular
  vector tiles were the successful transpose variant.

## Validation

- `make check`: all kernel checks; 3,430 custom-payload checks; 111,292 record
  checks; 2,126,359 predictor checks. The latter include independent quadratic
  and controller residual oracles, arbitrary float bits, and version boundaries.
- All 12 backend/level combinations pass the 12 archive fixtures and exact
  extraction checks, including empty/unused declarations, zero-width events,
  truncation, and old record formats. All 522 benchmark file hashes match.
- New reader verifies v9/v10/v11/v12/v13 archives. An original v9 reader verifies
  newly written legacy output. Old v13 rejects new v14 at the header.
- Native/scalar custom sample archives are byte-identical and cross-decode
  against all 140 source hashes.
- Fatal zstd/LZMA/Brotli encoder tripwires produce the identical custom mixed
  archive, confirming custom compression uses no external payload compressor.
- ASan/UBSan predictor fuzz: 69,731 runs in 61 seconds (458 MB peak reported RSS).
  Full custom archive roundtrip fuzz: 195,388 runs in 61 seconds (380 MB).
  Neither short smoke run found an error; these are not sustained campaigns.

`validation.json` includes source and binary hashes. Each dataset JSON retains
all timing samples; `summary.json` specifies the aggregate calculation.

## Reproduction

Build the baseline from commit `5c1659d` in a separate checkout. Then:

The historical driver and its command-line options are archived at commit
`5cdb2bf`: [compare_custom.py](https://github.com/Eppie/slippack/blob/5cdb2bf/benchmarks/compare_custom.py).

Repeat with `mixed-files.txt` and the mixed root, then `holdout-files.txt` with
the Summit root. The single-thread check uses mixed, `--threads 1 --repeats 5`.
