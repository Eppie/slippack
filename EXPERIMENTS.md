# Experiment findings

What measurement shows about compressing Slippi replays: the structure in the
data, the techniques that pay for themselves, and the ones that do not. Figures
come from development corpora on an Apple M4 Max and name the corpus they were
measured on. The studies under [benchmarks/](benchmarks/) hold the measurements
behind the README's headline numbers.

## Structure in replay data

- **A post-frame record repeats in the next pre-frame record.** Melee carries
  end-of-frame state into the following frame's pre-frame event for the same
  entity. Byte equality per field: x 98.7%, y 99.5%, facing 99.4%, percent
  99.96%, action state 97.5%. Predicting one from the other removes 59% of the
  pre-frame stream.
- **RNG seeds follow Melee's generator.** The seed advances by
  `s * 214013 + 2531011`, so a step count replaces the four seed bytes for
  99.995% of frames. Frame-start events reduce to a frame number and a seed
  (366 KB to 10 KB on 140 Summit 11 replays). Step counts are periodic per
  entity, so emitting them per entity instead of in global order saves a
  further 21%. A low-bit phase lookup with verified affine jumps finds the step
  count without walking the generator.
- **Analog fields are quantized.** Many four-byte float columns take at most
  255 distinct values, and many two-byte fields such as action states do too.
  Replacing them with one-byte dictionary indexes exposes structure that
  byte planes hide.
- **Dictionary metadata is compressible.** Sorting dictionary values by their
  big-endian bytes, remapping the indexes, and compressing the value frames
  shrinks both the stored values and the index deltas: 9.4% smaller archives on
  522 files.
- **Exception positions are shared across float planes.** Coding where a
  prediction fails separately from the failing values, with presence
  information shared between related planes, beats coding each plane alone.
- **Position responds to velocity, not to position history.** Recorded
  velocities predict the next position well. Records without velocity fields
  (129 of 140 Summit 11 files) use quadratic extrapolation from earlier
  positions. Second-order prediction on float bits alone loses to mantissa
  rounding noise.
- **Small side streams repeat between replays.** Game-start blocks, Gecko code
  lists, and message splits from all files of one setup compress far better
  pooled than alone. Large per-file streams gain almost nothing from pooling.
- **Declared event types often never occur.** Emitting nothing for zero-count
  streams, while keeping the declaration table, saves 1.45 MB on a 103-file
  mixed set.
- **Appended code dumps exceed 13 MB in some replays** and need chunking to
  parallelize.

## Entropy and match coding

- A general compressor's order-0 literal stage misses strong order-1 structure
  in transformed planes: on 37.3 MB the order-0 bound is 4.82 MB and the
  order-1 bound is 2.38 MB.
- An adaptive binary range coder with a previous-byte context loses to static
  models. Its probability floor costs about 0.2 bits per byte on near-constant
  planes, and 256×256 contexts do not converge within one replay's planes.
- Static previous-byte context trees with a two-bit presence coder beat the
  adaptive-bit version on size and speed.
- Building static contexts from 18 history intervals gives the same split
  decisions as 256 histograms.
- Masked coding beats direct match coding on controller packets with at least
  four controller planes in every one of 441 comparisons.
- Eight independent frequency counters remove the bottleneck of repeated-byte
  histogram updates. With early-exit sparse and run screening they encode 12%
  to 24% faster with identical output.
- Greedy parsing suits record planes and lazy parsing suits opaque frames.
  Greedy parsing everywhere makes some opaque attachments 17% to 27% larger.
- Dictionaries with one varying byte and a large alphabet cost more than they
  save; constant and small-alphabet dictionaries pay.

## Measured and rejected

- Sampling probe windows at a stride instead of contiguously: cache-hostile,
  and it ranks delta transforms wrongly.
- Deduplicating repeated suffixes across files: three blobs of 4 KB or more in
  419 files, none duplicated.
- Predicting state age as the previous value plus one: plane deltas already
  capture it.
- Unconditional numeric prediction on every field: worse than choosing fields
  individually. Second differences do not beat first differences.
- Higher-order and adaptive motion predictors, and replacing velocity
  prediction where velocity is recorded.
- Per-plane value coding, mask lookup tables, and interleaving masks in four
  sections.
- Byte-entropy and run-based presence coding: faster, and larger than the
  two-bit conditional model.
- Larger match hash tables: setup cost for negligible gain.
- Packed rANS tables, finer distance slots, and alternative match costs.
- Branchless and unrolled mask decoding: slower than testing the common
  all-zero and all-one pairs first.
- Two-axis input caches save bytes at an unmeasured speed cost.

The [generalization study](benchmarks/2026-11-12-generalization/README.md) lists
the encoder speed experiments rejected at level 3.

## Engineering

- **Concurrent writers hit a cliff on APFS.** Writing 344 MB across 140 files
  takes 0.87 s with 16 writers and 0.03 s with three writers, raw `open` and
  `write` calls, and directories created up front.
- **Decoding is a dependency graph.** Pools decode first; ordering them late
  serializes every file's assembly behind them. Dedicated writer threads behind
  a bounded queue keep decode workers off the file system.
- **Encoding admits files through a window** and frees each file's parse
  buffers when its last task retires, which bounds memory.
- **Profile-guided builds** encode 10% to 14% faster than `-O3` on 521 replays
  with identical output. The profile comes from real compression and hash
  verification runs; see the
  [build study](benchmarks/2026-11-11-profiled-release/README.md).
- **Scalar and SIMD builds write identical archives**, as do arm64 and x86-64
  builds.
- **A gain on one corpus overstates the gain on new sources.** Two builds that
  cleared a 10% encoding target on development data reached 5% and 8% on
  held-out files. Speed claims need a held-out set and enough repetitions to
  resolve launch-to-launch variation.

## Other tools

On matched single-replay inputs, slpz encodes faster and writes files more
than three times as large; slippc supports replay versions through 3.12.0;
Peppi 2.1.2 restores 28 of 120 sampled replays byte-for-byte. The
[matched comparison](benchmarks/2026-11-13-competitors/README.md) has the
figures.
