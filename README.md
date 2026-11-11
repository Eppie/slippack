# Slippack — Slippi replay compressor

Losslessly packs many `.slp` replays into one archive. It uses Slippi event
structure, cross-frame and controller prediction, field dictionaries, shared
presence masks, and a custom rANS/LZ payload codec. Extraction restores the
original file bytes, including metadata, malformed input, and unknown events.

The executable reads and writes **v1 custom archives**. Custom compression is
the default and only backend. No zstd, LZMA, or Brotli libraries are required.

Slippack was previously named `slpz`. It is a separate project from
[Alexander Harrison's slpz](https://github.com/AlexanderHarrison/slpz), whose
single-replay `.slpz` files use a different format. Use `.slippack` for new
Slippack archives. Our existing archives still decode without conversion;
the v1 wire signature remains `SLPZ` and the format version remains 1.

## Build and use

A C++20 compiler is required. macOS uses the system CommonCrypto implementation
for SHA-1; Linux requires OpenSSL development headers and `libcrypto`.

```sh
make
./slippack c -o out.slippack -l 3 -T 16 file1.slp file2.slp
./slippack c -o out.slippack --overwrite @listfile
./slippack x out.slippack -d outdir       # extract, recreating stored paths
./slippack t out.slippack                 # decode in memory without writing files
./slippack hash out.slippack              # SHA-1 per file in shasum format
./slippack selftest
make check
```

List files contain one input path per line. `-T` sets the thread count
(default: available hardware threads). `-v` prints compressed bytes by section.
`--codec custom` remains accepted for existing scripts; `legacy` and `tuned`
are rejected. Use `--` before filenames that start with a dash.

Source paths and archive names are separate. Relative paths retain their
subdirectories; absolute paths and paths outside the working directory use
basenames. Harmless `./` prefixes already stored in archives are preserved.
Duplicate names and file/directory conflicts are rejected.

Both creation and extraction refuse to overwrite existing files by default.
Pass `--overwrite` to replace regular files. Outputs are written to sibling
temporary files and installed atomically; extraction never follows symlinks
inside the selected destination. Archive members containing absolute paths,
parent traversal, backslashes, drive prefixes, NULs, or line breaks are rejected.
The destination root chosen by the caller is resolved once, including platform
aliases such as macOS `/var`. Each file is atomic; a failed multi-file extraction
can leave earlier completed files in place. Atomic installation does not promise
power-loss durability.

Decoding checks counts, sizes, and record shapes before allocating. Default
limits are 8 GiB of archive input, 16 GiB per decoded file, 1 TiB total decoded
output, just under 4 GiB per frame/record packet (the v1 frame maximum), 2 GiB of
shared pool output, and 512 MiB
for descriptors plus admitted side-stream metadata. The CLI exposes
`--max-archive-mib`, `--max-file-mib`, `--max-output-mib`, `--max-frame-mib`,
`--max-pool-mib`, and `--max-metadata-mib`. `--wave-mib` changes the default
2 GiB decoded-output admission target. These are validation and scheduling
limits, not a process RSS cap: input, pool buffers, record pointers, codec
scratch, and queued writes also occupy memory. Additional count limits are
explicit in `DecodeLimits` in [archive.h](archive.h).

`-l` controls the custom LZ search effort:

| Levels | Search candidates |
|---|---:|
| 1–2 | 2 |
| 3–5 (default 3) | 4, with lazy parsing |
| 6–8 | 8 |
| 9–12 | 24 |

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

## Code structure and library API

The build produces `build/libslippack.a` alongside the CLI. [archive.h](archive.h)
is the application interface: `encode_archive` accepts named input loaders and
returns owned archive bytes and statistics; `decode_archive` borrows archive
bytes, returns ordered entry information, and optionally sends owned decoded
files to a sink. Loaders and sinks may run concurrently. They must keep captures
alive for the call and synchronize shared state. No callbacks outlive a call.
Failures throw `slippack::Error` (or propagate callback/allocation exceptions) after
workers join; the CLI alone translates failures into diagnostics and exit codes.

| Module | Responsibility |
|---|---|
| `slippack.cpp` | CLI parsing, reporting, connecting loaders and sinks |
| `archive.h/.cpp` | v1 container, validation, scheduling, per-operation options/stats |
| `file_io.h/.cpp` | bounded writer queue, safe directory traversal, atomic file output |
| `slp.h/.cpp` | Slippi parsing, RNG folding, replay predictors, PPC transform |
| `record_codec.h/.cpp` | transpose/delta kernels, dictionary coding, shared packet parser |
| `frames.h/.cpp` | frame envelopes around the payload codec |
| `custom_codec.h`, `mask_codec.h`, `static_context.h` | self-contained payload coding headers |
| `binary_io.h`, `codec_types.h` | checked little-endian reading, varints, shared codec types |
| `parallel.h`, `platform.h/.cpp` | joining workers/error propagation, SHA-1 and allocator helpers |

Internal task payloads explicitly identify their work. Borrowed spans remain
valid until their owning plan's final task completes. Statistics and budgets
belong to each operation; concurrent calls and repeated calls after an error
are tested. Predictor operations establish round-to-nearest and restore the
caller's floating-point mode. The optimized kernels retain scalar alternatives.

## Compatibility

V1 names the same custom payload format previously labeled v15. New archives
use version byte `1`; the reader also accepts byte `15` as an alias for the
identical format. Existing v15 archives need no conversion. Older Slippi
replay record sizes are still supported inside either header.

Earlier archive formats and the former `tuned` backend are rejected. The
previous reader does not recognize the new v1 header; current readers accept
both headers through the same decoding path.

The last reader supporting v9–v15 and all three backends is commit
[`b66dd55`](https://github.com/Eppie/slippack/tree/b66dd55). To build it separately:

```sh
git worktree add --detach ../slippack-compat b66dd55
make -C ../slippack-compat
```

That checkout includes the old migration tools and requires zstd, LZMA, and
Brotli development libraries. The historical `main` branch contains an earlier
legacy implementation; the latest code is on `custom-codec`.

## Validation and benchmarks

Use `make format` to format all C++ code and insert braces around control-flow
bodies, or `make check-format` to check without editing. These targets require
clang-format 15 or newer; set `CLANG_FORMAT=/path/to/clang-format` if needed.

`make check` covers custom packets, dictionary framing, scalar predictor
oracles, exception propagation, concurrent/repeated library calls, path and
symlink confinement, atomic overwrites, limits, exact extraction, unsupported-format
rejection, and a committed
original-v15 fixture made by the pre-cleanup encoder. Fuzzing instructions are in
[fuzz/README.md](fuzz/README.md).

The `benchmarks/` directory and [EXPERIMENTS.md](EXPERIMENTS.md) record historical
experiments. Their backend selections and version comparisons describe the
commits used for those studies. For the latest recorded speed study, see
`benchmarks/2026-11-09-speed/`; all benchmark sets were examined during
development, so those results are not independent held-out estimates.
