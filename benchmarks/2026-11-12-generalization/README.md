# Encoding speed on held-out replays

The level-3 encoder at commit `aa9f3a9` compresses at least 10% faster than the
profile-guided release at `3d39721`, writes smaller output in total, and decodes
slightly faster, with one and with eight workers. The comparison holds on 512
replays from 16 source families that were excluded from tuning and from
compiler-profile training until the build was frozen.

| Workers | Encode throughput gain | Encode time reduction | Decode throughput gain |
|---|---:|---:|---:|
| 1 | 12.67% | 11.25% | 0.72% |
| 8 | 12.89% | 11.42% | 0.82% |

Total output is 64,132,365 bytes against 64,217,482 (0.13% smaller). Of the 512
files, 406 shrink, 1 is equal, and 105 grow; the largest growth is 4,772 bytes
or 3.34%. Five of the 16 source families grow in aggregate.

The same build passes on the development data:

| Cohort | Files | Encode gain, 1 / 8 workers | Output | Decode gain, 1 / 8 workers |
|---|---:|---:|---:|---:|
| Development outside the original 521 | 1,408 | 12.70% / 12.87% | 0.16% smaller | 1.01% / 1.17% |
| All development | 1,929 | 12.25% / 11.72% | 0.18% smaller | 0.89% / 0.96% |

## Method

Apple M4 Max, AC power, high-power mode (2), Apple Clang 17 at `-O3`. Both
libraries are profile-guided from the same 64 development inputs and link with
`-Wl,-no_deduplicate` against the same
[benchmark driver](../harness/cpp_runner.cpp). One replay per archive, inputs
in RAM, one warmup, then randomly interleaved repetitions. File reading,
hashing, and byte verification are outside the timers. Every run reconstructs
the original bytes exactly.

Each cohort is measured in exactly three process launches of nine repetitions,
and all 27 repetitions enter the result. Gains are ratios of arithmetic-mean
elapsed times. The launch count was fixed before measuring, and the gates run
in order: the 1,408-file cohort, then all 1,929 development files, then the
held-out 512. A cohort is only measured after the previous one passes. The
host is kept awake and checked for sleep during each launch.

## Data separation

Development data is 1,929 replays from 25 source collections, 5,914,472,893
bytes. [development-round3-inventory.json](development-round3-inventory.json)
records each input's hash, size, and source archive; it holds no replay data or
file names. The Gecko dictionary build reads this inventory.

The held-out set is 32 replays from each of 16 source families that appear
nowhere in development. The families are selected by a hash of the source name,
fixed before any member was extracted. No held-out file shares a SHA-256 with a
development or profile-training file. A second audit hashes the parsed event
stream, without headers or metadata, and finds no duplicates among 2,438 usable
streams.

## Limits

- The held-out files are consumed. They have informed later work and are
  development data for any further tuning.
- Timing varies between launches. The three eight-worker held-out launches gain
  19.96%, 7.01%, and 12.09%. The repeated-timing interval for the pooled
  eight-worker gain is 7.30% to 19.53%, which describes this machine's timing
  spread and not uncertainty over unseen source families.
- The result is a mean over the declared files on one machine. Individual
  replays and whole collections can grow.
- Family labels are heuristic. Exact file and event hashes cannot identify
  altered recordings of the same game.

## What transfers

A gain measured on one corpus overstates the gain on new sources:

| Build | Development gain, 1 / 8 workers | Held-out gain, 1 / 8 workers |
|---|---:|---:|
| Third candidate | 11.03% / 11.40% on 905 files | 5.00% / 7.95% |
| Sixth candidate | 10.64% / 11.00% on 1,417 files | 8.24% / 11.52% |
| `aa9f3a9` | 12.25% / 11.72% on 1,929 files | 12.67% / 12.89% |

Each failed validation used a fresh 512-file set, which then joined
development. Three further observations shape the protocol above:

- A single nine-repetition launch can pass or fail by itself. One build's three
  single-worker launches gain 9.13%, 11.07%, and 8.80%.
- A three-repetition screen on the 1,408-file cohort can reverse on the full
  1,929 files: carrying the reference-byte window across presence symbols gains
  2.38% on the first and loses 2.23% on the second.
- The original 521 files respond more strongly than later sources, so a gate on
  the files outside them keeps that collection from carrying the average.

## Changes in the build

These account for the gain, and none selects behaviour by file name, hash, or
collection:

- Ten-bit entropy tables, compact presence-model headers, implicit unused
  semantic trees, and constant channels.
- Less match searching on transformed planes at level 3, with two
  recent-distance probes.
- Uniform-run presence counting, and presence-model selection that charges the
  actual header bytes.
- A direct dictionary index for fields with two varying bytes, and one index
  buffer reused across dictionary trials.
- Context histograms summed across history buckets once per symbol; tree
  construction that skips leaves that cannot split and stops at 16 leaves.
- Direct mask writes with deferred decoder labels, and reusable encoder scratch
  buffers.
- Encoder presence tables packed into 64 bytes.
- Presence encoding in groups of four symbols with four independent states
  that the compiler keeps in registers.

## Measured and rejected

Each of these preserved the compressed bytes unless noted, and was slower or
no faster on the development data:

- **Compiler profile.** Training on 272 or 400 inputs balanced across sources
  instead of 64: between 3.1% slower and 1.7% faster, and not consistent across
  cohorts. `-mtune=native` produces byte-identical code.
- **Dictionary lookups.** Reading values directly from records (2.5% to 3.4%
  slower), a cache of the two most recent values, a SIMD path for batches of
  those two values, and packed lookup slots.
- **Presence coding.** A one-entry model/symbol cache, although 59.8% of
  presence symbols repeat the previous model and symbol; bounded
  renormalization; direct-frequency and frequency-bias table packing; separate
  loops for referenced and unreferenced symbols (decoding 5% slower); counting
  unreferenced nibbles before expanding them; skipping deterministic tails,
  which applies to 20 of 290,968,926 decoded nibbles.
- **Byte-wide presence alphabet.** Grouping eight decisions under an 18-bit
  joint CDF changes the bytes, saves 118 bytes on 1,408 files, and decodes
  about 3% slower.
- **Match scanning.** Sixteen-byte NEON match-length comparisons.
- **Decoder.** Local presence state, and the next model packed into unused
  decoder-entry bits.
- **Parsing and tables.** A cursor-based parser, smaller encoder tables, and
  block copies for short records.

The per-run timing samples, protocols, audits, and scripts behind these figures
are in the repository history at commit `a62de17`.
