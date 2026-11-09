# slpz — Slippi-specific solid-archive compressor

Packs many `.slp` replays into one archive by exploiting the .slp event-stream
structure. On the 140-file benchmark sample (344 MB) it is **2.2× smaller
than the best general-purpose codec while beating it on both compress and
decompress speed**:

| | size | compress | decompress | peak RSS |
|---|---|---|---|---|
| `slpz -l 1` (fast, brotli) | 6.71% (14.9×) | 0.28 s | 0.08 s | ~830 MB |
| `slpz -l 3` (default) | **6.51%** (15.4×) | 0.48 s | 0.11 s | ~960 MB |
| `slpz -l 9` (high) | 6.13% (16.3×) | 1.35 s | 0.11 s | ~1.1 GB |
| xz -0 -T16 (best general) | 14.66% (6.8×) | 0.40 s | 0.18 s | 68 MB |

All slpz modes sit inside the "no slower than `zstd -9`" budget (`-l 1/-l 3`
under the multithreaded reading, `-l 9` under the single-thread reading).
Full survey: `../benchmark_solid_summary.txt`; experiment history:
`EXPERIMENTS.md`.

## Usage

```sh
make                          # needs homebrew libzstd + liblzma + brotli
./slpz c -o out.slpz [-l 3] [-T 16] [-v] file.slp... | @listfile
./slpz x out.slpz -d outdir            # extract (recreates relative paths)
./slpz t out.slpz                      # decode in memory, no writes
./slpz hash out.slpz                   # sha1 per file (shasum format), no writes
./slpz selftest                        # NEON kernels + codec roundtrips
make check                            # also tests the custom codec + archive integration
```

An opt-in custom codec is available on the `custom-codec` branch:

```sh
./slpz c --codec custom -l 3 -o out.slpz @listfile
./slpz x out.slpz -d outdir             # backend detected from the archive
```

`--codec legacy` is the unchanged default and writes v9. `--codec custom`
writes v15 using only our own payload compression: field-conditioned rANS,
shared presence masks, constant/sparse/repeat modes, and a lazy LZ parser with recent-distance and
literal prediction. It also enables lossless velocity and controller-input
predictors, including a position fallback for older replays without velocity
fields. Levels 1–2 use two search candidates; 3–5 use four with lazy parsing,
6–8 use eight, and 9+ use 24. Level 7 is the more compact middle-tier option. Unknown data retains a reversible raw fallback.

`--codec tuned` is a comparison backend: the same new semantic predictors,
with existing entropy codecs and block-sized LZMA dictionaries. It writes v15
with the legacy entropy backend flag. Both use the same sorted, compressed
field dictionaries and semantic predictors to keep codec comparisons fair.

The CLI still links zstd, LZMA, and Brotli to read old archives and provide the
legacy/tuned backends. The custom payload codec itself is independent of those
libraries; its standalone test and fuzzer build without them.

The custom backend is experimental. V15 adds exact controller/integer
permutations, action-dependent animation counters, attack-velocity prediction,
complete-field dictionaries, implicit constant indexes, and four-bit presence
coding. Dictionary loads, model scoring, and direct reconstruction into the
final replay buffer reduce processing work. See `benchmarks/2026-11-08-v15/`
for paired measurements against v14, validation, and unsuccessful experiments.
Across the same 522 replays at level 3 and 16 threads, v15 reduces archive
bytes by 10.1%, compression time by 11.8%, and decompression
time by 10.7% in the 31-run comparison.
All benchmark sets have been examined during development; gains vary by set.

Custom/tuned v15 archives require the new reader. V9 through v14 remain
readable; older readers reject v15 at the header. Legacy remains the v9 default.
`CUSTOM_FORMAT.md` documents each version and the encoder policies.

For legacy/tuned, `-l` selects the entropy stage (≤2: brotli q5; 3-8: LZMA fast/nice=128/8MB;
≥9: LZMA BT4/nice273 — all raced against zstd(level) per group, smaller wins).
Work is scheduled as per-(file × group) tasks, largest first, with file
planning pipelined into the pack queue and a 64-file admission window
bounding memory; blobs ≥ 1 MB (custom) or 4 MB (legacy/tuned) are chunked. Decode runs the task graph with
dependency-driven per-file assembly and dedicated writer threads. `-T` threads (default all cores; parallelism is per
file). `-v` prints compressed bytes per section and codec win counts.

## Pipeline

1. **Parse** the UBJSON wrapper (`{U\x03raw[$U#l` + u32 BE length; trailing
   `metadata` element kept verbatim). Event payload sizes come from the 0x35
   Event Payloads event, so any replay version parses generically.
2. **Split** the event stream per command byte; a 1-byte-per-event order
   stream preserves exact interleaving. Pre/post-frame events (0x37/0x38)
   are further split per (playerIndex, isFollower) and item updates (0x3B)
   per item typeID — the same 2-byte entity key at payload offsets 4-5.
3. **Cross-stream prediction**: Melee carries end-of-frame state into the
   next frame's pre-frame event, so each pre-frame record XORs its
   actionState/x/y/facing/percent spans against the entity's previous
   post-frame record — ~99% of those 18 bytes become zero (probe-gated per
   file; the XOR is self-inverse, so the decoder repeats it after
   reconstructing the post-frame stream).
4. **Fold RNG seeds**: the 0x37 (offset 6) and 0x3A (offset 4) seeds follow
   Melee's LCG (`s·214013+2531011`); store LCG step-count varints instead of
   4 raw bytes (99.995% fold rate; auto-disables per file if seeds don't
   follow the LCG). Frame-start events collapse to almost nothing.
5. **Dictionarize quantized fields**: 4- and 2-byte columns with ≤255
   distinct values (analog floats, actionStates…) become 1-byte indexes.
   Small side-event streams (game start, gecko lists, message splits) are
   **pooled across all files** — repeated content between replays of one
   setup dedups via LZ in a shared per-(cmd, recSize) pool. Gecko/PPC blobs
   get branch-target absolutization when a sample trial says it pays.
6. **Transpose** remaining record bytes into byte planes (NEON 16×16 tiles);
   per plane pick plain / subtract-delta / xor-delta by a windowed entropy
   probe (NEON delta + prefix-scan kernels).
7. **Entropy-code**: run-dominated planes (probe ≤0.3 bits/byte — mostly
   the zeroed prediction spans) go to zstd; entropy-rich planes go to LZMA
   raw (lc=3 pb=0) or brotli by tier, raced against whole-group zstd.
   Metadata/appended blobs try flat-zstd / flat-LZMA / PPC-transform /
   4-byte-record planes.

Anything that doesn't parse — truncated events, unknown commands, rawLen=0
in-progress replays, non-.slp bytes — falls back to raw zstd, so output is
**byte-identical for arbitrary input**. Verified on all 419 summit-11 replays
plus truncated/in-progress/garbage/empty edge cases after every change.

## Container format (v9)

```
u32 magic "SLPZ"  u8 version=9  u8 flags
u32 nPools, per pool: u8 cmd, u32 recSize, u32 totalCount, packed
u32 fileCount
per file: u32 nameLen, name, u64 origSize, u64 blobLen, blob
blob: u8 mode (0 = raw zstd fallback | 1 = split)
mode 1: 15-byte slp header verbatim, 0x35 payload verbatim,
        packed_blob(metadata suffix), packed_blob(unparsed tail),
        side(order stream), u8 nStreams, per stream:
          u8 cmd, u32 recSize, u32 count, u8 flags(bit0 grouped, bit1 seeds, bit2 xpred, bit3 pooled),
          [seeds: side(varint stream)]
          [grouped: side(selector), u8 nGroups, per group: u32 count, packed]
          [else: packed]
packed  = u8 nFields, per field {u8 width(2|4), u16 offset, u8 nVals-1,
          nVals×width}, selector byte per output plane (bits0-1 transform,
          bit2 cheap-bucket), u8 layout (0: u8 codec + frame(all) |
          1: frame zstd(cheap) + u8 codec + frame(rich));
          codec ids: 0=zstd 1=lzma-fast 2=lzma-normal 3=lzma-lite 4=brotli 5=lzma-bt4
packed_blob = u8 mode (0 flat-zstd | 1 4-byte-record packed | 2 flat-codec
              | 3 chunked: u8 codec, u8 ppc-xform, u32 nChunks, frames
              | 4 ppc-xform + flat-codec)
side    = u8 codec (0 zstd | lzma id) + frame
frame   = u32 compLen, u32 origLen, bytes
```

The container remains little-endian. Hot transforms use NEON on Apple Silicon and scalar fallbacks
elsewhere; filesystem, SHA-1, and memory-pressure handling support macOS and Linux. Benchmark numbers
are from a 16-core M-series machine, July 2026.
