# slpz fuzzing

libFuzzer harnesses that share the real slpz code (`#include "../slpz.cpp"` with
`main()` guarded out by `SLPZ_FUZZ`). libFuzzer, not AFL++, because clang's
built-in `-fsanitize=fuzzer` works cleanly on macOS/arm64 while AFL++ does not.

## Targets

- **`slpz_fuzz_parse`** (tier 1) — calls `parse_slp` on the raw input. ASAN
  watches for OOB/UAF. This is the class of bug that hit the corrupt bobc-5
  file. Fast (~90–100K exec/s in fork mode).
- **`slpz_fuzz_roundtrip`** (tier 2) — packs the input as 1–4 `.slp` files,
  runs the real `cmd_compress` → `cmd_extract`, and asserts byte-identity.
  Catches **silent corruption** (wrong bytes that don't crash) — which
  `slpz t` cannot — and exercises the cross-file pool path. Slower (real
  compress+extract per input), so it runs at a lower `max_len`.

## Build

    ./build.sh          # builds both targets with ASAN+UBSAN+fuzzer

(The two clangd squiggles about `FUZZ_TARGET` are false — it's set via `-D` at
build time, which the editor's language server doesn't see.)

## Run

    ./run.sh parse                 # fuzz parse_slp until Ctrl-C
    ./run.sh roundtrip 3600        # round-trip oracle for 1 hour
    FORKS=8 MAXLEN=500000 ./run.sh parse

Fork mode = parallel, crash-tolerant, continuous (the AFL-like mode). Crashes
land in `runs/<target>/artifacts/` with a typed prefix (`crash-`, `oom-`,
`leak-`, `timeout-`); the growing corpus is `runs/<target>/corpus/`. Raw
libFuzzer output is teed to `logs/<target>.log`.

## Live dashboard (AFL++-style)

In a second terminal:

    ./dash.py parse                # refresh every 0.5s
    ./dash.py roundtrip -n 0.2     # faster refresh
    ./dash.py parse --once         # one plain-text snapshot (for logging)

Fields: `exec/s live` is computed by the dashboard between refreshes;
`exec/s (lF)` is libFuzzer's own figure; `data age` is seconds since libFuzzer
last wrote a stat line (turns yellow when the numbers go stale — e.g. the run
stopped). `edges (cov)` and `features` are libFuzzer's coverage counters.

## Reproduce a crash

    ./slpz_fuzz_parse runs/parse/artifacts/crash-<hash>       # replays that one input

## Custom codec target

The independent custom codec target covers malformed packet/plane decoding and
arbitrary-input packet and plane encode/decode identities, including shared
presence masks. It links no compression libraries.
From the `slpz` directory, with a clang installation containing libFuzzer:

```sh
make check
./build/custom_codec_test build/custom-corpus
clang++ -std=c++20 -O1 -g -fsanitize=fuzzer,address,undefined \
  -fno-omit-frame-pointer fuzz/custom_codec_fuzz.cpp -o build/custom_codec_fuzz
./build/custom_codec_fuzz build/custom-corpus -max_total_time=60 \
  -max_len=65539 -rss_limit_mb=2048 -artifact_prefix=build/
```

On macOS, if Apple's clang lacks `libclang_rt.fuzzer_osx.a`, use Homebrew LLVM's
`/opt/homebrew/opt/llvm/bin/clang++`. Generated seeds and artifacts stay under
the ignored `build/` directory. A short successful run is a smoke test, not
evidence of a sustained fuzzing campaign.
