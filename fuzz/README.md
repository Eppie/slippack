# slippack fuzzing

The harnesses link the same archive, replay, record, and frame modules as the
CLI. They call the in-memory library API directly; no production filesystem
replacement or inclusion of `.cpp` files is needed.

| Target | Coverage |
|---|---|
| `parse` | Arbitrary Slippi declarations, malformed/truncated event streams |
| `roundtrip` | 1–4 arbitrary files through encoding and decoding, exact byte equality |
| `dictionary` | All four dictionary transforms, index boundaries, record packet framing |
| `predictors` | Motion/controller transforms composed with record coding, old/new record lengths |
| `archive` | Hostile container metadata, frames, paths, counts, and limits; expected `Error` exceptions are caught |
| `gecko` | Built-in dictionary references, malformed tokens/record extensions, exact byte reconstruction |

## Build and run

Use a Clang installation with libFuzzer:

```sh
CXX=clang++ ./fuzz/build.sh
mkdir -p build/archive-corpus
cp tests/fixtures/v1-custom.slpc build/archive-corpus/
./fuzz/slippack_fuzz_archive build/archive-corpus -runs=2000 -max_len=65536
./fuzz/slippack_fuzz_roundtrip fuzz/corpus_rt -max_total_time=60 -max_len=131072
```

`fuzz/build.sh` compiles the core once with ASan/UBSan coverage and links all
six targets. `CXX`, `SAN`, and `FUZZER_LIB` can override the compiler,
sanitisers, and optional explicit libFuzzer runtime. On macOS, Apple's compiler
may omit libFuzzer. One option is Homebrew LLVM; if its object files are
incompatible with the installed Apple linker, use Apple's compiler with the
Homebrew runtime:

```sh
CXX=/usr/bin/clang++ SAN=address,undefined,fuzzer-no-link \
  FUZZER_LIB=/opt/homebrew/opt/llvm/lib/clang/22/lib/darwin/libclang_rt.fuzzer_osx.a \
  ./fuzz/build.sh
```

Adjust the runtime path for the installed version. Linux also needs OpenSSL
headers and `libcrypto`, which the script links automatically.

For ongoing runs with logs and a live dashboard:

```sh
cd fuzz
./run.sh parse
./run.sh roundtrip 3600
./dash.py parse --once
```

Generated seeds, logs, and crashes stay under ignored `build/` or `fuzz/runs/`
directories. Reproduce a crash by passing its artifact directly to the same
binary. A successful short run is a smoke test, not a sustained fuzzing campaign.

## Standalone payload codec

`fuzz/custom_codec_fuzz.cpp` covers malformed payload/plane decoding and exact
encode/decode identities, including shared masks. It needs no archive modules
or compression libraries.

```sh
./build/custom_codec_test build/custom-corpus
clang++ -std=c++20 -O1 -g -fsanitize=fuzzer,address,undefined \
  -fno-omit-frame-pointer fuzz/custom_codec_fuzz.cpp -o build/custom_codec_fuzz
./build/custom_codec_fuzz build/custom-corpus -max_total_time=60 \
  -max_len=65539 -rss_limit_mb=2048 -artifact_prefix=build/
```

The unit tests also reject malformed metadata through exceptions directly,
without forking children or treating ordinary decoder errors as crashes.
