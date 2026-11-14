# Benchmark harness

Shared drivers for the studies in the sibling directories. Each study's README
gives the exact command and build it used.

| File | Purpose |
|---|---|
| [`benchmark.py`](benchmark.py) | Compares a baseline and a candidate runner on one manifest of replays. Runs one warmup, then randomly interleaved repetitions at each level and worker count, and writes every timing sample, per-file size, and input, source, and binary hash to a JSON report. |
| [`cpp_runner.cpp`](cpp_runner.cpp) | The runner `benchmark.py` drives. It links against `libslippack.a`, loads the manifest's replays into RAM, and encodes and decodes one replay per archive. File reading and byte comparison are outside its timers. Built with `-DBENCH_SLIPPC`, it wraps slippc instead. |
| [`build_competitors.sh`](build_competitors.sh) | Fetches slpz and slippc at pinned commits and builds their runners into `build/single-file-study/`. |
| [`rust/`](rust/) | Runner for slpz and Peppi, with locked dependencies. |

## Comparing two builds

```sh
c++ -std=c++20 -O3 -pthread -I. -c benchmarks/harness/cpp_runner.cpp -o /tmp/runner.o
c++ -std=c++20 -O3 -pthread /tmp/runner.o /path/to/base/build/libslippack.a \
  -o /tmp/baseline-runner
c++ -std=c++20 -O3 -pthread /tmp/runner.o build/libslippack.a -o /tmp/candidate-runner
python3 benchmarks/harness/benchmark.py --allow-byte-changes \
  --baseline /tmp/baseline-runner --candidate /tmp/candidate-runner \
  --baseline-commit <commit> --manifest /path/to/replays.txt --levels 3 \
  --threads 1,8 --repeats 21 --power-mode 2 --output /tmp/result.json
```

The manifest is a text file with one replay path per line. Without
`--allow-byte-changes` the driver requires both runners to write identical
archives. Every run checks that decoding reproduces the original bytes.

`--power-mode` and `--require-ac-power` apply to macOS; the driver polls the
power source and mode once per second and rejects a run in which either
changes. Profile-guided libraries on macOS link with `-Wl,-no_deduplicate`.

Encode samples from one build and worker count span 6% to 20% from fastest to
slowest. Differences of a few percent need about 21 interleaved repetitions to
resolve.
