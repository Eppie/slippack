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

## 2026-11-08 — sorted, compressed field dictionaries (container v12)

Dictionary metadata was still raw: 2.35 MB of the mixed v11 archive. Sorting
values by original big-endian bytes and remapping indexes improves both that
metadata and temporal index deltas. The encoder now selects shuffled/plain,
byte-XOR, byte-subtract, or whole-field differences and stores a compressed
value frame only when smaller. Descriptors remain scannable. Tuned receives
the identical preprocessing and new dictionary representation.

Fixed-width expansion, index validation outside the gather loop, constant
fills, and NEON lookups for alphabets up to 64 entries reduce reconstruction
cost. Word-wise constant checks avoid redundant histogram work in encoding.
The rANS/LZ/mask payload formats themselves are unchanged.

Nine warmed level-3 repetitions, M4 Max, 16 threads, versus custom v11:

| Dataset | Smaller | Less encode time | Less decode time |
|---|---:|---:|---:|
| Summit sample, 140 files | 8.75% | 0.61% | 7.60% |
| GOML + Bair mixed, 103 files | 9.21% | -3.92% | 1.58% |
| Additional Summit, 279 files | 9.90% | 1.98% | 6.19% |

Total archive bytes fall 9.43%, with broadly unchanged compression time.
The mixed encoding regression is retained in the report. Against the equally
improved tuned level-3 baseline, custom is 1.84–2.93% smaller on the Summit
sets and 0.70% larger on mixed, while encoding/decoding are faster on all
three measured sets. At level 9 on mixed, custom is 6.84% larger than tuned, which takes 4.62×
as long to encode. No universal size/speed win is claimed.

All 522 source hashes match. Checks cover every new dictionary transform,
lookup boundaries, malformed frames, field overlap, v9/v10/v11 compatibility,
scalar/native byte identity, and a build with fatal external-encoder tripwires.
ASan/UBSan passed; the dictionary fuzzer completed 180,224 executions in 61
seconds without findings. Packed rANS tables, finer distance slots, alternate
match costs, and a constant-field discovery pass were measured and dropped
where they did not help. Selected patches are retained for another look.

See `benchmarks/2026-11-08-v12/` for exact samples, qualifications, input lists,
validation, and reproduction. Custom and tuned now write v12; legacy remains
the default v9 writer. Readers retain all prior versions.

## 2026-11-08 — numeric residuals and RNG jumps (container v13)

The older 51-byte post-frame record was the main remaining opportunity:
129/140 Summit sample files and 51/102 mixed replays lack velocity fields,
so the existing motion predictor never ran on them. V13 uses exact previous
position bits as the fallback and encodes signed modular bit differences with
zigzag mapping. Newer records retain velocity prediction, using the same
signed-residual mapping. Selected numeric fields (offsets 21/25/60/64, when
present) also use first differences. Arbitrary float bits remain reversible.

Two independent speed changes replace linear RNG walks with a low-bit phase
lookup plus verified affine jumps, and reduce repeated mask-neighbor loads.
Direction-specialized predictors, sparse clearing of entity lookup state,
fast stride-one context indexing, and skipping unused semantic labels remove
more overhead. The middle search tier now uses four candidates (levels 3–5);
levels 6–8 retain eight and level 9+ retains 24. The new predictors leave room
to reduce search work while keeping the archive smaller than v12.

Nine warmed repetitions, level 3, M4 Max, 16 threads, versus custom v12:

| Dataset | Smaller | Less encode time | Less decode time |
|---|---:|---:|---:|
| Summit sample, 140 files | 6.63% | 6.70% | 5.03% |
| GOML + Bair mixed, 103 files | 4.19% | 3.79% | 5.24% |
| Additional Summit, 279 files | 7.23% | 7.95% | 13.35% |

Every measured level-3 point improves all three medians. Desktop load was
higher than during the previous report: comparisons use v12 rerun alongside
v13, not historical absolute timings. No compilation or fuzzing ran alongside
these final timing rounds. All sets have been used during this research;
none is presented as an unseen test set.

Unconditional numeric prediction was worse; field-by-field trials retained
only consistent improvements. Second differences did not beat first ones in
that sweep. Larger match hash tables added setup cost for negligible gains.
Byte-entropy and run-based presence encodings were faster in an offline probe
but generally larger than the existing two-bit conditional model. Those
results are implementation-specific, not proof that the ideas cannot help.

See `benchmarks/2026-11-08-v13/` for exact samples, additional presets, the
Pareto plots, validation, and reproduction. V13 reads v9–v12; older readers
reject v13. Legacy remains the v9 default. No external payload compressor is
used by custom encoding.

## 2026-11-08 — v13 follow-up: empty streams and redundant modeling

The parser created a stream for every declared event type, even when no such
event occurred. Those zero-count streams emitted selectors and plane metadata.
Wide unused declarations contributed 1,450,260 bytes on mixed. Dropping the
unused streams while preserving the original declaration table fixes this for
all backends, without a format change. Actual zero-width events are retained.

The encoder also tried direct LZ before masked encoding for controller packets;
masked encoding won all 193 mixed and 248 sample large-packet comparisons with
at least four controller planes. Levels below 9 now skip the losing trial.
Level 9 retains the comparison. Static context construction uses 18 history
intervals rather than 256 histograms, with identical split decisions. Nearest
frequency rounding saves another 8–9 KB on sample/mixed. Mask decoding checks
the common all-zero/all-one pairs first; branchless and unrolled alternatives
were slower in the exploratory runs.

Nine warmed level-3 repetitions, 16 threads, relative to `c03652c`:

| Dataset | Smaller | Less encode time | Less decode time |
|---|---:|---:|---:|
| Summit sample, 140 files | 1.86% | 14.18% | 2.86% |
| Mixed, 103 files | 9.37% | 12.66% | 3.78% |
| Additional Summit, 279 files | 1.94% | 16.16% | 9.40% |

Combined bytes fall from 65,307,402 to 62,898,815. Single-thread mixed confirms
12.79% less encode time and 10.21% less decode time. Some other preset medians
regress slightly: level-1 decode by 1.44%, level-9 encode by 0.29%.

The equally updated tuned backend now loses narrowly on mixed at level 9:
custom 13,996,712 bytes versus tuned 14,021,253, with encode/decode medians
0.34754/0.09410 seconds versus 1.77623/0.11542. That is 0.175% smaller,
80.43% less encode time, and 18.47% less decode time. This closes the previously
reported gap on that set, not every possible input or compressor.

See `benchmarks/2026-11-08-v13-round2/` for timing samples, rejected trials,
validation, and reproduction. Output remains v13; old v13 readers accept it.

## 2026-11-08 — v14: a further 10% in all three aggregate categories

Against the preceding custom codec (`5c1659d`, code at `22e61aa`), level 3,
16 threads, nine warmed randomized repetitions:

| Dataset | Smaller | Less encode time | Less decode time |
|---|---:|---:|---:|
| Summit sample (140) | 10.66% | 9.71% | 15.25% |
| Mixed (103) | 6.38% | 3.89% | 13.69% |
| Additional Summit (279) | 11.92% | 14.98% | 13.55% |
| Combined | **10.32%** | **11.45%** | **13.99%** |

The aggregate sums archive sizes and dataset timing medians. Combined bytes
fall from 62,898,815 to 56,405,346. This meets the requested 10% improvement in
each category across the corpus, not independently on every dataset. At one
thread on mixed, encode improves 8.67% and decode is essentially flat (0.36%).

Old post-frame records benefit from quadratic position extrapolation; records
with explicit velocity keep that predictor. Raw-controller-indexed last-value
tables now use numeric bit residuals. Dictionaries with one varying byte and
large alphabets were a productive revisit of the constant-field idea: keep
useful constant/small-alphabet dictionaries, skip the low-value large ones.
Direct dictionary construction, shorter transform probes, rectangular vector
transpose tiles, and avoiding unnecessary final allocator/thread work supply
the speed gains. The entropy coder is unchanged and remains fully custom.

Higher-order and adaptive motion predictors, replacing modern velocity
prediction, per-plane value coding, mask lookup tables, four-section mask
interleaving, and several copy/lookup optimizations did not win. Two-axis input
caches saved additional bytes but were not integrated or speed-validated.

New custom/tuned output is v14; v9–v13 remain readable. Tests, 522 file hashes,
scalar/native equivalence, legacy compatibility, external-encoder tripwires,
and short ASan/UBSan predictor/archive fuzz runs all pass. Full evidence,
limits, rejected trials, and reproduction are in `benchmarks/2026-11-08-v14/`.

## 2026-11-10: single-replay comparison with slpz, slippc, and Peppi

Slippack v1 was compared with [Alexander Harrison's slpz](https://github.com/AlexanderHarrison/slpz/tree/9f4d952f444037edbbc49af11e5e963ecd4d399f),
[pcrain's slippc](https://github.com/pcrain/slippc/tree/8d79a665392e491edd62150687a956e45a7a0fe7),
and [Peppi 2.1.2](https://docs.rs/peppi/2.1.2/peppi/).
Every compressed file contains exactly one replay. There is no sharing between
replays, including in the parallel runs.

The sample contains 120 distinct replays, totaling 313,007,679 bytes: 40 from each
of the existing sample, mixed, and historically named holdout lists. Selection
sorts filenames by SHA-256 and takes the first 40, before any codec is run. One
non-replay inventory file in the original mixed list is excluded. Versions are
2.0.1 (92 files), 3.9.0 (6), and 3.17.0 (22). These source sets were used in earlier
development; they are not independent unseen evaluation data.

On an Apple M4 Max with 128 GiB RAM, each setting receives a warmup and five
measured runs in reproducibly shuffled order. Tables use medians. Inputs are
preloaded into RAM. Timings include codec setup, transformations, allocations,
and full output construction, and exclude file I/O, process startup, final
buffer disposal, and verification. Every output is compared byte-for-byte with
its input after each run. slippc's usual verification is likewise outside the
encoding timer. Peppi decoding includes reconstruction of the original `.slp`,
not just loading Arrow data into a parsed game.

One worker measures sequential single-replay operations. Eight workers process
independent files; each codec invocation uses one worker. slpz reuses its Zstd
context per worker, as its directory CLI does. Slippack calls the public archive
API with one member and the original basename. Complete file sizes, including
headers, filenames, and metadata, are counted. CLI/library compressed-size spot
checks agree for Slippack and slpz.

The core is Slippack commit `9bffdb5`; slpz is commit `9f4d952`, slippc is
`8d79a66`, and Peppi is the published 2.1.2 crate. Builds use Apple Clang 17 at
`-O3` (C++20 for Slippack, C++17 for slippc), rustc 1.88.0's release profile,
Zstd 1.5.6, and system liblzma 5.8.4. The benchmark pins Rust dependencies in
Cargo.lock. No upstream codec code is patched. Peppi's outer Zstd decoder sizes
its buffer from the frame's declared content size. Executable SHA-256 hashes
are recorded per comparison set in the raw report. Desktop scheduling and
allocator/cache state still affect these measurements.

### Coverage and matched sets

Slippack and slpz restored all 120 sampled files exactly. slippc restored all
98 files within its advertised support through replay version 3.12.0; the 22
newer files were not submitted to its fixed-layout transforms.

All 120 files could be written to Peppi format, but only 28 could be restored
byte-for-byte by Peppi 2.1.2 in this test. Seventy-four reconstructions panicked
at its Gecko-code size assertion (`io/slippi/ser.rs:394`); another 18 returned
different bytes. All three Peppi storage variants had the same failures. This
is a result for this corpus and this version, not a claim that Peppi generally
loses replay data. Peppi also provides an Arrow representation for analysis,
which the other formats do not.

Every table compares tools on exactly the same files. The three sets have
different compositions, so do not compare absolute throughput across sets.
The uncompressed Peppi row is a format-conversion reference, not a compression
competitor. Internal Zstd compresses Arrow buffers; the outer variants instead
compress the entire uncompressed `.slpp` file with Zstd at the indicated level.

### All 120 replays: Slippack versus slpz

Raw size: 313,007,679 bytes. Sizes are percentages of the original input
(smaller is better); speed is decimal MB/s of original replay bytes (larger is
better). All settings are listed to show the size/speed tradeoff.

**1 worker**

| Codec | Packed size | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|
| Slippack level 1 | 4.18% | 222.8 | 839.0 |
| Slippack level 3 (default) | 4.13% | 215.6 | 858.9 |
| Slippack level 9 | 4.11% | 158.4 | 870.7 |
| slpz --fast (default) | 13.45% | 531.2 | 1027.4 |
| slpz --small | 12.20% | 109.2 | 1164.8 |

**8 workers**

| Codec | Packed size | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|
| Slippack level 1 | 4.18% | 1700.0 | 6489.9 |
| Slippack level 3 (default) | 4.13% | 1579.7 | 6498.7 |
| Slippack level 9 | 4.11% | 1177.1 | 6653.2 |
| slpz --fast (default) | 13.45% | 3915.3 | 7246.8 |
| slpz --small | 12.20% | 774.3 | 7938.3 |

### 98 replays within slippc’s supported version range

Raw size: 242,937,060 bytes. Sizes are percentages of the original input
(smaller is better); speed is decimal MB/s of original replay bytes (larger is
better). All settings are listed to show the size/speed tradeoff.

**1 worker**

| Codec | Packed size | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|
| Slippack level 1 | 4.06% | 215.8 | 835.6 |
| Slippack level 3 (default) | 4.03% | 220.9 | 894.5 |
| Slippack level 9 | 4.01% | 160.2 | 890.7 |
| slpz --fast (default) | 13.38% | 528.1 | 1037.7 |
| slpz --small | 12.04% | 109.5 | 1087.7 |
| slippc (LZMA level 6) | 5.33% | 6.6 | 335.3 |

**8 workers**

| Codec | Packed size | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|
| Slippack level 1 | 4.06% | 1591.5 | 6439.4 |
| Slippack level 3 (default) | 4.03% | 1578.9 | 6210.6 |
| Slippack level 9 | 4.01% | 1147.9 | 6609.0 |
| slpz --fast (default) | 13.38% | 3842.4 | 7055.4 |
| slpz --small | 12.04% | 779.8 | 7800.2 |
| slippc (LZMA level 6) | 5.33% | 48.9 | 2196.1 |

### 28 replays that round-trip exactly through Peppi

Raw size: 74,297,116 bytes. Sizes are percentages of the original input
(smaller is better); speed is decimal MB/s of original replay bytes (larger is
better). All settings are listed to show the size/speed tradeoff.

**1 worker**

| Codec | Packed size | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|
| Slippack level 1 | 4.47% | 239.9 | 859.5 |
| Slippack level 3 (default) | 4.41% | 216.1 | 803.9 |
| Slippack level 9 | 4.39% | 156.3 | 852.7 |
| slpz --fast (default) | 13.72% | 536.4 | 1012.7 |
| slpz --small | 12.78% | 108.2 | 1108.5 |
| Peppi, no compression | 91.67% | 659.9 | 1919.5 |
| Peppi, internal Zstd | 17.69% | 257.3 | 501.8 |
| Peppi + whole-file Zstd 3 | 11.36% | 382.6 | 1072.2 |
| Peppi + whole-file Zstd 12 | 10.07% | 126.5 | 1108.8 |

**8 workers**

| Codec | Packed size | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|
| Slippack level 1 | 4.47% | 1438.0 | 5285.7 |
| Slippack level 3 (default) | 4.41% | 1414.6 | 5338.4 |
| Slippack level 9 | 4.39% | 995.2 | 5302.7 |
| slpz --fast (default) | 13.72% | 3488.6 | 5980.2 |
| slpz --small | 12.78% | 697.6 | 6376.3 |
| Peppi, no compression | 91.67% | 3061.9 | 8315.6 |
| Peppi, internal Zstd | 17.69% | 1226.9 | 1501.6 |
| Peppi + whole-file Zstd 3 | 11.36% | 1964.0 | 5183.9 |
| Peppi + whole-file Zstd 12 | 10.07% | 777.2 | 5087.4 |

### Reproduction

The benchmark adds optional Rust/Zstd/LZMA dependencies only to its runners;
Slippack itself retains its self-contained codec.

```sh
bash benchmarks/2026-11-10-single-file/build.sh
python3 benchmarks/2026-11-10-single-file/benchmark.py \
  --study /path/to/existing-study --repeats 5
```

The study directory must contain the existing `summit/` and `mixed/` sources
referenced by the tracked lists under `benchmarks/2026-11-09-speed/`. No replay
payloads are included in this repository. The JSON report records every selected
filename, source SHA-256, compressed size, coverage failure, and timing sample:
[raw results](benchmarks/2026-11-10-single-file/results.json).
