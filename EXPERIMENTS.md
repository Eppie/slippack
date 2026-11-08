# slpz experiment log

Benchmark: 140-file sample (344,239,004 B) from summit-11, 16 threads,
min of 2 runs, peak RSS via /usr/bin/time -l. Ratios = archive/input.
Correctness gate for every row: 419/419 byte-identical roundtrip + edge cases
(truncated, rawLen=0, garbage, empty, tiny).

## v2 — split by cmd + player, byte-plane transpose, whole-stream sub-delta trial
| level | ratio | comp | dec |
|---|---|---|---|
| 3 | 13.44% | 0.15s | 0.34s |
| 9 | 12.65% | 0.36s | 0.33s |
| 19 | 11.89% | 5.39s | 0.34s |

## v3 — per-plane {plain,sub,xor} probe, NEON transpose/delta/scan kernels
Strided probe sampling was cache-hostile and mis-ranked deltas (non-adjacent
neighbours): 13.51% @ 0.24s at l3 — ratio AND speed regression. Lesson:
sample windows must be contiguous.

## v4 — windowed probe + RNG seed folding + PPC blob handling
- Probe: 3× contiguous 4KB windows, fused 3-histogram pass, xlog2x LUT,
  constant-plane fast path.
- Seed folding: 0x37 seed (offset 6) follows Melee's LCG (s*214013+2531011);
  store LCG step-count varint instead of 4 raw bytes. 99.995% fold rate
  (2.71M/137 on the sample); auto-disables if probes miss.
- Large post-raw blobs (appended PPC/gecko dumps, up to 13.8 MB in summit-11)
  packed as 4-byte records IF that beats flat zstd (trial both — flat often
  wins because LZ matches across instructions dominate).

| level | ratio | comp | dec | comp RSS | dec RSS |
|---|---|---|---|---|---|
| 1 | 11.02% | 0.14s | 0.31s | 584MB | 556MB |
| 3 | 10.75% | 0.14s | 0.31s | 601MB | 550MB |
| 6 | 10.18% | 0.23s | 0.31s | 641MB | 551MB |
| 9 | 9.98% | 0.34s | 0.31s | 771MB | 553MB |
| 12 | 9.92% | 0.58s | 0.31s | 898MB | 550MB |
| 19 | 9.28% | 6.7s | 0.31s | 1218MB | 544MB |

Reference: xz -0 -T16 = 14.66% @ 0.40s; zstd -9 (plain, MT) budget = 0.80s.
Section breakdown at l3: 0x37 19.2MB, 0x38 13.1MB, metadata 2.5MB,
seeds 0.95MB, everything else < 1MB.

## v5 experiments — entropy stage beyond zstd
Python bounds on v4's transformed planes (8 files, 37.3 MB):
o0 = 4.82 MB, o1 = 2.38 MB, zstd3 = 4.06 MB, zstd19 = 3.52 MB → strong
order-1 structure that zstd's order-0 literal stage cannot capture.

**Adaptive o1 range coder (negative result).** LZMA-style binary range coder,
nibble trees, (prev byte) context. Lost every group trial: 44.5 MB vs 36.6 MB
(zstd -3). Two causes: (a) ~0.2 bit/byte probability floor loses badly on
near-constant/run planes where zstd matches encode ~0; (b) context dilution —
256×256 contexts cannot converge within one file's ~30 KB plane. Per-plane
codec bucketing fixed (a) but only won 57/490 groups. Cross-file pooling
(plane-major concat) was simulated in python: pooled o1 bound 2.92 MB vs
zstd19 3.41 MB — distribution drift between files eats the win. Not worth an
architecture rework; coder removed.

**Field dictionarization (kept).** Melee analog fields are quantized floats:
many 4-byte columns take ≤255 distinct values. Replacing them with 1-byte
dictionary indexes (greedy aligned scan, exact distinct count via open
addressing, dict stored raw) recovers cross-plane structure that both
byte-planes and per-plane o1 are blind to, and shrinks the entropy-stage
input 37 → 21 MB. Planes: zstd3 4.06 → 3.76 MB incl. dict overhead.

**LZMA raw frames as plane codec (kept).** LZMA literals model
(prev-byte-high-bits × position) context — the o1 structure — plus real match
finding, mature and fast. On dict-transformed planes (single thread):
zstd3 3.58 MB @ 832 MB/s; lzma fast/HC4/nice32/lc3/pb0 3.22 MB @ 76 MB/s
(decode 222 MB/s); lzma normal/BT4 3.04 MB @ 11 MB/s. Adopted: per-group
trial zstd(level) vs lzma (fast for -l ≤8, normal/HC4/nice64/8MB for -l ≥9);
blobs also try flat-lzma (wins on the PPC dumps).

## v5 — dictionarization + LZMA planes (format v4)
| level | ratio | comp | dec | comp RSS | dec RSS |
|---|---|---|---|---|---|
| 3 (fast) | 8.80% | 0.35s | 0.37s | 797MB | 547MB |
| 9 (high) | 8.32% | 2.38s | 0.38s | 1281MB | 550MB |

Full 419-file set: 8.63% (was 13.27% in v2). Wall-time pole is the single
largest file (per-file parallelism); pack_blob skips lzma inside its packed
candidate to halve the pole cost (0.61 → 0.35s).

Ideas not pursued: BT4 at high level (5-10x slower for ~1% ratio), per-group
parallelism to smooth the pole, cross-file solid streams (measured ≈ 0% for
big streams), stream/chunk I/O to cut RSS.

## v6 — spend the budget headroom (format v5)
LZMA param sweep on transformed planes found fast/nice=128/8MB = 2% smaller
than fast/nice=32/1MB at identical speed. Plus: 0x3A frame-start seed folding
(the event is just frame# + seed → 366 KB → 10 KB), 0x3B item grouping by
typeID (2-byte entity key generalizes the player split; ≤255 groups else
ungrouped), and zstd-vs-lzma trials for the seed/order/selector side streams.

| level | ratio | comp | dec | comp RSS |
|---|---|---|---|---|
| 3 (fast) | 8.59% | 0.52s | 0.37s | 1117MB |
| 9 (high) | 8.22% | 2.31s | 0.37s | 1298MB |

Full 419-file set: 8.43%. Attribution at l3 vs v5: params −280KB,
0x3A folding −355KB, 0x3B grouping −33KB, seed-stream lzma −26KB.
Clear diminishing returns; remaining mass is 0x37 (14.3MB) + 0x38 (11.7MB)
frame data where LZMA-normal only finds ~4% more (l9), and 1.9MB of PPC
dumps. Next big step would need real field-level float prediction.

## v7 — speed round (format v6; same ratios at l3/l9)
Goal: faster encode/decode without giving up compression. Findings:
- **Brotli on transformed planes** (never tested pre-transform!): q5 = +3.5%
  size vs LZMA-fast but 2.6x encode / 3.5x decode speed → adopted as the
  `-l <=2` tier codec (replaces lzma-lite; old archives still decode).
- **Pipelined task scheduler**: per-(file x group) work items with a
  largest-first heap; workers plan files whenever no pack work is ready (no
  phase barrier). Blobs >= 4MB are chunked (mode 3, 4MB frames) so even the
  13.8MB gecko dump parallelizes. Decode mirrors it: scan (spans only) ->
  parallel unpack -> parallel assemble.
- **APFS write concurrency cliff**: 344MB across 140 files took 0.87s with
  16 concurrent writers (7.4s sys!) vs 0.23s serial; 3 writers + raw
  open/write syscalls + pre-created dirs = 0.03s. Also: std::_Exit(0) skips
  ~0.2s of exit-time destructor teardown on multi-GB buffers.

| tier | ratio | comp | dec | vs v6 |
|---|---|---|---|---|
| 1 (brotli) | 8.87% | 0.32s | 0.09s | comp -24%, dec -75% |
| 3 | 8.59% | 0.54s | 0.14s | comp flat (CPU-bound), dec -62% |
| 9 | 8.22% | 1.30s | 0.14s | comp -44%, dec -62% |

Decode now beats xz -0 (0.18s) outright; xz -0 is fully dominated again.
Cost: peak RSS ~1.5GB compress / ~0.9GB decompress (phases retain all
files' buffers; freed eagerly where possible). l3 compress is now genuinely
CPU-bound on LZMA-fast throughput (~0.30s of perfectly-packed work + tail).

## v8 — cross-stream prediction (format v7)
Novel-idea round. Winner: **post-frame(N) ≈ pre-frame(N+1)** — Melee carries
end-of-frame state into the next frame's pre-frame event for the same entity.
Measured exact byte equality per field: x 98.7%, y 99.5%, facing 99.4%,
percent 99.96%, actionState 97.5%. Both spans are contiguous (pre 10..24 ↔
post 7..21, pre 59..63 ↔ post 21..25), so the transform is two XOR spans per
record against the entity's previous post-frame (probe-gated per file,
self-inverse, rollback-safe). Python pipeline test: **-59.3% on 0x37**.

| tier | ratio | comp | dec | vs v7 ratio |
|---|---|---|---|---|
| 1 (brotli) | 6.89% (14.5x) | 0.31s | 0.12s | -22% |
| 3 | 6.68% (15.0x) | 0.50s | 0.12s | -22% |
| 9 | 6.38% (15.7x) | 1.13s | 0.13s | -22% |

Full 419-file set: 8.43% -> **6.48%**. Compress got faster too (LZMA flies
through zeroed planes). 2.2x smaller than the best general codec (xz -0).

Negative results, same round: cross-file suffix dedup (only 3 blobs >=4KB in
all 419 files, zero duplicates); 0x38 stateAge prev+1.0 prediction (+0.2% —
plane deltas already capture it); 0x38 second-order float position
prediction (-1.4% — mantissa rounding noise). Post-frame is now genuine
innovation (inputs/dynamics), not echo — further ratio needs modeling, not
plumbing.

## v9 — plane-class split, wider dicts, PPC transform, cross-replay pools,
## bounded memory (format v8)
Everything-at-once round; every change gated by the full roundtrip suite.
- **Plane-class codec split**: probe-classified cheap planes (est <= 0.30
  bits/byte — runs, constants, the zeroed prediction spans) go to zstd; only
  entropy-rich planes hit LZMA/brotli. Layout byte per group; whole-zstd
  fallback trial kept.
- **2-byte field dictionarization** (u16 actionStates etc.) via a second
  greedy pass; dict fields now carry a width byte.
- **PPC branch-target absolutization** (kkrunchy-style, adapted to PowerPC
  b/bl/bc): relative displacements -> index-based targets, modular arithmetic
  so exactly invertible. Blob trial candidate + sample-gated flag on chunked
  blobs.
- **Cross-replay pooled streams**: small side-event streams (0x36 game
  start, 0x3D gecko lists, 0x10 message splits, 0x39/0x3C) from ALL files
  concatenate into per-(cmd, recSize) pools packed once — repeated content
  between replays of one setup dedups via LZ. Archive gains a pool section.
- **Bounded memory**: compress admits files through a 64-file window and
  frees each file's parse buffers when its last task retires; decode runs a
  dependency graph (pools first — sorting them late serialized every file's
  assembly behind them) with inline assemble on the last-task thread and
  dedicated writer threads (bounded queue) so workers never block on APFS.

| tier | ratio | comp | dec | comp RSS | dec RSS |
|---|---|---|---|---|---|
| 1 (brotli) | 6.75% (14.8x) | 0.28s | 0.07s | 872MB | 624MB |
| 3 | 6.55% (15.3x) | 0.46s | 0.11s | 978MB | 632MB |
| 9 | 6.21% (16.1x) | 1.06s | 0.11s | 1141MB | 657MB |

Full 419-file set: 6.34% (l3), 6.04% (l9). vs v8: ratio -2%, compress -8%,
decode -15%, compress RSS -35%, decode RSS -30%. All four objectives
improved at once.

## v10 — BT4 tier + group-ordered seeds (format v9)
- **kLzmaMax** (BT4/nice=273/8MB) as the -l >=9 codec: -6.4% measured on
  frame planes at ~5 MB/s/thread; affordable because the plane split routes
  only entropy-rich planes there. l9 wall 1.06 -> 1.35s, still under the
  single-thread zstd -9 budget (2.68s).
- **Group-ordered seed streams**: LCG step counts still computed in global
  RNG order but emitted per entity (the interleaved [K,0,K,0...] pattern
  deinterleaves to a periodic per-player sequence; measured -21% on the
  varint stream). Decoder pre-parses per-group cursor offsets — no format
  overhead.

| tier | ratio | comp | dec | full 419-set |
|---|---|---|---|---|
| 1 | 6.71% (14.9x) | 0.28s | 0.08s | 6.45% |
| 3 | 6.51% (15.4x) | 0.48s | 0.11s | 6.30% |
| 9 | 6.13% (16.3x) | 1.35s | 0.11s | **5.96%** |

Diminishing returns are now steep: this round bought 0.6-1.3% relative.
Remaining mass is x/y position mantissas (true motion innovation) and the
input streams — sequence-model territory.

## 2026-11-08 — custom backend (container v10, opt-in)

Implemented `--codec custom` with field-conditioned static rANS models,
explicit constant/sparse/repeat packets, and a bounded lazy LZ parser.
Three-byte matches, four recent-distance codes, bit-packed length/distance
extras, and optional last-match literal prediction improve the initial generic
custom prototype. Semantic model and literal prediction choices account for
actual serialized size. Velocity and controller-input predictors remain
bit-exact, including exceptional float values.

`--codec tuned` provides the same new predictors with the existing entropy
codecs and block-sized LZMA dictionaries. `--codec legacy` remains the default,
writing the original v9 format. New binaries read both v9 and v10; the old
binary cleanly rejects v10. Custom payload coding uses no external compressor,
although the CLI retains the libraries for legacy compatibility.

Five-run medians on the M4 Max, 16 threads, 242 replays plus one provenance
file (641,897,168 input bytes) across Summit 11, GOML 2019, and Bair 2026:

- Relative to tuned LZMA, custom is 7.8–11.1% larger, takes 6.1–9.8% more encode
  time, and 20.5–24.9% less decode time.
- Relative to the earlier generic custom LZ+rANS, archive sizes improve by
  10.2% on Summit and 5.4% on the mixed sample.
- All sample hashes match. Standalone tests exercise every packet mode and
  force the semantic probability model to win on distinct field distributions.
  ASan/UBSan checks and a 31-second, 79,333-execution fuzz smoke run passed.

The velocity predictor disproves the earlier broad claim that post-frame
compression necessarily requires sequence modeling: recorded velocities help
where position-only extrapolation did little. The custom backend is a useful
decode-speed tradeoff, but has not matched tuned LZMA's compression ratio.
Full results, input lists, and reproduction commands are in
`benchmarks/2026-11-08/`; the format is documented in `CUSTOM_FORMAT.md`.

## 2026-11-08 — shared-mask custom backend (container v11)

The custom backend now beats the tuned **level-3** LZMA baseline on archive
size, compression time, and decompression time across all three measured sets.
It retains the same upstream parsing, dictionaries, transforms, and motion/
input predictors. The main new information model separates the locations of
exceptions from their values, sharing presence information across related
float planes. Static previous-byte context trees and a two-bit presence rANS
coder avoid the cost of the initial adaptive-bit prototype.

The ratio improvement alone was insufficient: match-finder pruning, word
comparisons, bounded skipping, fewer coding trials, and ARM NEON exception
packing/scattering were needed to meet the encode-speed target. Scalar and
native archives are byte-identical. No external payload compressor is called.

Nine warmed repetitions, M4 Max, 16 threads, relative to tuned LZMA at level 3:

| Dataset | Smaller | Less encode time | Less decode time |
|---|---:|---:|---:|
| Summit sample, 140 files | 2.66% | 7.01% | 17.19% |
| GOML + Bair mixed, 103 files | 0.53% | 4.41% | 11.53% |
| Additional Summit, 279 files | 3.74% | 4.04% | 17.69% |

All 522 files (521 replays plus a provenance TSV), totaling 1,309,561,719
source bytes, passed decoded hash checks. Validation also includes 3,430
standalone checks under ASan/UBSan and with SIMD disabled, all-backend archive
roundtrips at levels 1/3/9, v9/v10 compatibility, and 80,004 sanitizer-backed
fuzz executions in 61 seconds without findings. The additional 279 files
were initially held out, then used to diagnose and validate speed fixes.

This is not a win over every LZMA preset. On mixed at level 9, custom is 4.78%
larger than tuned LZMA, while LZMA takes about 4.6 times as long to encode.
The default remains legacy; `--codec custom` opts into v11. The new reader
retains v9 and v10 support, and older readers reject v11 cleanly.

Full timings, hashes, file lists, and validation evidence are in
`benchmarks/2026-11-08-v11/`. Intermediate results and promising-but-rejected
source experiments are retained in `benchmarks/2026-11-08-model-study/`.
The adaptive model, larger presence symbols, distance deltas, and cross-plane
value prediction remain possible research directions rather than universal
negative conclusions.
