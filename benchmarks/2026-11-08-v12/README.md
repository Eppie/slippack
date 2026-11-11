# Sorted, compressed field dictionaries — v12

Apple M4 Max, level 3, 16 threads, `-O3`. Nine warmed repetitions with
backend order randomized each round. Times include process startup and input
reading. Decode is `slpz t`: reconstruction included, output writes excluded.
File hashes were verified before the timing repetitions.

Previous custom is commit `b1c9a01` (v11). Both new backends receive the same
dictionary sorting, transform selection, dictionary compression framing, and
expansion improvements. Tuned uses its zstd/LZMA backend; custom uses no
external compressor. The default remains legacy, writing v9.

| Dataset | Codec | Archive bytes | Encode seconds | Decode seconds |
|---|---|---:|---:|---:|
| Summit sample (140) | previous_custom | 21,002,920 | 0.2653 | 0.0961 |
| Summit sample (140) | tuned | 19,523,097 | 0.2707 | 0.1063 |
| Summit sample (140) | custom | 19,164,566 | 0.2637 | 0.0888 |
| GOML + Bair mixed (103) | previous_custom | 17,902,407 | 0.2324 | 0.0819 |
| GOML + Bair mixed (103) | tuned | 16,140,451 | 0.2599 | 0.0981 |
| GOML + Bair mixed (103) | custom | 16,253,126 | 0.2415 | 0.0806 |
| Additional Summit (279) | previous_custom | 38,096,827 | 0.4818 | 0.1710 |
| Additional Summit (279) | tuned | 35,362,289 | 0.4925 | 0.1898 |
| Additional Summit (279) | custom | 34,324,479 | 0.4723 | 0.1604 |

## Change from previous custom

Positive values below mean a reduction. Negative encode values mean more time.

| Dataset | Smaller archive | Less encode time | Less decode time |
|---|---:|---:|---:|
| Summit sample (140) | 8.75% | 0.61% | 7.60% |
| GOML + Bair mixed (103) | 9.21% | -3.92% | 1.58% |
| Additional Summit (279) | 9.90% | 1.98% | 6.19% |

Total archive size falls from 77,002,154 to 69,742,171 bytes: **9.43% smaller**.
Decode medians improve on each set. Compression time is broadly unchanged;
mixed is 3.92% slower than v11 in this run. The timing distributions overlap,
and the additional-Summit run includes several system-load outliers in all
backends. Small speed differences should be rechecked on the target machine.

Against the equally improved tuned level-3 backend, custom is 1.84% smaller
on the sample and 2.93% smaller on additional Summit, but **0.70% larger on
mixed**. It uses 2.6–7.1% less encode time and 15.5–17.9% less decode time
than tuned in these measurements. This is not a claim of winning every axis
on every dataset or preset.

The corpus is 521 replays plus one provenance TSV, totaling 1,309,561,719
source bytes. All three sets have been examined during this research; the
additional 279 files are additional validation data, not an unseen test set.
No model uses replay identities, filenames, or corpus-specific trained tables.

## High-compression comparison

At level 9 on mixed, five warmed repetitions give:

| Codec | Archive bytes | Encode seconds | Decode seconds |
|---|---:|---:|---:|
| Previous custom | 17,832,856 | 0.2528 | 0.0794 |
| Tuned | 15,140,989 | 1.1639 | 0.0877 |
| Custom | 16,176,367 | 0.2521 | 0.0793 |

Custom is **6.84% larger** than the improved tuned high tier, which takes
4.62× as long to encode. The high-tier size target remains open.
See `level9-mixed.json`. Custom v12 is 9.29% smaller than previous custom at
this level, with essentially unchanged encode/decode times in this run.

## What changed

- Sorted dictionary values make both metadata and temporal index differences
  more compressible. Descriptors remain directly scannable; value bytes use
  a size-checked tagged frame only when smaller than raw storage.
- Four dictionary transforms cover shuffled bytes, byte XOR, byte subtraction,
  and whole-field differences before shuffling. All reset at field boundaries.
- Fixed-width dictionary expansion validates indexes once, uses constant fills
  and NEON register lookups for small alphabets, and retains scalar fallbacks.
- Constant planes use word comparisons and skip redundant histograms/copies.

These changes require container v12 for tuned and custom. V9/v10/v11 archives
remain readable, and older readers reject v12 cleanly. `CUSTOM_FORMAT.md`
specifies the new representation. The rANS, LZ, and mask payload formats are
unchanged from v11.

## Experiments retained as evidence

The adjacent `exploratory-results.json` records preliminary trials, with their
original repetition counts. They are not substitutes for the final measurements.

- Compressing unsorted dictionaries first cut mixed by about 1.19 MB. Sorting
  plus the level-3 dictionary search saved roughly another 0.44 MB; whole-field
  differences saved a further 20,570 bytes. These are sequential observations,
  not independent ablations of every setting.
- A 32-bit packed rANS lookup tied the existing byte lookup in 19 alternating
  decodes (83.98 vs 83.85 ms medians), with more table storage. It was dropped.
- Finer distance slots increased mixed from 16,273,696 to 16,309,367 bytes and
  sample from 19,186,867 to 19,203,901 bytes. The existing distance format remains.
- Rechecking match-byte costs 4/5/6/7 still favored 6 on both tuning sets.
- Detecting constant dictionary fields before hashing did not yield a clear
  speed benefit in seven paired rounds, so that extra pass was dropped.

`packed-rans.patch` and `distance-slots.patch` retain the rejected coding
variants against the current headers for later investigation. The distance
variant changes the LZ representation and needs a separate experimental
version before reuse.

These results apply to the tested implementations; they do not establish that
better distance models, table layouts, or constant detection cannot help.

## Validation

- All 522 input hashes match after decoding; all backends also pass extraction
  and hash checks for seven synthetic fixtures at levels 1/3/9.
- Standalone payload tests: 3,430 checks under ASan/UBSan.
- Record tests: 111,292 assertions under ASan/UBSan and with custom/dictionary
  SIMD disabled. This includes 180 expansion cases, 80 forced dictionary
  packets, all four transforms, index limits, field resets, arithmetic wrap,
  truncated frames, invalid sizes/transforms, and overlapping fields.
- New reader: v9 (419 files), v10 custom/tuned (140 each), and v11 (140).
  Both v9 and v11 readers reject v12 cleanly; the v9 reader accepts freshly
  encoded legacy output.
- Native/scalar custom archives are byte-identical; both readers check both
  against 140 source-file hashes. A build with fatal zstd/LZMA/brotli encoder
  tripwires produces the byte-identical mixed custom archive.
- New dictionary/record libFuzzer target: 180224 executions in 61 seconds,
  with ASan/UBSan and no findings. Peak reported RSS: 347 MB.
  This is a short smoke run, not a sustained fuzzing campaign.

Detailed results are in `validation.json`. No production replay data or
benchmark archives are checked into Git.

## Reproduction

The historical driver and its command-line options are archived at commit
`5cdb2bf`: [compare_custom.py](https://github.com/Eppie/slippack/blob/5cdb2bf/benchmarks/compare_custom.py).

Build v11 separately from commit `b1c9a01`. JSON files retain all samples,
input-list hashes, and both binary hashes. `fuzz/README.md` gives sanitizer
fuzzing commands; generated corpora, archives, and binaries remain ignored.
