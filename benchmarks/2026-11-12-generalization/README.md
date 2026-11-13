# Development and independent validation

**Candidate 8 meets goal 2 on independent validation:** at least 10% faster
compression, smaller total compressed output, and faster average decoding,
with both one and eight workers. The baseline is `3d39721`; measurements use
level 3, one replay per archive, RAM inputs, and an Apple M4 Max on AC power.
The 512 held-out files are 32 from each of 16 additional source families,
excluded from tuning and compiler-profile training until this frozen build
entered final validation.

| Workers | Encode throughput gain | Encode time reduction | Decode throughput gain |
|---|---:|---:|---:|
| 1 | 12.6718% | 11.2466% | 0.7197% |
| 8 | 12.8895% | 11.4178% | 0.8233% |

Total output decreases from 64,217,482 to 64,132,365 bytes (**0.1325% smaller**).
All 27 repetitions from exactly three launches are retained in the
[independent pooled result](candidate8-awake1-holdout-replication-summary.json),
including source-size breakdowns and every launch's measurements. The
[completion audit](candidate8-completion-audit.json) verifies all nine guarded
launches across the three cohorts, rehashes all 2,441 input files, checks
source-family/training separation, verifies correctness evidence and frozen
artifacts, and confirms that the installed CLI matches the tested binary.

Timing variation remains material: eight-worker independent launch gains are
19.96%, 7.01%, and 12.09%; the pooled gain is 12.89%. Its descriptive 95%
repeated-timing interval is 7.30%–19.53%, not an interval over unseen source
families. No measurement is discarded. Size shrinks for 406 files, is equal
for 1, and grows for 105, with maximum growth of 4,772 bytes or 3.343%.
Five of the 16 source aggregates grow. These results establish the stated
mean-based target on the declared datasets and machine, not a guarantee for
every replay, collection, or machine. Family labels are heuristic; exact
file/event hashes cannot identify altered recordings of the same game.

Candidate 8 is frozen with four-symbol presence-encoder groups and independent
main-loop states. It passes full correctness checks and preserves candidate 7's
packets on all 1,929 files and pooled archives at four levels. Its fixed
outside-original gate passes on all 1,408 files from 22 source groups:
compression throughput improves 12.70% / 12.87% with one/eight workers, and
elapsed time falls 11.27% / 11.40%. Output is 0.161% smaller in aggregate;
decoding throughput improves 1.01% / 1.17%. All 27 replacement repetitions are
retained in the [pooled report](candidate8-awake1-outside521-replication-summary.json),
and all three sleep guards pass. Output
shrinks for 1,154 files, is equal for 3, and grows for 251 (maximum 7,282 bytes
or 2.447%). These files have influenced development and are not independent
validation. The full 1,929-file development gate also passes: compression
throughput improves 12.25% / 11.72%, elapsed time falls 10.91% / 10.49%, total
output shrinks 0.183%, and decoding improves 0.89% / 0.96%. All 27 repetitions
and launch variation are retained in the
[full-development report](candidate8-awake1-development-replication-summary.json).

An earlier attempt was stopped during full-development launch 2 after a
937-second host sleep inflated baseline decode repetition 2 to 941.52 seconds.
The AC/mode monitor did not detect sleep. The [incident record](candidate8-sleep-incident.json)
preserves the independent power-log evidence and hashes every original report,
including the contaminated sample. The attempt cannot establish the goal.
A complete replacement sequence with explicit sleep prevention and detection
has passed both development gates and independent validation under a
[new protocol](candidate8-awake1-replication-protocol.json)
with the same frozen build, profile, baseline and datasets;
old samples are not mixed with new measurements or adjusted. Independent
validation used the same frozen build after both development gates passed:
[holdout start record](candidate8-awake1-holdout-start.json).
These files were excluded from tuning and compiler training; they are now
consumed for any future tuning and must not be reused as a fresh holdout.
All 19 guard/protocol tests and a live macOS
smoke check passed: [guard validation](awake-validation.json). Both later stages
were confirmed unable to start without new prerequisite results:
[gate preflight](candidate8-awake1-gate-preflight.json). All nine production
sleep guards passed, and the scoped sleep-prevention processes exited when
qualification finished. Earlier failed candidates remain part of the history below.

Candidate7 is frozen with dictionary index-buffer reuse and passes full
correctness checks, but **fails its outside-original development gate**:
compression throughput improves9.68% /10.08% with one/eight workers, below the
single-worker target. Elapsed compression time falls8.83% /9.16%; total output
is0.161% smaller and decoding throughput improves0.93% /0.99%. All27 repetitions
are retained in the [pooled report](candidate7-outside521-replication-summary.json).
The full1,929-file qualification and reserved512-file holdout were not run.
That candidate left the holdout from16 further source families unused. No fourth launch
was added, and the optimization goal remained unmet at that stage.

The preceding frozen candidate (6) **fails independent validation**: +8.24%
compression throughput with one worker and +11.52% with eight, with 0.204%
smaller total output and faster decoding. All 27 predeclared repetitions are
retained in the [independent report](candidate6-holdout-replication-summary.json).
The one-worker target remains unmet. Its 1,417-file development result
(+10.64% / +11.00%) did not fully generalize. Both failed independent
validations remain preserved below; consumed holdouts become development data.

## Data separation

The original 521 files are development data. An additional 384 files, 64 from
each of six other source collections, were added for tuning. The combined
905-file development set contains 2,679,843,261 original bytes.

The first reserved 512-file holdout contains 1,583,404,781 bytes from eight source
collections excluded from tuning and compiler-profile training. Sampling uses
the smallest SHA-1 hashes of member paths, independent of codec measurements.
SHA-256 deduplication excludes overlap with both development sets and within
the holdout. Its [protocol](holdout-protocol.json) was recorded before candidate
measurements. Source archives had previously been decoded/recompressed during
migration; these samples are new to this optimization effort, rather than
globally unseen. Exact deduplication cannot exclude related recordings of a game.

The inventories record source archive hashes and selected input hashes:
[additional development](development-inventory.json), [holdout](holdout-inventory.json).
No replay data or filenames are embedded in these inventories.
The [inventory audit](leakage-audit.json) confirms that all 64 compiler-training
inputs are in development and that neither development nor training overlaps
the 512 holdout hashes. `leakage_audit.py` reproduces this check.

## First frozen candidate

The baseline is the profile-guided release at
`3d39721ef0e696d7a7d23c86c4805b37b45685e8`. Both libraries use Apple Clang 17,
`-O3`, fresh/source-matched PGO, and `-Wl,-no_deduplicate`. The candidate profile
uses the same 64 original development inputs as the baseline. This comparison
does not reuse gains over the older `888e0be` release.

Changes include ten-bit entropy tables, compact presence-model headers,
implicit unused semantic trees, constant channels, less level-3 match searching
on transformed planes, direct literal buffers, an equivalent prefix-only
prediction probe, and deferred exception-context allocation. These are general
codec rules; no file names, hashes, or source-collection IDs select behavior.

The frozen candidate's sources, profile, and binary hashes are in
[build-info.json](build-info.json). All 905 packed fingerprints match the
corresponding pre-integration candidate after cleanup and profile regeneration.

## Controlled development result

Apple M4 Max; high power mode on battery, with no observed mode/source change.
Inputs are resident in RAM. One replay is encoded per archive; eight workers
process independent archives, each with one internal archive worker. Both
libraries link the same benchmark-driver object. Each setting has one warmup
and nine randomized interleaved measured repetitions. I/O, hashing, and byte
verification are outside the timers. All repetitions and outliers are retained.

Rates are original decimal MB divided by arithmetic mean elapsed seconds:

| Workers | Encode before | Encode after | Throughput gain | Encode time reduction | Decode gain |
|---|---:|---:|---:|---:|---:|
| 1 | 382.3 MB/s | 413.9 MB/s | 8.26% | 7.63% | 0.64% |
| 8 | 2768.1 MB/s | 2980.7 MB/s | 7.68% | 7.13% | 0.39% |

Packed output totals 108,703,468 bytes versus 108,752,842 (0.0454% smaller).
The mean per-file percentage change is -0.3319%; 687 files shrink and 218 grow.
The largest increases are 3.0103% and 6,905 bytes.

The original 521 files shrink 0.1655% in aggregate. The newer 384 grow 0.0721%
(55,007,609 versus 54,967,978 bytes), despite a mean per-file change of -0.1460%.
Four of their six source groups grow in aggregate. This is evidence against
treating the original corpus's result as a generalization guarantee.

[Raw samples](development-905.json) include exact reconstruction checks,
stable packed fingerprints, input/source/binary hashes, and power monitoring.
[The summary](development-summary.json) includes each source's size results and
descriptive paired-repeat bootstrap intervals. Those intervals capture timing
variation within this run, not uncertainty on unseen source collections.
Many candidates were screened on development data; none of these are held-out
estimates. The 512-file holdout remains unused because this candidate failed
the development target.

## Correctness

Ordinary, scalar, AddressSanitizer/UndefinedBehaviorSanitizer, and PGO release
checks pass, as does `make check-format`. The suites include 5,065 custom-codec
checks, 16,905,344 probability-model/CDF checks, 111,137 record-codec checks,
2,463,583 predictor checks, and 92 library-API checks. Integration tests cover
the frozen v1 archive, four levels, exact extraction, and invalid inputs.

New independent fixtures exercise ten-bit channels, constant channels, implicit
semantic trees, every presence-header form, truncations and invalid labels.
Prediction-probe tests compare pairing against a direct ordinal-search oracle
and check immutability and repeated-call state cleanup. Existing archives remain
readable; older readers cannot decode the new payload tags.

`summarize.py` reproduces the result summary. `pooled_check.py` was prepared for
additional real-CLI validation; its completed results for candidate 3 appear
below.

## Second frozen candidate: target still unmet

The second candidate adds uniform-run presence counting, charges actual compact
header bytes when selecting presence models, reuses bounded encoder scratch
buffers, and uses a direct dictionary index for fields with two varying bytes.
It restores two recent-distance probes for level-3 transformed planes. No input
names, content hashes, or collection IDs enter these decisions.

The five-repeat development screen measured 10.71% faster single-worker encoding.
The integrated, freshly profiled build's nine-repeat result did not reproduce
that gain. All 905 packed fingerprints remain identical to the screened candidate.

| Workers | Encode before | Encode after | Throughput gain | Encode time reduction | Decode gain |
|---|---:|---:|---:|---:|---:|
| 1 | 384.5 MB/s | 411.3 MB/s | 6.99% | 6.53% | 1.09% |
| 8 | 1422.2 MB/s | 1562.0 MB/s | 9.82% | 8.95% | 0.32% |

The battery fell from 4% to 2% during validation. The eight-worker absolute
throughput was roughly half that of the earlier run, although the monitor
observed no configured power-mode or source change. A later `pmset -g therm`
query reported no recorded thermal/performance warning. These observations do
not establish the cause of the change. The measurements are retained in full;
the next comparison requires stable power. Neither worker count's observed
average meets the 10% throughput target.

Output is 108,569,081 bytes, 0.1690% smaller than baseline. The original 521
inputs shrink 0.2605% in aggregate; the newer 384 shrink 0.0794%. Overall 758
files shrink and 147 grow, with maximum increases of 2.4466% and 5,612 bytes.
Two of the six additional source groups still grow in aggregate.

[Build provenance](candidate2-build-info.json),
[all measurements](candidate2-development-905.json), and
[size/source/timing summary](candidate2-development-summary.json) are retained.
The untouched holdout has not been consumed by either candidate.

Ordinary, scalar, sanitizer, and profiled release suites pass. They now include
6,233 custom-codec checks and 111,185 record-codec checks; the other counts above
are unchanged. An independent bit-by-bit histogram oracle covers uniform runs,
reference masks, and tail padding. Tests also cover nested scratch leases,
reuse, exception cleanup, the retention cap, all six varying-byte pairs, and
127/255/256/257-value dictionary boundaries. Dictionary lookup tables are cleared
before allocations or callbacks can occur.

At that critically low charge, further timing validation required AC power. The benchmark driver's
`--require-ac-power` guard checks this before loading replay data or launching
workers; its existing monitor rejects a source or configured-mode change during
the run. The guard was checked on the critically low battery and rejected the
run without producing a result file. The frozen candidate's source, profile,
and binary hashes were rechecked and match its provenance.

Once AC power is available, retain the earlier measurements and run the same
frozen candidate into a new result file:

```sh
python3 benchmarks/2026-11-10-encode-speed/benchmark.py \
  --require-ac-power --allow-byte-changes \
  --baseline build/goal-2026-11-12/baseline/runner \
  --candidate build/goal-2026-11-12/candidate2/runner \
  --baseline-commit 3d39721ef0e696d7a7d23c86c4805b37b45685e8 \
  --manifest build/goal-2026-11-12/development-all.txt \
  --levels 3 --threads 1,8 --repeats 9 \
  --output benchmarks/2026-11-12-generalization/candidate2-ac-development-905.json
```

The monitor records the active power mode rather than changing it. Evaluate the
holdout only after a frozen candidate qualifies on development data.

## Same frozen candidate on AC power

After power was connected and the battery reached 36%, the exact same frozen
candidate completed the nine-repeat comparison above. Its hashes were checked
before the run; source, binary, and input hashes remained unchanged throughout.
The monitor recorded AC power and mode 2 throughout, with no changes.

| Workers | Encode before | Encode after | Throughput gain | Encode time reduction | Decode gain |
|---|---:|---:|---:|---:|---:|
| 1 | 377.1 MB/s | 410.8 MB/s | 8.94% | 8.21% | 1.36% |
| 8 | 2762.3 MB/s | 3001.4 MB/s | 8.66% | 7.97% | 0.70% |

These observed averages remain below the target. Packed sizes are unchanged.
[Raw AC measurements](candidate2-ac-development-905.json) and their
[summary](candidate2-ac-development-summary.json) are retained alongside both
earlier runs. Further development continues without consuming the holdout.


## Third frozen candidate: controlled development result

Candidate 3 changes the context histogram aggregation to sum each symbol across
history buckets before storing it once. Tree construction also skips leaves
that cannot split and stops evaluating further splits at the existing 16-leaf
limit. These changes preserve the second candidate's exact compressed output
on all 905 development files. The code, profile, and executable were frozen
before the first holdout compression: [build provenance](candidate3-build-info.json).

Ordinary, scalar, ASan/UBSan, and profiled release suites pass, as does format
checking. Three constructed 16-leaf cases increase the custom-codec checks to
6,236; other suite counts remain unchanged. The fresh compiler profile uses the
same 64 development inputs. The [overlap audit](candidate3-leakage-audit.json)
confirms that no holdout hashes occur in development or profile training.

The Mac was on battery at 100% before validation. This choice was recorded
before measurement; the low-charge condition that prompted the earlier AC-only
guard no longer applied. Both binaries ran under the same monitored power
source and mode 2 throughout. The driver observed no change, and all source,
binary, input, and reconstruction checks passed. Earlier exploratory screens
had no power monitor and are not the reported performance estimate.

| Workers | Encode before | Encode after | Throughput gain | Encode time reduction | Decode gain |
|---|---:|---:|---:|---:|---:|
| 1 | 360.0 MB/s | 399.7 MB/s | 11.03% | 9.93% | 0.73% |
| 8 | 2673.0 MB/s | 2977.7 MB/s | 11.40% | 10.23% | 0.52% |

“Faster” here means increased throughput; elapsed-time reductions are reported
separately. These are arithmetic means over all nine repetitions. The single
worker's descriptive throughput interval is 8.42–14.28%, so the point estimate
is not a guarantee that every repeat exceeds 10%. The eight-worker decode
interval is -0.08–0.89%; its observed average improves slightly.

Output remains 108,569,081 bytes, 0.1690% smaller than baseline, including a
0.0794% aggregate reduction on the additional 384 development files. Individual
files and two of those six source groups can grow, as recorded in the summary.
[Raw measurements](candidate3-development-905.json) and the
[complete summary](candidate3-development-summary.json) contain the samples,
size distribution, and source breakdown. This is a development result after
many experiments; independent holdout validation is still required.


## Independent holdout: target not reproduced

The same frozen candidate was evaluated once, without further tuning, on 512
files from eight collections excluded from development and compiler training.
All nine repetitions completed with unchanged battery power and mode 2, and
all input, source, binary, and reconstruction checks passed.

| Workers | Encode before | Encode after | Throughput gain | Encode time reduction | Decode gain |
|---|---:|---:|---:|---:|---:|
| 1 | 368.9 MB/s | 387.3 MB/s | 5.00% | 4.76% | -0.18% |
| 8 | 2544.3 MB/s | 2746.7 MB/s | 7.95% | 7.37% | -0.06% |

Packed output totals 65,440,287 bytes versus 65,563,561 (0.1880% smaller).
Of 512 files, 433 shrink, 2 tie, and 77 grow. The largest increases are
1.9309% and 7,282 bytes. One source group grows 0.0175% in aggregate.
The decode differences are small and their descriptive intervals include zero;
the observed averages nevertheless do not meet the no-slowdown condition.
None of the three requested alternatives is established on this holdout.

[Raw holdout measurements](candidate3-holdout-512.json) and the
[summary with source sizes](candidate3-holdout-summary.json) remain the independent
estimate for this frozen build. Any later work informed by this result must
treat these inputs as validation/development data. A new holdout from other
collections must be reserved before further tuning; the original protocol and
failed result are preserved rather than overwritten.

Real CLI pooled archives containing all 905 development files also pass exact
member-hash verification for both baseline and candidate at levels 1, 3, 7, and
9. This is a correctness check, not another speed benchmark:
[pooled validation](candidate3-pooled-check.json). Source archives were untouched.


## Further development and a new untouched holdout

The 512 failed-validation inputs are now explicitly part of the 1,417-file
expanded development set (4,263,248,042 original bytes), documented in the
[development inventory](development-round2-inventory.json). Their original
independent result remains preserved for candidate 3.

Before any further optimization, eight unused source families were selected
by a fixed hash ordering: Shine 2022, fall 2019 Purdue weeklies, HTL 3,
Abbey 52, Full House 2025, Battle of BC 8, Mango's birthday, and LACS 5.
The [second protocol](holdout-round2-protocol.json) records the eligible sources,
family exclusions, seed, and selection rule before extraction.

This reserves another 512 files, with no SHA-256 duplicates against all 1,417
known inputs or within the new set. Only baseline extraction and hashing have
been performed; no candidate compression, tuning, or profiling has used them.
See the [new inventory](holdout-round2-inventory.json) and
[overlap audit](round2-leakage-audit.json). Future final validation requires a
new frozen candidate; these files must remain excluded from further tuning.

The first expanded-development screen rejected the cursor-based parser and
context-loop probes: all were slower than unchanged candidate 3. Training the
compiler on 16 files from each of 17 development sources (272 total) improved
encoding only 0.55% relative to candidate 3 and slowed decoding 0.76% in this
three-repeat screen. No new variant was adopted. The
[training selection](balanced-round2-training-plan.json),
[complete screen](round2-first-screen.json), and
[arithmetic-mean summary](round2-first-screen-summary.json) preserve these results.
The screen's driver changes only untimed input loading; final qualification
continues to require the original frozen benchmark driver.

A separate five-repeat diagnostic run adds per-file clock reads to compare
the 17 source collections. Candidate 3's encoding gains range from 0.86% to
11.68%, with 5.08% pooled and 5.87% when source speed ratios receive equal
geometric weight. This run does not establish the target. Its different
instrumentation and the variation between runs preclude treating it as a
replacement for canonical validation. The source labels are reporting groups,
not codec inputs or switches. See the [source diagnostics](candidate3-development-1417-source-diagnostics.json)
and [all five samples](candidate3-development-1417-source-diagnostics-raw.json).
The new 512-file holdout remains unused.

A second three-repeat screen rejected smaller encoder tables, four-state
loop unrolling, and a shortcut for deterministic presence tables: they were
0.13%, 5.92%, and 2.59% slower than candidate 3, respectively. Short-record
block copies improved encoding only 0.42%, insufficient to justify adoption
given the observed run-to-run variation. All 1,417 compressed outputs exactly
matched candidate 3. [Raw results](round2-packed-copy-screen.json) and
[arithmetic means](round2-packed-copy-screen-summary.json) include every sample.

The third screen tested mask-buffer changes. Direct mask writes with deferred
decoder labels improved encoding 3.90% and decoding 0.60% relative to candidate 3;
all outputs remained identical. The separate deferred-mask, direct-write-only,
and uninitialized-value-buffer variants are retained in the
[complete screen](round2-mask-buffers-screen.json) and
[summary](round2-mask-buffers-screen-summary.json). These three-repeat results
select a candidate for longer validation; they do not establish the goal.

Candidate 4 integrates the direct-write/deferred-label variant and fixes scratch
reuse: each new lease clears the previous logical size before growing its
allocation, preventing relocation of potentially uninitialized entries. A
tracked-relocation regression test fails before the fix and passes afterward.
Normal, scalar, and ASAN/UBSAN suites pass, including 6,289 custom codec checks,
the frozen v1 archive fixture, four compression levels, exact reconstruction,
and extraction safeguards. The rebuilt profiled release also passes all checks.
[Candidate 4's frozen build](candidate4-build-info.json) and
[overlap audit](candidate4-leakage-audit.json) record the exact sources, binaries,
profile, and reserved holdout inventory. Nine-repeat validation on all 1,417
development files completed under unchanged AC power and mode 2:

| Workers | Encode before | Encode after | Throughput gain | Encode time reduction | Decode gain |
|---|---:|---:|---:|---:|---:|
| 1 | 361.6 MB/s | 398.6 MB/s | 10.25% | 9.30% | 1.04% |
| 8 | 2820.3 MB/s | 3086.2 MB/s | 9.43% | 8.61% | 0.67% |

The single-worker point estimate barely exceeds 10%, with a descriptive repeat
interval of 8.92–11.70%; the eight-worker mean falls short. This is insufficient
evidence of the requested general improvement, so the fresh holdout remains
unused. All 1,417 candidate outputs exactly match candidate 3, totaling
174,009,368 bytes versus 174,316,403 for baseline (0.1761% smaller). All input,
source, binary, power, and reconstruction checks passed. See the
[raw nine-repeat run](candidate4-development-1417.json) and
[complete summary](candidate4-development-summary.json). Pooled CLI verification
on all 1,417 files also passes for baseline and candidate 4 at levels 1, 3, 7,
and 9, with every reconstructed member hash matching the source:
[pooled correctness results](candidate4-pooled-check.json).

Follow-up buffer probes were rejected. Combining uninitialized exception-value
storage with an exact-match stop in the reference search improved the
eight-worker screen by 2.92% over candidate 4, but regressed the single-worker
screen by 6.47%. The individual changes also failed to improve eight-worker
encoding. Both complete five-repeat comparisons are retained:
[eight workers](round2-mask-finish-t8-screen-summary.json) and
[one worker](round2-mask-finish-t1-screen-summary.json).

A stronger [event-stream overlap audit](round2-gameplay-overlap-audit.json) used
only the frozen baseline parser, excluding raw-length headers, metadata, and
unparsed tails. All 512 held-out streams were parseable, with no matching event
stream in development or elsewhere in the holdout. Of 1,417 development files,
1,416 had usable events; the remaining file was empty and remains covered by
the original full-file hash audit. Every hash match would require an exact byte
comparison before being called a duplicate. This additional audit does not
identify versions of a recording whose actual event bytes differ, and performs
no candidate compression or timing on the holdout.

Reading dictionary values directly from records also failed to improve the
expanded development workload. Three variants regressed single-worker encoding
2.46–3.37% relative to candidate 4, while preserving every compressed byte.
They remain unintegrated; the [complete screen](round2-record-loads-screen.json)
and [means](round2-record-loads-screen-summary.json) retain all three repetitions.

A [development-only lookup count](round2-dictionary-cache-statistics.json)
found 13,248,511 opportunities to reuse the second-most-recent distinct value
among 68,988,397 general four-byte dictionary lookups (19.20%). A cache confined
to that path improved mean single-worker compression throughput 2.37% over
candidate 4 in a three-repeat screen; extending it to all dictionary lookups
regressed throughput 1.70%. Both preserved every compressed byte and passed
329 synthetic archive-equality and reconstruction cases. These are exploratory
results, with no integration or independent-validation claim:
[all samples](round2-dictionary-cache-screen.json) and
[means](round2-dictionary-cache-screen-summary.json).

The follow-up [one/eight-worker comparison](round2-dictionary-pairblock-screen-summary.json)
rejected the narrow cache too: mean compression throughput fell 2.05% and
2.53%, respectively, relative to candidate 4. A SIMD extension processed
whole batches containing only the two cached values; a separate
[counting pass](round2-dictionary-block-statistics.json) found that condition
in 14.11% of otherwise nonuniform batches. That extension gained 1.10% with
one worker but lost 1.78% with eight. Neither change was integrated. Both
passed the expanded 389-case synthetic checks and preserved all development
archive bytes. The [complete follow-up samples](round2-dictionary-pairblock-screen.json)
are retained, including the initial cache improvement's failure to reproduce.

A subsequent presence-mask change captures the encoder state and CDF start
before writing renormalization bytes, avoiding state/table reloads in generated
code. It preserves the original loop and arithmetic. The single-worker screen
improved throughput 1.89% over candidate 4; a five-repeat eight-worker screen
improved it 1.44%. A separate version that bounded renormalization to two checks
gained 2.28% with one worker but regressed 1.26% with eight, and was rejected.
All repetitions, including the slower final samples, are retained:
[one worker](round2-presence-renorm-screen-summary.json) and
[eight workers](round2-presence-renorm-t8-screen-summary.json).
Both versions passed 1,080 exact presence-packet/reconstruction cases. The
bounded-loop arithmetic additionally passed 7,667,703 sanitizer-checked state,
frequency, and CDF boundary cases. The simpler loop-preserving version is now
integrated for full validation; these exploratory results do not establish
the performance goal or generalization to the reserved 512 files.

Candidate 5 freezes that loop-preserving change on top of candidate 4.
[Build provenance](candidate5-build-info.json) records core, test, profile,
driver, and executable hashes, including the unchanged reserved-holdout
inventory hash. Normal, scalar, ASan/UBSan, and profiled release suites and
format checks all pass; the [validation record](candidate5-validation.json)
retains counts and log hashes. Its [input audit](candidate5-leakage-audit.json)
again confirms zero development/training overlap with the reserved holdout.
Controlled nine-repeat validation on all 1,417 development files remains below
target:

| Workers | Encode throughput gain | Encode elapsed-time reduction | Decode throughput gain |
|---:|---:|---:|---:|
| 1 | 9.1333% | 8.3690% | 0.5753% |
| 8 | 10.2361% | 9.2856% | 0.5888% |

All samples are retained. The descriptive encode intervals are 6.68–11.51%
and 9.48–10.92%, respectively; these describe repeat variation, not uncertainty
over unseen sources. The single-worker average misses the target, so the new
512-file holdout remains unused. Packed output totals 174,009,368 bytes versus
174,316,403 baseline bytes, a 0.1761% reduction. Every candidate archive matches
candidate 4's compressed fingerprint exactly at both worker counts. AC power
and mode 2 remained unchanged, and input, source, binary, and reconstruction
guards passed. See the [full run](candidate5-development-1417.json) and
[summary with source sizes](candidate5-development-summary.json). Differences
between candidate 4 and candidate 5 in separate controlled runs do not isolate
the effect of the latest code change.

Pooled CLI verification also passes on all 1,417 development files at levels
1, 3, 7, and 9. Every reconstructed member hash matches its source, and every
complete archive matches candidate 4 byte for byte:
[pooled results](candidate5-pooled-check.json).

Before further code changes, a [fixed replication protocol](candidate5-replication-protocol.json)
adds exactly two independent process launches to the completed run, for three
launches and 27 repetitions total. The existing nonqualifying run is included
in the pooled mean; no samples or completed launches will be dropped. This
plan was chosen after seeing the initial result and addresses timing/process
variation, not source generalization. If that fixed aggregate meets the target
at both worker counts, final validation uses three nine-repeat launches of the
same frozen candidate on the reserved 512 files. Those files remain unused
while development replication runs.

The fixed replication is complete, and its gate **fails**:

| Workers | Pooled encode throughput gain | Encode elapsed-time reduction | Decode throughput gain |
|---:|---:|---:|---:|
| 1 | 9.6512% | 8.8017% | 1.1686% |
| 8 | 10.9803% | 9.8940% | 1.1396% |

The three single-worker launch gains were 9.1333%, 11.0703%, and 8.8017%; all
27 repetitions enter the pooled result. Selecting only the second launch would
give a misleading pass. The descriptive hierarchical intervals for encoding
are 8.25–11.11% and 9.85–12.47%, respectively. These remain development results.
No fourth launch was added, and the reserved 512 files were not evaluated.
The [complete aggregate](candidate5-development-replication-summary.json)
records every sample and source size; the two additional raw runs are
[launch 2](candidate5-development-1417-launch2.json) and
[launch 3](candidate5-development-1417-launch3.json).

The next [compiler-profile experiment](balanced272-candidate5-plan.json) keeps
candidate 5's core sources unchanged and reuses the previously declared
272-file training selection: 16 files from each of 17 development sources.
This is a new profile for the current code; the earlier balanced-profile trial
on candidate 3 remains recorded above. No held-out inputs enter training.
That experiment passed the release suite and preserved all compressed bytes,
but was slower than candidate 5 by 2.63% with one worker and 1.68% with eight.
It was rejected: [raw samples](round2-balanced272-candidate5-screen.json),
[means](round2-balanced272-candidate5-screen-summary.json), and
[training-overlap audit](balanced272-candidate5-leakage-audit.json).

A [native CPU-tuning probe](native-tuning-candidate5-plan.json) retained the
compiler's Apple M1 instruction target while selecting Apple M3 scheduling
through `-mtune=native`. Fresh training used the same 64 inputs as candidate 5.
All five core object text sections and the linked benchmark runner's text were
byte-identical to candidate 5, so no timing experiment was performed:
[compiled-code audit](native-tuning-candidate5-code-audit.json).

An encoder-only packed presence table reduces each table from 128 to 64 bytes.
It preserves positive frequencies and CDF starts exactly, uses an invalid-pair
sentinel for zero frequencies, and leaves the decoder unchanged. Sanitizer
checks cover 139,264 tables and 2,228,224 symbol entries, including zero and
full-frequency cases; 1,080 encoded-packet and reconstruction comparisons also
pass. Its initial full-development screen gains 0.87% with one worker and 1.16%
with eight over candidate 5. It remains an unintegrated prototype:
[raw samples](round2-presence-packed32-screen.json) and
[means](round2-presence-packed32-screen-summary.json).

A [separate counting pass](round2-presence-reuse-statistics.json) found that
166,525,230 of 278,637,863 presence symbols (59.76%) use exactly the same model
and symbol as the immediately preceding entry within a packet. This is a
development-only count, not a timing measurement. A one-entry cache is being
compared both alone and with the packed tables; no reserved inputs are used.
The completed comparison rejected both cache versions. The ordinary-table
cache gained only 0.23% with one worker and lost 3.14% with eight; the combined
cache/packed version lost 0.38% and 2.06%. All output bytes still matched:
[raw samples](round2-presence-cache-screen.json) and
[means](round2-presence-cache-screen-summary.json). The packed-table change
alone is being taken through full validation as candidate 6, with permanent
frequency/CDF boundary coverage added to the model tests.

Candidate 6 is now frozen after passing normal, scalar, ASAN/UBSAN, fresh
64-file compiler training, optimized release, and format checks. Its permanent
model suite checks 19,133,569 cases, including 2,228,224 packed-table symbols.
The full 1,417-file pooled CLI check passes at levels 1, 3, 7, and 9, with
identical whole archives to candidate 5: [validation](candidate6-validation.json),
[pooled archives](candidate6-pooled-check.json), and
[split audit](candidate6-leakage-audit.json). Before any canonical timing, the
[protocol](candidate6-replication-protocol.json) fixes exactly three launches
of nine repetitions, retaining all 27 samples. Both one/eight-worker averages
must meet the target before the unchanged candidate can access the reserved
512-file holdout. That final validation would also use all three launches.

All 27 candidate-6 development repetitions pass the recorded mean-based gate:
compression throughput +10.64% / +11.00%, elapsed time -9.62% / -9.91%,
decoding throughput +1.16% / +1.39%, and total packed bytes -0.176%.
The launch gains are 10.65%, 10.12%, 11.16% with one worker and 10.38%,
11.98%, 10.65% with eight. Every packed fingerprint matches candidate 5.
The [pooled report](candidate6-development-replication-summary.json) retains
all samples; its descriptive timing intervals are not source-population
confidence intervals. These are development results, so the overall goal
is still unproven. The same frozen candidate now proceeds to the three
predeclared independent launches on the reserved 512-file set.

Candidate 6 also **fails independent validation**. The full 27-repetition
result on the second reserved set is +8.24% / +11.52% compression throughput
(-7.62% / -10.33% elapsed time), +0.75% / +0.78% decoding throughput, and
-0.204% total bytes. Every source group shrinks in aggregate, while 84 of
512 individual files grow (maximum 1.864% or 4,000 bytes). All exact round
trips and frozen-input/source/binary/power guards pass.
[All samples and means](candidate6-holdout-replication-summary.json) remain
preserved. The one-worker target fails, so the goal remains unmet.

The [retirement record](holdout-round2-retirement.json) makes these files
development data for subsequent work. Development now contains 1,929 files
from 25 collections. A separate 1,408-file development manifest excludes
the original 521; the next candidate must satisfy the speed/size/decoding
gate both on that set and on full development before another independent
validation. This additional gate prevents the original benchmark collection
from carrying the average. A new holdout will use more source families,
selected and frozen before further optimization. No fourth unchanged-candidate
qualification launch is added.

The next [reserved split](holdout-round3-protocol.json) contains 512 files,
32 each from 16 previously unused source families. Selection was fixed by
source-name hashes before extracting members with the original baseline
decoder. No candidate has encoded, timed, trained on, or profiled these files.
The [full-file audit](round3-leakage-audit.json) finds no overlap with all
1,929 development files or compiler training. The
[gameplay-stream audit](round3-gameplay-overlap-audit.json) checks all 2,441
inputs and finds no duplicates among 2,438 usable event streams; one development
and two reserved inputs fall back to full-file hashing. This cannot exclude
recordings with altered event bytes.

Encoding-only sampling on the 1,408 development files outside the original 521
identifies dictionary construction, presence encoding, and match searching
as the main remaining costs: [profile provenance](round3-outside521-profile-provenance.json)
and [leaf samples](round3-outside521-profile-summary.json). Sampling is diagnostic,
not qualification timing. A [400-input compiler-training plan](balanced-round3-training-plan.json)
uses 16 inputs from each of 25 development collections, with no new holdout
overlap; its unchanged-code performance is being evaluated separately.

The first 400-input profile screen on all 1,408 development files outside
the original 521 shows +0.43% / +1.70% compression throughput relative to
the unchanged 64-input candidate 6. Decoding changes -0.42% / +0.13% relative
to candidate 6, while remaining +0.15% / +1.15% above the original release.
All compressed fingerprints match. These small exploratory gains are not
a qualification result: [raw samples](round3-balanced400-outside521-screen.json)
and [all means](round3-balanced400-outside521-screen-summary.json). The broader
profile will be a matched control for the next general table-access experiments.

The presence-table screen retains all three variants and failures:
[raw samples](round3-presence-outside521-screen.json),
[means](round3-presence-outside521-screen-summary.json), and
[prototype checks](round3-presence-prototype-validation.json). Relative to
the matched 400-input profile, sentinel-only changes compression -0.99% /
+0.52%; pair-only +0.65% / +1.11% but single-worker decoding falls below the
original baseline. Neither is selected alone. The combination gives +0.54% /
+2.25% compression, with decoding still +0.12% / +0.74% above the original
baseline. It remains a modest exploratory component, not a qualifying candidate.

Packed dictionary lookup experiments pass their exact-output and lifecycle
checks but fail performance: [validation](round3-dictionary-prototype-validation.json),
[raw screen](round3-dictionary-outside521-screen.json), and
[all means](round3-dictionary-outside521-screen-summary.json). Relative to
the matched 400-input control, reused packed slots change compression
-2.59% / -2.18%. Removing the now-unused per-field table changes it
-1.20% / +0.31%; combining that with the presence changes gives -1.53% /
-1.46% and slightly slower eight-worker decoding than the original baseline.
All are rejected; no dictionary experiment is integrated.

Direct-frequency and frequency-bias packing pass independent model, arithmetic,
exact-packet, and sanitizer checks, but do not pass the exploratory performance
gates: [validation](round3-presence-direct-prototype-validation.json),
[raw samples](round3-presence-direct-outside521-screen.json), and
[all means](round3-presence-direct-outside521-screen-summary.json). Against
the matched 400-input control, direct-frequency packing changes encoding
-0.35% / +1.96%; adding the algebraically equivalent multiply-add state update
gives +0.70% / +3.11%; bias packing gives -0.20% / +1.02%. The multiply-add
variant decodes -0.26% / -0.001% below the original baseline. No variant is
integrated or qualified. All outputs match frozen candidate 6 exactly. An
empty-constructor proposal was discarded before timing because the existing
allocator already avoids zero initialization, and all generated core instruction
bytes were unchanged: [code audit](round3-empty-constructor-audit.json).

Decode-only sampling on all 1,408 development files outside the original 521
finds presence decoding at 26.75% and ten-bit context decoding at 11.12% of
listed leaf samples: [provenance](round3-outside521-decode-profile-provenance.json)
and [samples](round3-outside521-decode-profile-summary.json). Each input was
encoded and verified byte-for-byte before sampling. These fractions include
some allocation overhead between passes and are diagnostic, not speed gains.

Sixteen-byte NEON match-length scans preserve every compressed fingerprint and
pass 3,360,752 independent prefix comparisons plus 6,289 codec checks in each
normal, scalar, and sanitizer build: [validation](round3-match16-prototype-validation.json).
Both are rejected by the all-1,408-file development screen. Starting directly
with sixteen bytes changes encoding -1.21% / -1.06% against the matched
400-input control; first checking eight bytes changes it -0.67% / +0.26%.
The stronger absolute gains against the original release in this particular
screen do not establish a benefit from either change:
[raw samples](round3-match16-outside521-screen.json),
[all means](round3-match16-outside521-screen-summary.json).

Decoder-only local-state and packed-next-model experiments also fail to
improve the matched control consistently: [checks](round3-decoder-prototype-validation.json),
[raw samples](round3-decoder-outside521-screen.json),
[means](round3-decoder-outside521-screen-summary.json). Local presence state
changes decoding +0.23% / +0.62% but encoding -0.66% / -1.55%. Packing the
next model into unused ten-bit decoder-entry bits changes decoding -0.27% /
-0.15%; its incidental encoding changes +0.17% / +1.61% do not establish the
intended decoder benefit. Neither is integrated. Exact compressed output and
all input, binary, and power guards pass across all 1,408 development files.

A diagnostic count rules out skipping deterministic, unreferenced presence
tails: only 20 of 290,968,926 decoded nibbles would be skipped across all
1,408 outside-original development inputs. There are only 484,032
full-frequency nibbles overall (0.166%). The proposed shortcut is not
implemented or benchmarked: [occurrence counts](round3-presence-decode-tail-statistics.json).
All 1,408 source round trips passed during this diagnostic.

Carrying the reference-byte window across presence symbols is promising in
the outside-original development screen: [checks](round3-presence-windows-prototype-validation.json),
[raw samples](round3-presence-windows-outside521-screen.json),
[means](round3-presence-windows-outside521-screen-summary.json). The combined
forward decoder and backward encoder windows change encoding +2.38% / +0.53%
and decoding +0.85% / +0.65% against the matched 400-input control. Every
compressed fingerprint is unchanged. The decoder-only version changes
encoding +1.80% / -0.86% and decoding +0.54% / +0.52%, so the combined version
is selected for a full 1,929-file development screen. These are exploratory
results; eight-worker encoding was only +8.36% against the original release
in this run, and no qualification or new holdout has been performed.

Round3 qualification uses separate helpers so the candidate6 protocol remains
immutable: [cohort guards](round3_cohorts.py), [protocol declaration](declare_round3_protocol.py),
[runner](replicate_round3.py), and [pooled summary](summarize_replicates_round3.py).
The ordered gates are all1,408 outside-original development files, all1,929
development files, then the512 reserved files. Each gets exactly three launches
of nine repeats at one/eight workers. Both development cohorts must independently
pass before the runner permits holdout timing. Tests reject incorrect cohort
roles/counts and missing/failed prerequisites before any benchmark launch;
pooled aggregation is checked against an independent arithmetic fixture:
[helper checks](round3-validation-helper-checks.json). No candidate7 is frozen
and no candidate7 qualification protocol has been declared at this point.

The full1,929-file screen reverses the apparent window improvement, so the
combined window change is rejected: [raw samples](round3-presence-windows-full1929-screen.json),
[means](round3-presence-windows-full1929-screen-summary.json). Against the
matched400-input control, encoding changes -2.23% / -0.99% and decoding
+0.34% / -0.30%. Eight-worker decoding is also -0.33% below the original
release. The400-input profile itself is effectively tied with frozen64-input
candidate6 in single-worker encoding and is about3.13% slower at eight workers
in this screen. It is therefore not promoted by this result either. All
compressed fingerprints and input/binary/power guards pass. No candidate7
is frozen or qualified, no runtime change is integrated, and the new512-file
holdout remains unused. A positive outside-original exploratory result is
insufficient when the full-development comparison reverses it.

Reference/no-reference loop specialization, with the same64 compiler-training
inputs as candidate6, is rejected on all1,408 outside-original development
files: [checks](round3-presence-refspecial-prototype-validation.json),
[raw samples](round3-presence-refspecial-outside521-screen.json),
[means](round3-presence-refspecial-outside521-screen-summary.json). Encoding
changes -0.63% / -1.36% against candidate6 for coding-loop specialization,
and -0.20% / +0.97% when histogram construction is also specialized. Decoding
regresses about4.9–5.7% against candidate6. All fingerprints and guards pass;
no change is integrated or sent to final validation.

An experimental byte-presence alphabet groups eight decisions for unreferenced
full bytes, retaining the nibble coder for references and partial tails. It
uses the same binary probabilities with an18-bit joint CDF and retains legacy
modes0..5: [independent checks](round3-presence-byte18-prototype-validation.json).
The first implementation fails performance: encoding changes +0.19% / -2.56%
and decoding -2.88% / -1.52% against frozen candidate6 on all1,408 outside-original
development files. Packed size is183,629,560 bytes, only118 bytes below candidate6;
1,405 of1,408 fingerprints change, so the new format is actually exercised.
This prototype is not integrated or qualified:
[raw samples](round3-presence-byte18-outside521-screen.json),
[means](round3-presence-byte18-outside521-screen-summary.json).

The cached byte decoder and cheaper tree/escape byte model also fail the complete
1,408-file outside-original screen. Against candidate6, cached encoding changes
-2.56% / -1.79% and decoding -2.77% / -2.38% (T1/T8). The tree changes encoding
+0.15% / +1.02% but decoding -2.87% / -2.62%; it is also1.36% /1.00% slower
to decode than the original baseline. Both are rejected without consuming the
reserved holdout. The cached encoder matches all1,408 uncached byte packets,
and original/candidate6 fingerprints match their prior frozen reports.
[Correctness checks](round3-presence-byte18-next-prototype-validation.json),
[fingerprint audit](round3-presence-byte18-next-fingerprint-audit.json),
[raw samples](round3-presence-byte18-next-outside521-screen.json),
[pooled means](round3-presence-byte18-next-outside521-screen-summary.json).

Round3 freezing now requires an explicit compiler-training scheme, either the
unchanged64-input reference or the declared balanced400-input reference. Both
are checked for exact membership and exclusion of the reserved holdout;
[invalid-reference checks](round3-training-helper-checks.json) reject altered,
duplicate, wrong-count, heldout, and undeclared training references. This does
not promote a profile or declare a new candidate.

Two exact-output presence histogram changes also fail the1,408-file screen:
counting unreferenced nibbles before expanding their binary decisions changes
compression throughput -1.48% / -0.76% against candidate6; expanding the uniform
shortcut to unreferenced all-one runs changes it -0.86% / -0.58%. All packets
match frozen candidate6 exactly. Neither is integrated or sent to the holdout.
[Normal/scalar/sanitizer checks](round3-presence-histogram-prototype-validation.json),
[raw samples](round3-presence-histogram-outside521-screen.json),
[pooled means](round3-presence-histogram-outside521-screen-summary.json).

Reusing the dictionary index buffer across trials preserves every candidate6
packet and shows a small compression gain against candidate6: +0.38% / +0.20%
on1,408 outside-original files, and +1.53% / +1.03% on all1,929 development files.
Against the original baseline, the full screen shows +10.14% / +12.40% encode
throughput, unchanged candidate6 size, and +0.98% / +0.51% decode throughput.
These three-repeat screens do not establish independent qualification. The
small change is selected for full correctness checks and fixed canonical gates;
no round3 holdout result exists. [Prototype validation](round3-dictionary-index-reuse-prototype-validation.json),
[outside raw](round3-dictionary-index-reuse-outside521-screen.json),
[outside means](round3-dictionary-index-reuse-outside521-screen-summary.json),
[full raw](round3-dictionary-index-reuse-full1929-screen.json),
[full means](round3-dictionary-index-reuse-full1929-screen-summary.json).

A four-symbol presence-encoder experiment retains the existing format and
rotates four states at plane boundaries. Separating tail temporaries lets the
compiler keep all four main-loop states in registers. The initial version
still spills three states and regresses eight-worker compression1.43% against
candidate7, so it is rejected. The corrected version gains0.68% /0.51% on all
1,408 outside-original files, with identical packets and decoding still above
the original baseline. It advances only to the full development screen; these
small gains are exploratory. Each version passes13,400 exact reference packets
and6,289 custom checks in normal, scalar, and sanitizer builds.
[Initial checks](round3-presence-four-states-prototype-validation.json),
[corrected checks](round3-presence-four-local-prototype-validation.json),
[raw screen](round3-presence-four-states-outside521-screen.json),
[pooled means](round3-presence-four-states-outside521-screen-summary.json).

The corrected four-state encoder also survives the full1,929-file screen:
compression throughput improves2.85% /3.59% against candidate7 (T1/T8), compared
with the smaller0.68% /0.51% outside-original gains. Against the original
baseline the full screen shows13.83% /14.98% encoding throughput improvement
and0.49% /0.53% decoding improvement, with packets identical to candidate7.
The difference between cohorts is retained explicitly; it is not evidence of
independent generalization. The corrected change is selected for full checks
and fixed qualification. [Full raw samples](round3-presence-four-local-full1929-screen.json),
[full pooled means](round3-presence-four-local-full1929-screen-summary.json).
