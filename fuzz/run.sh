#!/bin/bash
# Launch a slpz fuzz target in libFuzzer fork mode (parallel, crash-tolerant,
# continuous — the AFL-like mode). Streams stats to the terminal AND to
# logs/<target>.log, which fuzz/dash.py renders as a live status screen.
#
#   ./run.sh parse            # fuzz parse_slp until Ctrl-C
#   ./run.sh roundtrip 3600   # round-trip oracle for 1 hour
#   FORKS=8 MAXLEN=500000 ./run.sh parse
#
# Watch it in another terminal with:  ./dash.py parse
set -euo pipefail
cd "$(dirname "$0")"

target=${1:?usage: run.sh <parse|roundtrip> [seconds]}
secs=${2:-0}   # 0 = run until Ctrl-C

# STUB=1 selects the roundtrip binary with LZMA/brotli stubbed to identity
# (zstd stays real) — fuzzes our code, not the trusted codecs. Much faster.
binname="slpz_fuzz_$target"
[[ "$target" == roundtrip && "${STUB:-0}" != 0 ]] && binname="slpz_fuzz_roundtrip_fast"
bin="./$binname"
[[ -x "$bin" ]] || { echo "building..."; ./build.sh; }

case "$target" in
  parse)     seeds=corpus_parse; defmax=1500000 ;;
  roundtrip) seeds=corpus_rt;    defmax=131072  ;;  # smaller = more exec/s; the
                                                    # transform paths don't need
                                                    # multi-MB inputs to fire
  *) echo "unknown target '$target' (use parse|roundtrip)"; exit 2 ;;
esac

# sweep temp dirs left by the pre-memfs disk-based harness (harmless if none)
rm -rf /tmp/slpz_fuzz_* 2>/dev/null || true

run="runs/$target"
mkdir -p "$run/corpus" "$run/artifacts" logs
forks=${FORKS:-$(( $(sysctl -n hw.ncpu) - 2 ))}
maxlen=${MAXLEN:-$defmax}
# scalar, not an array: macOS Bash 3.2 errors on empty-array expansion under
# `set -u`. The flags have no spaces, so unquoted expansion is safe.
timeflag=""; [[ "$secs" -gt 0 ]] && timeflag="-max_total_time=$secs"

# ── crash-discovery flags ────────────────────────────────────────────────────
# value_profile: guide the mutator past the magic memcmp, payload-size compares,
#   and seed LCG matches using CMP instrumentation. The single biggest lever for
#   a format parser; costs some speed. Off with VP=0.
# dict: Slippi tokens (magic, command bytes) so mutations build valid structure.
# timeout=25: a single input taking >25s is flagged as a hang/crash instead of
#   silently wedging the run.
vp="-use_value_profile=${VP:-1}"
dict=""; [[ -f slp.dict ]] && dict="-dict=slp.dict"
# ── speed / robustness (ASAN) ────────────────────────────────────────────────
# detect_leaks=0: skip the exit-time leak scan (faster, no spurious fork stops);
# detect_stack_use_after_return=1: catch a bug class default ASAN misses;
# abort_on_error=1: SIGABRT so libFuzzer captures the crashing input cleanly.
export ASAN_OPTIONS="detect_leaks=0:detect_stack_use_after_return=1:abort_on_error=1:${ASAN_OPTIONS:-}"
# ── verbosity ────────────────────────────────────────────────────────────────
# VERBOSE=1 -> -verbosity=2 + print new coverage PCs as they're found. Best
# paired with FORKS=0 (single process), since fork mode only prints merged job
# stats and hides per-input mutation (MS:) lines.
verbose=""; [[ "${VERBOSE:-0}" != 0 ]] && verbose="-verbosity=2 -print_pcs=1"

# FORKS=0 -> single process: every NEW input prints its mutation chain
# (MS: ShuffleBytes-ChangeBit-...) and coverage. Slower, no crash-tolerance,
# but the right mode for watching how it explores.
if [[ "$forks" -le 0 ]]; then
  modeflag=""
  echo "target=$target MODE=single-process max_len=$maxlen level=${SLPZ_FUZZ_LEVEL:-3} seeds=$seeds"
else
  modeflag="-fork=$forks -ignore_crashes=1"
  echo "target=$target forks=$forks max_len=$maxlen level=${SLPZ_FUZZ_LEVEL:-3} vp=${VP:-1} seeds=$seeds"
fi
echo "dashboard:  ./dash.py $target   (log: logs/$target.log)"

# fork mode merges per-job corpora into $run/corpus; artifacts (crashes) land in
# $run/artifacts with a typed prefix. -ignore_crashes keeps fuzzing past a find
# so an overnight run surveys the whole space instead of stopping at #1.
exec "$bin" \
  $modeflag -print_final_stats=1 $vp $dict $verbose \
  -artifact_prefix="$run/artifacts/" -max_len="$maxlen" $timeflag \
  "$run/corpus" "$seeds" 2>&1 | tee "logs/$target.log"
