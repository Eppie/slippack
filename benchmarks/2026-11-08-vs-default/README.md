# Current custom versus the original SLPZ default

The original default is **v9, level 3**, rebuilt from commit `37345ca` using
`-std=c++20 -O3 -Wall -Wextra` and the same installed compression libraries.
Current custom is commit `22e61aa`, custom backend at level 3. Current legacy is
also measured to distinguish the historical baseline from shared improvements
that now apply to every backend. The CLI still defaults to legacy.

Apple M4 Max, 16 threads, nine warmed repetitions per dataset, randomized
backend order each round. Both compression and decompression use the respective
binary. Timing includes process startup and input reads. Decompression uses
`slpz t`, including reconstruction and excluding output-file writes. Every
file's decoded SHA-1 matches its source. No compilation or fuzzing ran alongside
these timing rounds. Timings are medians and depend on the machine/workload.

The 522 files comprise 521 replays and one provenance TSV, totaling
1,309,561,719 input bytes. All datasets have been used in earlier experiments.

## Original default versus current custom

Sizes are decimal MB. Archive/raw is compressed bytes divided by source bytes;
lower is better. All arrows below mean original default → current custom.

| Dataset | Archive MB | Archive/raw | Compression seconds | Decompression seconds |
|---|---:|---:|---:|---:|
| Summit sample (140) | 22.436 → 17.562 | 6.518% → 5.102% | 0.504247 → 0.211107 | 0.116067 → 0.079367 |
| Mixed (103) | 20.211 → 14.112 | 6.790% → 4.741% | 0.458093 → 0.203315 | 0.102095 → 0.069193 |
| Additional Summit (279) | 41.292 → 31.225 | 6.184% → 4.677% | 0.966677 → 0.396302 | 0.198501 → 0.135449 |

| Dataset | Smaller archive | Less compression time | Less decompression time |
|---|---:|---:|---:|
| Summit sample (140) | 21.73% | 58.13% | 31.62% |
| Mixed (103) | 30.17% | 55.62% | 32.23% |
| Additional Summit (279) | 24.38% | 59.00% | 31.76% |

Across these three separate archives, total size falls from **83,938,700
to 62,898,815 bytes (25.07% smaller)**. The weighted
archive/raw fraction falls from **6.410% to 4.803%**,
equivalent to source/archive ratios of **15.60:1 → 20.82:1**.

Summing the per-set median times gives 1.929018 → 0.810724
seconds compression (2.38× throughput) and
0.416663 → 0.284009 seconds decompression
(1.47× throughput). These sums describe processing the
three separate archives; they are not timings for a single combined archive.

## Current legacy for context

The CLI still defaults to legacy. Its shared RNG and empty-stream improvements
mean it is no longer identical to the source we started with. The original
source baseline is byte-identical to the `original.cpp` snapshot saved at the
start of today's study (SHA-256
`84ecf09b309118d4596aaec8269265703c993c2458f4cfd3d21c930a3a2c264a`).

| Dataset | Codec | Bytes | Compression seconds | Decompression seconds |
|---|---|---:|---:|---:|
| Summit sample (140) | original_default | 22,436,101 | 0.504247 | 0.116067 |
| Summit sample (140) | current_legacy | 22,327,255 | 0.502822 | 0.108059 |
| Summit sample (140) | custom | 17,561,851 | 0.211107 | 0.079367 |
| Mixed (103) | original_default | 20,210,993 | 0.458093 | 0.102095 |
| Mixed (103) | current_legacy | 19,726,355 | 0.460967 | 0.098728 |
| Mixed (103) | custom | 14,112,375 | 0.203315 | 0.069193 |
| Additional Summit (279) | original_default | 41,291,606 | 0.966677 | 0.198501 |
| Additional Summit (279) | current_legacy | 41,090,096 | 0.961421 | 0.190525 |
| Additional Summit (279) | custom | 31,224,589 | 0.396302 | 0.135449 |

## Reproduction

Build `slpz.cpp` from commit `37345ca` separately using the repository's original
Makefile flags. With the current binary built from `22e61aa`, run:

The historical driver and its command-line options are archived at commit
`5cdb2bf`: [compare_default.py](https://github.com/Eppie/slippack/blob/5cdb2bf/benchmarks/compare_default.py).

Repeat with the mixed and holdout lists and their corresponding input roots.
The JSON files retain every timing sample, binary SHA-256, and input-list
SHA-256. Both old and new outputs matched all 522 source-file hashes. No codec
implementation or default selection changed during this comparison.
