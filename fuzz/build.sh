#!/bin/bash
# Build the two libFuzzer targets (parse + roundtrip) with ASAN + UBSAN.
# clang's built-in libFuzzer works cleanly on macOS/arm64 (unlike AFL++).
set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-clang++}
INC=${ZSTD_INC:-/opt/homebrew/include}
LIB=${ZSTD_LIB:-/opt/homebrew/lib}
SAN=${SAN:-address,undefined,fuzzer}
FLAGS="-std=c++20 -g -O1 -fno-omit-frame-pointer -fsanitize=$SAN"
LIBS="-I$INC -L$LIB -lzstd -llzma -lbrotlienc -lbrotlidec"

echo "building slpz_fuzz_parse (FUZZ_TARGET=1)..."
$CXX $FLAGS -DFUZZ_TARGET=1 slpz_fuzz.cpp $LIBS -o slpz_fuzz_parse

echo "building slpz_fuzz_roundtrip (FUZZ_TARGET=2)..."
$CXX $FLAGS -DFUZZ_TARGET=2 slpz_fuzz.cpp $LIBS -o slpz_fuzz_roundtrip

# Same round-trip, but LZMA/brotli stubbed to an identity codec (zstd stays
# real). Much faster — fuzzes our transform/format code, not the trusted codecs.
echo "building slpz_fuzz_roundtrip_fast (FUZZ_TARGET=2, SLPZ_FUZZ_STUB_SLOW)..."
$CXX $FLAGS -DFUZZ_TARGET=2 -DSLPZ_FUZZ_STUB_SLOW slpz_fuzz.cpp $LIBS -o slpz_fuzz_roundtrip_fast

echo "done: ./slpz_fuzz_parse  ./slpz_fuzz_roundtrip  ./slpz_fuzz_roundtrip_fast"
