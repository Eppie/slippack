# Architecture-independent encoder heuristics on arm64

The record encoder's packet-selection heuristics run on every architecture, so
arm64 and x86-64 builds write the same archive bytes. This study measures their
effect on Apple arm64 at level 3 against commit
`0e90b0198f6feacbf79cc5e940dacaec8c9819da`'s arm64 behaviour, where those
heuristics were compiled for x86 only.

The heuristics, described in the
[Linux x86 study](../2026-11-14-linux-x86/README.md), are: early rejection of
field-dictionary trials, immediate acceptance of a strong built-in Gecko
dictionary packet for code commands, and contiguous raw storage of singleton
non-code records.

## Method

Apple M4 Max, macOS 15.5, Apple Clang 17 (`clang-1700.0.13.5`), high-power mode
(2), AC power. One replay per archive, inputs in RAM, one warmup, randomly
interleaved measured repetitions. File reading, verification, and output
fingerprints are outside timers. Every run reconstructs all original bytes
exactly. Rates are original decimal MB divided by median seconds.

Each executable links the same compiled
[benchmark driver](../harness/cpp_runner.cpp) through
[`benchmark.py`](../harness/benchmark.py). Profile-guided builds
use `-fprofile-use` and `-Wl,-no_deduplicate`. The candidate's profile is
`profiles/level3.proftext`, trained on the candidate sources. The baseline's
profile was trained on the same 64 inputs with earlier sources; the compiler
discarded its counts for three changed functions.

These corpora were examined during development and include the 64
profile-training inputs. They are not independent holdouts.

## Sizes

| Corpus | Files | Original bytes | Before | After | Change |
|---|---:|---:|---:|---:|---:|
| Development | 1,929 | 5,914,472,893 | 223,105,405 | 218,851,097 | −1.91% |
| 521-file set | 521 | 1,309,548,357 | 51,280,883 | 50,310,840 | −1.89% |

In the development corpus 1,928 archives are smaller, one is unchanged, and none
is larger. The development corpus compresses to 3.70% of its original size
(27.03:1).

The change applies to archives holding one replay. Archives that pool many
replays keep the same bytes: a 128-replay archive is identical before and after
at levels 1, 3, and 9. Single-replay archives keep the same size at levels 1
and 9.

## Speed

Development corpus, profile-guided builds, three measured repetitions
([`development.json`](development.json)):

| Operation | Workers | Before MB/s | After MB/s | Change |
|---|---:|---:|---:|---:|
| Encode | 1 | 410.9 | 443.4 | +7.9% |
| Encode | 8 | 3,223.9 | 3,363.3 | +4.3% |
| Decode | 1 | 963.7 | 951.5 | −1.3% |
| Decode | 8 | 7,371.6 | 7,310.7 | −0.8% |

521-file set, 21 measured repetitions:

| Build | Operation | Workers | Before MB/s | After MB/s | Median change | Fastest-run change |
|---|---|---:|---:|---:|---:|---:|
| Profile-guided ([`all-521-release.json`](all-521-release.json)) | Encode | 1 | 398.4 | 429.8 | +7.9% | +4.3% |
| Profile-guided | Encode | 8 | 2,831.3 | 3,094.4 | +9.3% | +7.8% |
| Profile-guided | Decode | 1 | 933.4 | 938.9 | +0.6% | +0.5% |
| Profile-guided | Decode | 8 | 7,020.5 | 7,109.2 | +1.3% | +1.0% |
| Default `-O3` ([`all-521-default.json`](all-521-default.json)) | Encode | 1 | 368.9 | 396.8 | +7.6% | +2.5% |
| Default `-O3` | Encode | 8 | 2,698.3 | 2,916.6 | +8.1% | +4.8% |
| Default `-O3` | Decode | 1 | 908.4 | 915.9 | +0.8% | +0.2% |
| Default `-O3` | Decode | 8 | 6,884.1 | 6,920.3 | +0.5% | +0.4% |

Encode samples within one build and worker count span 6% to 20% from fastest
to slowest, so a run with few repetitions can order the builds either way. With
21 interleaved repetitions the heuristics encode faster by both the median and
the fastest run in every configuration, and decoding speed is unchanged. The
profile-guided rows also include the effect of a profile matched to the
candidate sources. The larger x86 encoding gain in the Linux study comes mostly
from its SIMD kernels, which arm64 already has in NEON form.

## Cross-architecture output

An arm64 build and an x86-64 build (`-arch x86_64`, run under Rosetta 2) write
byte-identical archives for 128 replays from the independent 512-file
validation set: one replay per archive at levels 1, 3, and 9, and all 128 in
one archive at the same levels. Each build decodes the other's archives to the
exact original bytes, and the arm64 build of `0e90b01` decodes them as well.

Rosetta 2 provides SSE2 through AVX2. The AVX-512 kernels selected by
`-march=native` on supporting processors are not covered by this comparison;
`check_exception_oracles` in `tests/custom_codec_test.cpp` checks those kernels
against a byte oracle on hardware that has them.

## Reproduce

```sh
git worktree add --detach /path/to/base 0e90b01
make -C /path/to/base build/libslippack.a
make -C /path/to/base BUILD_DIR=build/pgo PROFILE=profiles/level3.proftext \
  build/pgo/libslippack.a
make release-check
c++ -std=c++20 -O3 -pthread -I. -c \
  benchmarks/harness/cpp_runner.cpp -o /tmp/runner.o
c++ -std=c++20 -O3 -pthread -Wl,-no_deduplicate /tmp/runner.o \
  /path/to/base/build/pgo/libslippack.a -o /tmp/baseline-runner
c++ -std=c++20 -O3 -pthread -Wl,-no_deduplicate /tmp/runner.o \
  build/release/libslippack.a -o /tmp/candidate-runner
python3 benchmarks/harness/benchmark.py --allow-byte-changes \
  --baseline /tmp/baseline-runner --candidate /tmp/candidate-runner \
  --baseline-commit 0e90b01 --manifest /path/to/replays.txt --levels 3 \
  --threads 1,8 --repeats 3 --power-mode 2 --output /tmp/result.json
```

The 521-file rows use `--repeats 21`.
For the default-build rows, link the two `build/libslippack.a` libraries
without `-Wl,-no_deduplicate`.
