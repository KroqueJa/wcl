#!/usr/bin/env bash
# ab: mechanism A/B between the working-tree qwc (./qwc, built from the
# branch) and the main baseline (./qwc-main), via `perf stat -r N`. Complements
# benchmarks/bench.py: bench.py gives wall-clock + speedup ratios; this gives
# cycles / instructions / branches / branch-misses / LLC-load-misses, which is
# how you tell a real win from an alignment-luck win.
#
# Pre-reqs:
#   1. ./qwc-main exists. Build once per main-side change:
#        git worktree add -f /tmp/qwc-main-baseline main
#        cmake -S /tmp/qwc-main-baseline -B /tmp/qwc-main-baseline/build \
#            -DCMAKE_BUILD_TYPE=Release -DQWC_BUILD_TESTS=OFF
#        cmake --build /tmp/qwc-main-baseline/build --target qwc -j
#        cp /tmp/qwc-main-baseline/qwc ./qwc-main
#        git worktree remove --force /tmp/qwc-main-baseline
#   2. A current ./qwc; this script rebuilds via sync-current-build.sh.
#   3. `perf` available (linux-tools / perf package).
#
# Usage:
#   scripts/bench/ab.sh [-r RUNS] <flags-string> <corpus-path>
# Examples:
#   scripts/bench/ab.sh "-l -w" benchmarks/test-data/mixed-256MiB
#   scripts/bench/ab.sh -r 20 -- "-l -w -m" benchmarks/test-data/cjk-short.txt
#   LC_ALL=C.UTF-8 scripts/bench/ab.sh "-l -w" benchmarks/test-data/cjk-short.txt
#
# Output goes to stdout: two perf stat blocks, branch then main, with the qwc
# --version banner above each so it's unambiguous which build produced which
# numbers. Both perf and the banners are merged onto stdout so a single
# redirect (`scripts/bench/ab.sh ... > out.log`) captures everything.
set -euo pipefail
exec 2>&1

runs=10
if [[ "${1:-}" == "-r" ]]; then runs="$2"; shift 2; fi
if [[ "${1:-}" == "--" ]]; then shift; fi
if [[ $# -lt 2 ]]; then
  echo "usage: $(basename "$0") [-r RUNS] <flags-string> <corpus-path>" >&2
  echo "  flags can be empty: '' = bare invocation (lines+words+bytes)" >&2
  exit 2
fi
flags="$1"
corpus="$2"

cd "$(dirname "$0")/../.."
"$(dirname "$0")/sync-current-build.sh" >/dev/null

if [[ ! -x ./qwc-main ]]; then
  echo "ab: ./qwc-main missing -- build it (see script header) and retry." >&2
  exit 1
fi

events=cycles,instructions,branches,branch-misses,LLC-load-misses

# Warm the page cache so the first build doesn't pay a cold-cache penalty.
cat "$corpus" >/dev/null

# shellcheck disable=SC2086  # flags is meant to word-split into argv tokens.
echo "=== branch: $(./qwc --version | head -1) / flags='$flags' / runs=$runs ==="
perf stat -e "$events" -r "$runs" ./qwc $flags "$corpus" >/dev/null

echo
echo "=== main:   $(./qwc-main --version | head -1) / flags='$flags' / runs=$runs ==="
# shellcheck disable=SC2086
perf stat -e "$events" -r "$runs" ./qwc-main $flags "$corpus" >/dev/null
