# Level 3 hardware profiling

Baseline: `cf99bea9ee37cc00eb0fd14a6ba87bf9002400c4`, Apple M4 Max,
macOS 15.5, Apple Clang 17, C++20 `-O3`. The inputs are the same 120 replays
(313,007,679 original bytes) used in the single-file comparison. Each replay
gets its own archive, at level 3 with one worker.

The power mode was not recorded during these profiles. Later optimization
experiments revealed a switch between low and high power mode, so elapsed
times from these profiles must not be compared with later speed results.

## Where the time goes

A 10-second time profile contained 5,455 samples in encoding call stacks.
Disjoint groups from `profile-breakdown.json`:

| Work | Share of encoding samples |
|---|---:|
| LZ match search and parsing | 26.9% |
| Record and dictionary preparation | 18.4% |
| Presence masks | 12.7% |
| Entropy coding | 10.7% |
| Other plane compression | 12.4% |
| Other archive work | 18.9% |

Memory copies and allocation costs cross these categories. Dictionary sorting
accounted for about 1.7% of encoding samples. Sampling and inlining limit the
precision of these categories.

## Direct counters

The [cpu_counter](https://github.com/Eppie/cpu_counter) harness at commit
`68e13b4f2e8b20b52727e417dcc549bca98bc21e` measured four counter groups in
separate passes, with three repetitions each. Every measurement follows a
warmup and checks compressed bytes against its reference. Only measurements
with live configurable counters and matching performance-core IDs at both
endpoints were accepted. Endpoint checks cannot exclude a migration away and
back during a measurement. There were 360 accepted measurements per group;
27 attempts across all four groups were rejected and retried.

`direct-pmu-summary.json` contains the per-repetition totals. Rounded medians:

| Measurement | Result |
|---|---:|
| Instructions per cycle | 3.93–3.95 |
| Branch mispredictions / branches | 1.62% |
| Retired L1 load misses | 71.5 million |
| Retired L1 load misses / 1,000 integer and SIMD load instructions | 18.1 |
| Mapper stalled cycles / cycles | 18.1% |
| Mapper stalled by dispatch / cycles | 17.1% |

Mapper stalls mean the front end cannot advance work through the mapping
stage. Dispatch backpressure can come from downstream queues, dependencies,
or execution resources. These two stall counters overlap and must not be added.
They do not identify time waiting for RAM. An L1 miss can be served by another
cache; the load-instruction ratio is a proxy, not an exact cache hit rate.
The available L2 TLB counter measures address translation, not L2 data-cache
misses. These measurements support investigating memory access and instruction
dependencies, but do not establish that DRAM latency dominates this encoder.

A separate performance-core control loop with 256 AArch64 NOP instructions
per iteration achieved aggregate IPC 9.99. It demonstrates that this machine
can retire substantially more than four instructions per cycle under favorable
conditions. NOP throughput is not a useful-work target or a predicted speedup
for compression.

## Function attribution with Instruments

Instruments 16's CPU Counters instrument also collected cycles, instructions,
branches, branch misses, and L1 load misses at 1 ms intervals. Encoding samples
on performance cores gave IPC 3.92 and branch mispredictions around 1.63%,
consistent with the direct counters.

A separate 20-second debug-symbol profile sampled every 20,000
`L1D_CACHE_MISS_LD_NONSPEC` events. Among 13,340 encoding samples on performance
cores, the largest leaf-function groups were:

| Function or region | Share of retired-load-miss samples |
|---|---:|
| Memory copies | 22.5% |
| Record transpose (`tile16`) | 19.7% |
| LZ match search (`Matcher`) | 10.9% |
| Dictionary hash lookup (`U32Set::find_or_insert`) | 6.3% |
| Field loads (`little_word`) | 3.1% |

The complete function summary is in `load-miss-attribution.json`. These are
shares of miss samples, not runtime shares. A cheap streaming transform can
generate many misses while overlapping their latency. Event sampling can skid
past the instruction that triggered a miss; optimized source-line attribution
is approximate. The relevant regions are `transpose_fwd` and dictionary trials
in `record_codec.cpp`, and `Matcher::find` in `custom_codec.h` at the pinned
baseline commit.

Instruments provides the function/source attribution needed for this work and
does not require running the benchmark itself with sudo. The direct harness
remains useful for scripted, per-encode counter totals and independent checks.
See Apple's [CPU performance session](https://developer.apple.com/videos/play/wwdc2025/308/)
for the pipeline and counter interpretation.

## Reproduction

Build the pinned baseline library and the harness against the pinned
cpu_counter checkout (its header uses Apple's private counter APIs):

```sh
c++ -std=c++20 -O3 -pthread -I/path/to/cpu_counter \
  benchmarks/2026-11-10-level3-profiling/counter_runner.cpp \
  /path/to/baseline/build/libslippack.a -o /tmp/slippack-counters
sudo /tmp/slippack-counters /path/to/selected-120.txt cache 3
sudo /tmp/slippack-counters /path/to/selected-120.txt branch 3
sudo /tmp/slippack-counters /path/to/selected-120.txt stall 3
sudo /tmp/slippack-counters /path/to/selected-120.txt traffic 3
```

The core-ID check in this harness is specific to this 16-core M4 Max. Adapt it
for other machines. In Instruments, configure the CPU Counters instrument with
the named events above; its default empty event selection is insufficient.
Build with `-O3 -g` and generate a dSYM for source attribution. Capture encoding
only, then restrict results to encoding stacks on performance cores. Raw traces
are omitted because they contain machine identifiers and local paths.
