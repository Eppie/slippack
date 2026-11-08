# Custom codec implementation benchmark

Apple M4 Max, 16 threads, `-O3`, level 3. Median of five warmed runs; backend
order randomized each round. Decode uses `slpz t` and excludes disk writes.
All source files were checked against decoded SHA-1s before timing repetitions.
Source lists and complete timing samples are stored alongside this report.

| Dataset | Codec | Archive bytes | Encode seconds | Decode seconds |
|---|---|---:|---:|---:|
| Summit 11, 140 files / 344,239,004 B | Legacy | 22,436,101 | 0.542 | 0.118 |
| | Tuned LZMA | 21,577,020 | 0.268 | 0.114 |
| | Custom | 23,262,365 | 0.285 | 0.086 |
| GOML 2019 + Bair 2026, 103 files / 297,658,164 B | Legacy | 20,210,993 | 0.465 | 0.107 |
| | Tuned LZMA | 17,998,650 | 0.251 | 0.096 |
| | Custom | 19,999,491 | 0.275 | 0.076 |

The mixed dataset contains 102 replays and one small provenance TSV. Across
both datasets, custom archives are 7.8–11.1% larger than tuned LZMA, encoding
takes 6.1–9.8% longer, and decoding takes 20.5–24.9% less time. This is a
decode-speed/size tradeoff, not a universal improvement.

The earlier generic custom LZ+rANS prototype produced 25,903,037 and
21,137,227 bytes, respectively. The implemented model/parse changes reduce
those sizes by 10.2% and 5.4%. The original default remains unchanged.

`tuned` and `custom` use the same velocity and input predictors. Tuned also
right-sizes the LZMA encoder dictionary to each block. This comparison avoids
attributing gains from the new predictors or fixed allocation overhead to the
custom entropy coder.

Reproduce after extracting the source archives into the same directory layout
as the recorded lists:

```sh
make check
python3 benchmarks/compare_custom.py --root /path/to/summit \
  --list benchmarks/2026-11-08/sample-files.txt --repeats 5 --threads 16
python3 benchmarks/compare_custom.py --root /path/to/mixed \
  --list benchmarks/2026-11-08/mixed-files.txt --repeats 5 --threads 16
```

Validation includes standalone ASan/UBSan checks, all packet modes, semantic
model selection, truncation/mutation cases, archive extraction at levels 1/3/9
for all backends, and exceptional float bits. A 31-second libFuzzer smoke run
completed 79,333 executions without findings; this was not a sustained
campaign. New code decoded all 419 files in the existing Summit v9 archive;
the old binary decoded new legacy output and rejected v10 cleanly.
