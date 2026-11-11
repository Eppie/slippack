#!/bin/bash
# Build core once with sanitizer coverage, then link each focused harness.
set -euo pipefail
cd "$(dirname "$0")"
CXX=${CXX:-clang++}
SAN=${SAN:-address,undefined,fuzzer}
FLAGS="-std=c++20 -g -O1 -fno-omit-frame-pointer -fsanitize=$SAN -pthread"
LIBS="${FUZZER_LIB:-}"
if [[ "$(uname -s)" != Darwin ]]; then LIBS="$LIBS -lcrypto"; fi
mkdir -p ../build/fuzz
OBJECTS=""
for source in archive frames record_codec slp platform; do
  $CXX $FLAGS -c "../$source.cpp" -o "../build/fuzz/$source.o"
  OBJECTS="$OBJECTS ../build/fuzz/$source.o"
done
names=(parse roundtrip dictionary predictors archive)
for target in 1 2 3 4 5; do
  name=${names[$((target - 1))]}
  echo "building slpz_fuzz_$name..."
  $CXX $FLAGS -DFUZZ_TARGET=$target slpz_fuzz.cpp $OBJECTS $LIBS -o "slpz_fuzz_$name"
done
