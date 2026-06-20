#!/usr/bin/env bash
# perf record + flat report + optional annotate, wrapped for the qwc bench
# workflow. Rung 1 of the "observe before guessing" upgrade.
#
# Usage:
#   scripts/bench/perf-annotate.sh <flag> <corpus-path> [<symbol>]
#
# Env:
#   QWC_PERF_LOCALE  -- locale for LC_ALL (default: C.UTF-8)
#   QWC_BIN          -- path to the qwc binary (default: ./qwc-perf)
#
# Notes:
#   * Records to /tmp/qwc-perf.data so re-annotating a different symbol
#     does not need a fresh record.
#   * Binary-size flags strip unwind tables; --call-graph dwarf is
#     unusable. Use --call-graph lbr as the escape hatch if you need
#     caller attribution.
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "usage: $0 <flag> <corpus-path> [<symbol>]" >&2
  exit 64
fi

flag="$1"
corpus="$2"
symbol="${3:-}"
locale="${QWC_PERF_LOCALE:-C.UTF-8}"
qwc_bin="${QWC_BIN:-./qwc-perf}"
perf_data="/tmp/qwc-perf.data"

if [ ! -x "$qwc_bin" ]; then
  echo "perf-annotate: $qwc_bin not found or not executable." >&2
  echo "Build it with:" >&2
  echo "  cmake -B build-perf -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQWC_SUFFIX=perf" >&2
  echo "  cmake --build build-perf --target qwc" >&2
  exit 1
fi

if [ ! -e "$corpus" ]; then
  echo "perf-annotate: corpus $corpus does not exist." >&2
  echo "Regenerate with:" >&2
  echo "  uv run python3 benchmarks/gen-data.py --bench-corpora --out-dir benchmarks/test-data" >&2
  exit 1
fi

paranoid="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 4)"
if [ "$paranoid" -gt 1 ]; then
  echo "perf-annotate: kernel.perf_event_paranoid=$paranoid (need <= 1)." >&2
  echo "Lower it for this session with:" >&2
  echo "  sudo sysctl kernel.perf_event_paranoid=1" >&2
  exit 1
fi

echo "=== perf record (locale=$locale, flag=$flag, corpus=$corpus) ==="
# shellcheck disable=SC2086  # $flag may legitimately be e.g. "-l -w"
LC_ALL="$locale" perf record -o "$perf_data" -e cycles:pp -- \
  "$qwc_bin" $flag "$corpus" >/dev/null

echo
echo "=== perf report (top symbols, >= 1% of cycles) ==="
perf report -i "$perf_data" --stdio --no-children --percent-limit 1 | head -50

if [ -n "$symbol" ]; then
  echo
  echo "=== perf annotate $symbol ==="
  perf annotate -i "$perf_data" --stdio --no-source "$symbol"
else
  echo
  echo "Re-annotate any symbol from the report above with:"
  echo "  perf annotate -i $perf_data --stdio --no-source <symbol>"
fi
