# slpz — Slippi replay solid-archive compressor

Losslessly packs many `.slp` replays into one archive. It uses Slippi event
structure, cross-frame and controller prediction, field dictionaries, shared
presence masks, and a custom rANS/LZ payload codec. Extraction restores the
original file bytes, including metadata, malformed input, and unknown events.

The executable reads and writes **v1 custom archives**. Custom compression is
the default and only backend. No zstd, LZMA, or Brotli libraries are required.

## Build and use

A C++20 compiler is required. macOS uses the system CommonCrypto implementation
for SHA-1; Linux requires OpenSSL development headers and `libcrypto`.

```sh
make
./slpz c -o out.slpz -l 3 -T 16 file1.slp file2.slp
./slpz c -o out.slpz @listfile
./slpz x out.slpz -d outdir       # extract, recreating stored paths
./slpz t out.slpz                 # decode in memory without writing files
./slpz hash out.slpz              # SHA-1 per file in shasum format
./slpz selftest
make check
```

List files contain one input path per line. `-T` sets the thread count
(default: available hardware threads). `-v` prints compressed bytes by section.
`--codec custom` remains accepted for existing scripts; `legacy` and `tuned`
are rejected.

`-l` controls the custom LZ search effort:

| Levels | Search candidates |
|---|---:|
| 1–2 | 2 |
| 3–5 (default 3) | 4, with lazy parsing |
| 6–8 | 8 |
| 9+ | 24 |

## How it works

1. Parse the UBJSON wrapper and self-described Slippi event payload sizes.
   Preserve the header, metadata, unparsed tail, and event order.
2. Split events by command and player/follower or item group. Pool repeated
   side-event records across files.
3. Predict pre-frame values from the preceding post-frame state, fold RNG
   seeds into bounded LCG step counts, and predict positions and controller
   inputs. Older Slippi replay record sizes remain supported.
4. Replace low-cardinality fields with sorted dictionary indexes. Constant
   dictionaries need no index plane.
5. Transpose records into byte planes and choose plain, subtract-delta, or
   XOR-delta transforms. Encode semantic planes using constant/sparse/run
   packets, shared presence masks, and the custom entropy/LZ codec.
6. Store opaque input in custom frames. Large attachments are chunked into
   1 MiB tasks and may use a reversible PowerPC branch transform.

Encoding pipelines file planning into a shared task queue; decoding assembles
files as their dependencies complete. NEON accelerates transforms on Apple
Silicon, with scalar paths on other architectures. Float predictors require
IEEE binary32 round-to-nearest; fast-math builds are unsupported.

See [CUSTOM_FORMAT.md](CUSTOM_FORMAT.md) for the format and predictor details.

## Compatibility

V1 names the same custom payload format previously labeled v15. New archives
use version byte `1`; the reader also accepts byte `15` as an alias for the
identical format. Existing v15 archives need no conversion. Older Slippi
replay record sizes are still supported inside either header.

Earlier archive formats and the former `tuned` backend are rejected. The
previous reader does not recognize the new v1 header; current readers accept
both headers through the same decoding path.

The last reader supporting v9–v15 and all three backends is commit
[`b66dd55`](https://github.com/Eppie/slpz/tree/b66dd55). To build it separately:

```sh
git worktree add --detach ../slpz-compat b66dd55
make -C ../slpz-compat
```

That checkout includes the old migration tools and requires zstd, LZMA, and
Brotli development libraries. The historical `main` branch contains an earlier
legacy implementation; the latest code is on `custom-codec`.

## Validation and benchmarks

`make check` covers custom packets, dictionary framing, scalar predictor
oracles, exact extraction, unsupported-format rejection, and a committed
original-v15 fixture made by the pre-cleanup encoder. Fuzzing instructions are in
[fuzz/README.md](fuzz/README.md).

The `benchmarks/` directory and [EXPERIMENTS.md](EXPERIMENTS.md) record historical
experiments. Their backend selections and version comparisons describe the
commits used for those studies. For the latest recorded speed study, see
`benchmarks/2026-11-09-speed/`; all benchmark sets were examined during
development, so those results are not independent held-out estimates.
