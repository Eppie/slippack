#!/bin/bash
# Build the two libFuzzer targets (parse + roundtrip) with ASAN + UBSAN.
# clang's built-in libFuzzer works cleanly on macOS/arm64 (unlike AFL++).
set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-clang++}
SAN=${SAN:-address,undefined,fuzzer}
FLAGS="-std=c++20 -g -O1 -fno-omit-frame-pointer -fsanitize=$SAN"
LIBS=""
if [[ "$(uname -s)" != Darwin ]]; then LIBS="-lcrypto"; fi

echo "building slpz_fuzz_parse (FUZZ_TARGET=1)..."
$CXX $FLAGS -DFUZZ_TARGET=1 slpz_fuzz.cpp $LIBS -o slpz_fuzz_parse

echo "building slpz_fuzz_roundtrip (FUZZ_TARGET=2)..."
$CXX $FLAGS -DFUZZ_TARGET=2 slpz_fuzz.cpp $LIBS -o slpz_fuzz_roundtrip

echo "done: ./slpz_fuzz_parse  ./slpz_fuzz_roundtrip"
