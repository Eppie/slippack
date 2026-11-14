# Built-in Gecko dictionary validation

The encoder now compares the existing plane packet with contiguous-byte coding
and references into an immutable built-in dictionary for commands 0x10 / 0x3d.
It stores the smallest packet. References can cover partial entries, so edited
payloads do not need an exact whole-list match. All record bytes, including split
packet padding and framing, are preserved. The dictionary is in the executable
and library, not the archive.

Dictionary v1 contains 136,904 bytes / 596 entries. Selection used eight replay
hashes per eligible development source, retained entries occurring in at least
two selected replays, and excluded eight source families using a fixed source
hash rule. Of 136 selected replays, 80 contain payloads used by this path.
[Provenance](../../dictionaries/gecko-v1.json) records every input and entry hash.
The random comparison replay is not a training input.

The baseline is commit `60f7ab839a81565dceccaeda689a4904c60c0273`, with its frozen
release binary/library. Both variants use source-matched PGO profiles trained on
the same 64 inputs. [Binary hashes](binaries.json) and all candidate source hashes
in [the raw run](development.json) identify the measured builds. The installed
executable grew from 1,047,640 to 1,197,352 bytes (+149,712).

## Level 3 sizes

One replay per archive, identical stored names:

| Cohort | Files | Before | After | Reduction |
|---|---:|---:|---:|---:|
| All development | 1,929 | 237,274,426 | 223,105,405 | 5.97% |
| Not dictionary training inputs | 1,793 | 218,939,312 | 206,022,252 | 5.90% |
| Source families excluded from dictionary | 854 | 98,640,626 | 94,473,080 | 4.22% |
| Outside original 521-file set | 1,408 | 183,629,678 | 171,824,522 | 6.43% |
| Dictionary training inputs | 136 | 18,335,114 | 17,083,153 | 6.83% |

There are 1,298 smaller files, 631 unchanged files, and no larger files. All
original bytes reconstructed exactly; compressed fingerprints were stable across
repeats and worker counts. Cohorts overlap. The excluded-source cohort tests
dictionary generalization, but these files were already used in broader codec
development: this is **not a fresh holdout for the whole compressor**. No final
holdout files were consulted or added to the dictionary.

| Prior comparison file | Original | Before L3 | After L3 | PAQ8PX L12 |
|---|---:|---:|---:|---:|
| Best, `01ffbfd6ad363aaee817a6ca6b9f8aed049c9481.slp` | 2,046,146 | 16,110 | 16,110 | 32,572 |
| Random, `0c9caeb8fab2376816d4f58700f6e4d53179d8e6.slp` | 3,378,666 | 112,959 | 69,706 | 77,502 |
| Worst >=1 MB, `Game_20210715T103147.slp` | 13,846,612 | 1,817,428 | 1,817,428 | 393,165 |

PAQ sizes are the user's supplied measurements. The random file improves 38.29%;
its 56,760-byte split-record stream now occupies a 2,823-byte dictionary packet,
down from 46,076 bytes. Its few differences and all transport bytes are encoded
losslessly. The worst file still has the separate stale raw-length parser issue;
this change does not integrate the earlier parser-recovery experiment.

## Speed and correctness

Apple M4 Max, high-power mode (2), battery power throughout, one warmup and three
randomly interleaved measured repeats. Inputs reside in RAM, one archive per
replay; file reading, verification, and output fingerprints are outside timers.
Each encode/decode invocation uses one worker; T8 runs eight files concurrently.

| Operation | Workers | Before median (s) | After median (s) | Time change |
|---|---:|---:|---:|---:|
| Encode | 1 | 14.4844 | 14.2567 | -1.57% |
| Decode | 1 | 6.1330 | 6.0448 | -1.44% |
| Encode | 8 | 1.9294 | 1.9037 | -1.33% |
| Decode | 8 | 0.8130 | 0.8119 | -0.13% |

These small differences are within run variability; the evidence supports
roughly unchanged speed, not a precise speedup claim. This is a short local
measurement on battery power, not the earlier independent speed qualification.

- `make release-check` passed all codec, predictor, API, frozen old-format,
  extraction, and CLI integration tests. The new dictionary test passes 16,697
  checks, including pinned dictionary identity, concurrent first use, edits,
  nonzero padding, forced extension decoding, every packet truncation, unknown
  IDs, output limits, and malformed tokens.
- ASan/UBSan dictionary and archive API tests passed. A focused dictionary/record
  fuzzer completed 355,625 cases in 46 seconds without a failure. This is a smoke
  campaign, not exhaustive fuzzing.
- A real 16-replay pooled archive round-tripped at levels 1, 3, 7, and 9 with eight
  workers. Level 3 shrank from 2,065,959 to 1,927,590 bytes; all four levels improved.
  See [pooled results](pooled.json).
- Regenerating the dictionary header and provenance reproduces both byte for byte.
- The release PGO provenance check passes; source replay hashes were verified
  before and after the full benchmark. Inputs and existing archives were unchanged.

The updated decoder reads older v1 archives. Older executables reject the new
record extension. Dictionary ID 1 and its bytes are permanent; future releases
must keep them available for decoding. See [the format](../../CUSTOM_FORMAT.md).

## Reproduction

```sh
python3 tools/profile_build.py train --manifest build/frontier/all-521.txt --limit 64 --jobs 3
make release-check
c++ -std=c++20 -O3 -pthread -Wl,-no_deduplicate \
  benchmarks/2026-11-10-single-file/cpp_runner.cpp build/release/libslippack.a \
  -o build/gecko-dictionary-v1/release-runner
python3 benchmarks/2026-11-10-encode-speed/benchmark.py \
  --baseline build/gecko-dictionary-v1/baseline-runner \
  --candidate build/gecko-dictionary-v1/release-runner \
  --baseline-commit 60f7ab839a81565dceccaeda689a4904c60c0273 \
  --manifest build/goal-2026-11-12/development-round3.txt \
  --output benchmarks/2026-11-13-gecko-dictionary/development.json \
  --levels 3 --threads 1,8 --repeats 3 --allow-byte-changes
python3 benchmarks/2026-11-13-gecko-dictionary/summarize.py
```

Compile the baseline runner with the same runner source and flags, linking the
frozen pre-change library. [Summary JSON](summary.json) includes per-source sizes
and full paths to the three comparison files.
