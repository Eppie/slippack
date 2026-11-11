#!/usr/bin/env bash
# Fetch pinned upstream sources and build isolated benchmark executables.
set -euo pipefail
cd "$(dirname "$0")/../.."
CXX=${CXX:-c++}
work=build/single-file-study
mkdir -p "$work"
checkout() {
  local name=$1 url=$2 commit=$3
  if [[ ! -d build/$name/.git ]]; then
    git clone --no-checkout "$url" "build/$name"
  fi
  if [[ -n "$(git -C "build/$name" status --porcelain --untracked-files=no)" ]]; then
    echo "Refusing to change modified checkout build/$name" >&2
    exit 1
  fi
  git -C "build/$name" fetch --depth 1 origin "$commit"
  git -C "build/$name" checkout --detach "$commit"
}
checkout alexander-slpz https://github.com/AlexanderHarrison/slpz.git 9f4d952f444037edbbc49af11e5e963ecd4d399f
checkout pcrain-slippc https://github.com/pcrain/slippc.git 8d79a665392e491edd62150687a956e45a7a0fe7
make build/libslippack.a
libs=()
if [[ $(uname -s) != Darwin ]]; then libs+=(-lcrypto); fi
"$CXX" -std=c++20 -O3 -pthread benchmarks/2026-11-10-single-file/cpp_runner.cpp \
  build/libslippack.a "${libs[@]}" -o "$work/ours"
# slippc's own vendored LZMA headers are used, with the system liblzma.
read -r -a lzma_flags <<< "$(pkg-config --libs liblzma)"
"$CXX" -std=c++17 -O3 -pthread -DBENCH_SLIPPC -Ibuild/pcrain-slippc/src \
  benchmarks/2026-11-10-single-file/cpp_runner.cpp build/pcrain-slippc/src/compressor.cpp \
  "${lzma_flags[@]}" -o "$work/slippc"
cargo build --release --locked --manifest-path benchmarks/2026-11-10-single-file/rust/Cargo.toml \
  --target-dir "$work/rust-target"
