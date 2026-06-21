#!/usr/bin/env bash
# TMA (top-down microarchitecture analysis) wrapper for the qwc bench
# workflow. Rung 2 of the "observe before guessing" upgrade.
#
# Usage:
#   scripts/bench/toplev.sh <flag> <corpus-path>
#
# Env:
#   QWC_PERF_LOCALE  -- locale for LC_ALL (default: C.UTF-8)
#   QWC_BIN          -- path to the qwc binary (default: ./qwc-perf)
#
# --global aggregates across all CPUs into one readout, which is what
# qwc's multithreaded workload wants: worker threads migrate between
# cores, so per-core readings are dominated by worker-migration variance
# rather than kernel-cost signal. Override with --core / --per-thread if
# you need that view; the wrapper's default targets the headline question.
#
# Common variations (run toplev.py directly for these):
#   * -l1 / -l6                  : different drill-down depth
#   * --csv ';'                  : machine-parsable output
#   * --per-thread               : per-thread breakdown instead of global
#   * --core C0-C5               : limit to specific cores
#   * --no-multiplex             : avoid counter time-sharing (needs
#                                  restricted counter group; trade
#                                  hierarchy depth for accuracy)
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <flag> <corpus-path>" >&2
  exit 64
fi

flag="$1"
corpus="$2"
locale="${QWC_PERF_LOCALE:-C.UTF-8}"
qwc_bin="${QWC_BIN:-./qwc-perf}"

if ! command -v toplev.py >/dev/null 2>&1; then
  echo "toplev: toplev.py not found on PATH." >&2
  echo "One-time setup (pmu-tools is not packaged on Arch):" >&2
  echo "  git clone https://github.com/andikleen/pmu-tools <somewhere>" >&2
  echo "  ln -sfn <somewhere>/toplev.py <a-dir-on-your-PATH>/toplev.py" >&2
  exit 1
fi

if [ ! -x "$qwc_bin" ]; then
  echo "toplev: $qwc_bin not found or not executable." >&2
  echo "Build it with:" >&2
  echo "  cmake -B build-perf -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQWC_SUFFIX=perf" >&2
  echo "  cmake --build build-perf --target qwc" >&2
  exit 1
fi

if [ ! -e "$corpus" ]; then
  echo "toplev: corpus $corpus does not exist." >&2
  echo "Regenerate with:" >&2
  echo "  uv run python3 benchmarks/gen-data.py --bench-corpora --out-dir benchmarks/test-data" >&2
  exit 1
fi

paranoid="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 4)"
if [ "$paranoid" -gt -1 ]; then
  echo "toplev: kernel.perf_event_paranoid=$paranoid (need <= -1 for CPU event access)." >&2
  echo "Lower it for this session with:" >&2
  echo "  sudo sysctl kernel.perf_event_paranoid=-1" >&2
  exit 1
fi

echo "=== toplev (locale=$locale, flag=$flag, corpus=$corpus) ==="
# shellcheck disable=SC2086  # $flag may legitimately be e.g. "-l -w"
LC_ALL="$locale" toplev.py -l3 --no-desc --global -- "$qwc_bin" $flag "$corpus"
