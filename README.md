# Slippack — Slippi replay compressor

Slippack losslessly compresses one or many `.slp` replays into a `.slpc` archive.
Extraction restores the original bytes, including metadata, unknown events, and
malformed input. The compressor uses Slippi event structure, replay predictors,
field dictionaries, and a self-contained rANS/LZ codec. It requires no zstd,
LZMA, or Brotli libraries.

## Size and speed

**27.0:1 compression: 5.91 GB of replay data occupies 218.85 MB (3.70% of the
original size).** This level-3 measurement uses 1,929 development replays, each
in its own archive, with exact byte-for-byte reconstruction.

| Workers | Encode MB/s | Decode MB/s |
|---|---:|---:|
| 1 | **443** | **952** |
| 8 | **3,363** | **7,311** |

These are medians from the [2026-11-14 arm64 study](benchmarks/2026-11-14-encoder-parity/README.md)
on an Apple M4 Max in high-power mode on AC power: one warmup, three
measured runs, and inputs in RAM. Rates use original decimal MB/s and exclude
file I/O. Eight workers process eight independent replays. The corpus is
development data; results depend on replay contents and archive grouping.

### Compared with other codecs

**Slippack produces 71% smaller files than slpz fast on the matched 120-replay
set.** Slpz fast has higher throughput; Slippack encodes about 3.5 times faster
than slpz small while producing smaller files.

The [matched comparison](benchmarks/2026-11-13-competitors/README.md) measures
Slippack `64bff11` on an Apple M4 Max with one worker, RAM inputs, and three-run
medians on AC power. Packed size is a percentage of the original bytes; encode
and decode rates are original MB/s. Compare rows within the same matched set.

| Matched replays | Codec / setting | Packed size | Encode MB/s | Decode MB/s |
|---|---|---:|---:|---:|
| 120 | **Slippack level 3** | **3.90%** | **410.4** | **977.8** |
| 120 | slpz fast | 13.45% | 577.0 | 1,071.1 |
| 120 | slpz small | 12.20% | 118.3 | 1,220.1 |
| 98 | **Slippack level 3** | **3.81%** | **434.5** | **942.0** |
| 98 | slippc level 6 | 5.33% | 7.1 | 352.7 |
| 28 | **Slippack level 3** | **4.19%** | **375.1** | **907.2** |
| 28 | Peppi 2.1.2, internal Zstd | 17.69% | 260.1 | 868.1 |
| 28 | Peppi 2.1.2 + whole-file Zstd 3 | 11.36% | 391.0 | 1,102.9 |
| 28 | Peppi 2.1.2 + whole-file Zstd 12 | 10.07% | 130.3 | 1,078.0 |

The slippc subset contains the 98 replays within its supported version range.
Peppi rows use the 28 replays with verified exact reconstruction in this corpus;
Peppi also provides an Arrow representation for analysis. All reported outputs
reconstruct the original replay bytes exactly. The study records pinned tool
versions, input and binary hashes, coverage limits, and every timing sample.

## Build

A C++20 compiler is required. macOS uses the system CommonCrypto implementation
for SHA-1; Linux requires OpenSSL development headers and `libcrypto`.

```sh
make
make check
```

The default build produces `./slippack` and `build/libslippack.a`.

### Profile-guided build

```sh
make release-check
```

This builds and tests the profile-guided release, installs `./slippack`, and
produces `build/release/libslippack.a`. `make release` builds and installs without
running the tests. The included profile targets Apple Clang 17
(`clang-1700.0.13.5`) on arm64 macOS. Release builds verify the compiler, target,
codec source hashes, and profile checksum.

To generate a profile for a different compiler or changed codec sources, supply
a file containing one absolute replay path per line:

```sh
python3 tools/profile_build.py train --manifest /path/to/replays.txt --limit 64
make release-check
```

Training uses up to 64 evenly spaced inputs, compresses them at level 3, and
verifies exact reconstruction through temporary archives. Source files and
existing archives remain untouched. The profile stores execution counts and
symbols; its [provenance](profiles/level3.json) records compiler, source, and input
hashes. Plain `make` needs no profile. Use `make clean && make` to replace a
release executable with a default build.

For the measured Linux x86 build, use Clang 20, LLD 20, and the separate x86
profile. This builds into its own directory:

```sh
python3 tools/profile_build.py check --cxx clang++ \
  --profile profiles/linux-x86-level3.proftext
make -j4 CXX=clang++ BUILD_DIR=build/linux-x86 PROGRAM=build/linux-x86/slippack \
  'CXXFLAGS=-std=c++20 -O3 -Wall -Wextra -march=native -flto=thin' \
  PROFILE=profiles/linux-x86-level3.proftext LLVM_PROFDATA=llvm-profdata-20 \
  LDFLAGS=--ld-path=/usr/bin/ld.lld-20 check
```

`-march=native` targets the build machine. See the
[Linux x86 measurements](benchmarks/2026-11-14-linux-x86/README.md) for the
baseline, training corpus, encoder heuristics, and archive grouping results.

## Use

```sh
./slippack c replay.slp                          # creates replay.slpc beside the input
./slippack c first.slp second.slp                # creates archive.slpc
./slippack c -o out.slpc -l 3 -T 16 @replays.txt # one input path per line
./slippack l out.slpc                            # original size and stored name per file
./slippack x out.slpc -d outdir                  # extract stored paths
./slippack x out.slpc -d outdir game.slp         # extract only the named files
./slippack x out.slpc -d outdir --flatten        # extract without stored directories
./slippack t out.slpc                            # decode in memory without writing files
./slippack hash out.slpc                         # SHA-1 per file, in shasum format
./slippack selftest
```

Without `-o`, one input produces a sibling file with its extension replaced by
`.slpc`; multiple inputs produce `archive.slpc` in the working directory. This
rule also applies after expanding `@listfile` entries. List-file paths are
resolved from the working directory.

### Listing and selecting files

`l` prints one `size  name` line per file in archive order, where the size is
the original byte count. It reads only the archive directory, so its cost
depends on the number of files, not on the amount of replay data.

`x`, `t`, and `hash` accept stored names after the archive path, either as
arguments or in an `@listfile`, and then process only those files. Names match
in canonical form: `nested/game.slp` selects a file stored as
`./nested/game.slp`. Output follows archive order regardless of the order of
the request. A name that is not in the archive is an error, reported before
any file is written. Put `--` before names that begin with a dash.

A selection decodes the chosen files and the shared record pools they refer
to; other files are scanned for their record counts and not decoded. Each pool
is one compressed packet, so the pools a file uses are decoded whole. The
`--max-output-mib` limit applies to the selected files.

`-T` accepts 1–256 threads and defaults to the available hardware thread count,
capped at 256. Use `--` before filenames that begin with a dash. Set `SLIPPACK_TIMING=1` to report phase timings.

`-v` or `--verbose` adds detail. For `c` it reports compressed bytes by
section. For `x` and `t` it prints a `size  path` line on standard output as
each file completes; files are decoded concurrently, so the lines follow
completion order, and `x` shows the path written after any renaming.

`c`, `x`, `t`, and `hash` report completed files and original megabytes on
standard error while they run. On a terminal the report is one line redrawn in
place and erased before the summary. `--progress` requests it when standard
error is not a terminal, as one line per second; `--no-progress` turns it off.
Verbose `x` and `t` leave it off unless `--progress` is given.
Standard output is unaffected. For `c` the count is files read, which runs
slightly ahead of compression.

### Compression levels

`-l` accepts levels 1–12; the default is 3. Levels control LZ search effort:

| Levels | Search candidates | Parsing |
|---|---:|---|
| 1–2 | 2 | Greedy |
| 3 | 2 | Greedy for record planes; lazy for opaque data |
| 4–5 | 4 | Lazy |
| 6–8 | 8 | Lazy |
| 9–12 | 24 | Lazy |

Levels 3–12 also use literal prediction. Compression ratios depend on the replay
contents and whether files share an archive. Pooling side-event records across
replays can reduce repetition that remains in separate archives.

### Paths and output files

Relative input paths retain their subdirectories inside the archive. Absolute
paths and relative paths outside the working directory use basenames. Harmless
`./` prefixes are preserved. Duplicate names and file/directory conflicts are
rejected.

Creation and extraction refuse to overwrite existing files by default. Pass
`--overwrite` to replace regular files. Each output is written to a sibling
temporary file and installed atomically. Extraction resolves the chosen output
root once, including platform aliases such as macOS `/var`, and never follows
symlinks beneath that root. Archive member names cannot contain absolute paths,
parent traversal, backslashes, drive prefixes, NULs, or line breaks.

`x --strip-components N` removes the first N directories of each stored path,
and `x --flatten` removes all of them. Both apply to the selected files. The
resulting paths are checked before extraction starts: two files mapping to the
same path, a file mapping onto another's directory, or a path with fewer than
N directories is an error and nothing is written.

Atomicity applies to each file: a failed multi-file extraction can leave
completed files in place. Atomic installation does not guarantee durability
across power loss.

### Decode limits

The decoder validates counts, sizes, and record shapes before allocating.

| Resource | Default limit | CLI option |
|---|---:|---|
| Archive input | 32 GiB | `--max-archive-mib` |
| One decoded file | 16 GiB | `--max-file-mib` |
| Total decoded output | 1 TiB | `--max-output-mib` |
| One frame or record packet | 4 GiB − 1 byte | `--max-frame-mib` |
| Shared pool output | 2 GiB | `--max-pool-mib` |
| Descriptors and admitted side-stream metadata | 2 GiB | `--max-metadata-mib` |

Each option takes a size in MiB. `--wave-mib` sets the decoded-output admission
target, which defaults to 2 GiB. These limits govern validation and scheduling;
process memory also includes pool buffers, record pointers, codec scratch, and
queued writes. The CLI memory-maps the archive, so its pages are file-backed
and reclaimable; a library caller that loads the archive into memory holds
those bytes as well. Additional count limits are defined in `DecodeLimits` in
[archive.h](archive.h).

## Archive format and compatibility

Slippack reads and writes the **SLPC v1 custom format**. The encoder and decoder
contain a 136,904-byte dictionary of recurring Gecko code entries. Archives can
refer to byte ranges in that dictionary and store differing bytes as literals.
The dictionary itself is not stored in the archive. For code payloads, the
encoder compares dictionary references, contiguous-byte compression, and record
plane compression, then stores the smallest packet. At level 3, a dictionary
packet at most one quarter of the original size is stored without evaluating
the other two. Split-message padding and framing bytes are preserved exactly.

Archive bytes depend on the inputs, their stored names and grouping, and the
level. They do not depend on the CPU architecture, SIMD instruction set, or
worker count: arm64 and x86-64 builds write identical archives, and each
decodes the other's.

Decoding dictionary references requires support for the built-in dictionary
record extension and its dictionary ID. Decoders without that extension reject
those packets. The decoder also accepts v1 packets without the extension.
Dictionary IDs and their associated bytes are permanent format dependencies;
unknown IDs are rejected.

See [CUSTOM_FORMAT.md](CUSTOM_FORMAT.md) for the format specification and
[dictionaries/README.md](dictionaries/README.md) for dictionary construction,
provenance, and reproduction.

## How compression works

1. Parse the UBJSON wrapper and self-described event payload sizes, preserving
   the original header, metadata, unparsed tail, and event order.
2. Split events by command and player/follower or item group. Pool eligible
   side-event records across files, within a 512 MiB archive-wide budget
   divided equally among the inputs; records beyond a file's share stay in
   that file's own packets.
3. Predict pre-frame values from preceding post-frame state, fold RNG seeds
   into bounded LCG step counts, and predict positions and controller inputs.
   Predictors account for the fields available in each replay record size.
4. Transpose records into byte planes and replace low-cardinality fields with
   sorted dictionary indexes. Constant dictionaries need no index plane.
5. Choose plain, subtract-delta, or XOR-delta transforms. Encode semantic planes
   with constant, sparse, and run packets, shared presence masks, and rANS/LZ.
   Code payloads also have the built-in dictionary and contiguous-byte options.
6. Store opaque data in custom frames. Large attachments use 1 MiB tasks and
   can use a reversible PowerPC branch transform.

Encoding pipelines file planning into a shared task queue; decoding assembles
files as their dependencies complete. NEON accelerates transforms on ARM64;
SSE2 and, where the build enables them, AVX-512 kernels do so on x86-64. Scalar
alternatives produce the same bytes. Float predictors require IEEE binary32 arithmetic
with round-to-nearest; fast-math builds are unsupported.

## Library API and code structure

[archive.h](archive.h) exposes the in-memory API. `encode_archive` accepts named
input loaders and returns owned archive bytes and statistics. `decode_archive`
borrows archive bytes, returns ordered entry information, and optionally sends
owned decoded files to a sink. `DecodeOptions::members` restricts decoding to
named files. `list_archive` returns names and original sizes from the archive
directory without decoding payloads; it suits memory-mapped archives because it
touches only the first bytes of each entry. Library calls default to one worker; callers can
set options independently for each operation.

Loaders and sinks can run concurrently. Their captures must remain alive for
the call, and shared state requires synchronization. No callbacks outlive a
call. Failures throw `slippack::Error`, or propagate callback/allocation
exceptions, after workers join. The CLI translates exceptions into diagnostics
and exit codes. Predictor operations establish round-to-nearest and restore the
caller's floating-point mode.

| Module | Responsibility |
|---|---|
| `slippack.cpp` | CLI parsing, reporting, loaders, and sinks |
| `archive.h/.cpp` | Container, validation, scheduling, options, and statistics |
| `file_io.h/.cpp` | Bounded writer queue, mapped input, directory traversal, and atomic output |
| `slp.h/.cpp` | Slippi parsing, RNG folding, replay predictors, and PPC transform |
| `record_codec.h/.cpp` | Transpose/delta kernels, field dictionaries, and packet parsing |
| `gecko_dictionary.h/.cpp`, `gecko_dictionary_data.h` | Built-in code dictionary and byte-range references |
| `frames.h/.cpp` | Frame envelopes around the payload codec |
| `custom_codec.h`, `mask_codec.h`, `static_context.h` | Payload coding |
| `binary_io.h`, `codec_types.h` | Checked binary I/O and shared codec types |
| `parallel.h`, `platform.h/.cpp` | Worker joining, error propagation, SHA-1, and allocator helpers |

## Validation and measurements

`make check` covers codec packets, field and built-in dictionaries, scalar
predictor oracles, concurrent and repeated library calls, exception propagation,
path and symlink confinement, atomic overwrites, decode limits, exact extraction,
listing, member selection, renamed extraction paths, verbose output, progress reporting,
unsupported-format rejection, and a frozen v1 fixture with independently
generated expected contents. [fuzz/README.md](fuzz/README.md) describes the
sanitizer-backed fuzzing harnesses.

Use `make format` to format C++ code and insert control-flow braces, or
`make check-format` to check without editing. These targets require clang-format
15 or newer; set `CLANG_FORMAT=/path/to/clang-format` to select it.

Performance measurements are tied to the builds, corpora, hardware, and archive
grouping documented in each study:

- [Architecture-independent encoder study, 2026-11-14](benchmarks/2026-11-14-encoder-parity/README.md):
  arm64 sizes and speed for the headline figures above, and byte-identical
  arm64 and x86-64 archives with exact decoding in both directions. The x86-64
  build ran under Rosetta 2, which does not exercise the AVX-512 kernels.
- [Linux x86 study, 2026-11-14](benchmarks/2026-11-14-linux-x86/README.md):
  level-3 encoding on an AMD Ryzen 9 7950X with the x86 SIMD kernels, a separate
  profile, and ThinLTO, for separate and pooled archives.
- [Gecko dictionary study, 2026-11-13](benchmarks/2026-11-13-gecko-dictionary/README.md):
  1,929 development replays compressed individually at level 3 occupy 5.97% less
  space than the `60f7ab8` build, with roughly unchanged speed. The 854 files from
  source families excluded from dictionary construction occupy 4.22% less space.
  Those families are part of codec development, so this is not an independent
  holdout for the whole compressor. Savings for individual archives do not
  predict savings for collections that already pool records.
- [Encoding generalization study, 2026-11-12](benchmarks/2026-11-12-generalization/README.md):
  frozen-build comparisons against `3d39721`, including a 512-file independent
  validation from 16 source families, all timing repetitions, and limitations.
- [Matched codec comparison, 2026-11-13](benchmarks/2026-11-13-competitors/README.md):
  Slippack at `64bff11` compared with slpz, slippc, and Peppi on matched input
  subsets, with version coverage, exact reconstruction checks, and raw results.

[EXPERIMENTS.md](EXPERIMENTS.md) and [benchmarks/](benchmarks/) contain experiment
history, rejected approaches, build provenance, and reproduction instructions.
