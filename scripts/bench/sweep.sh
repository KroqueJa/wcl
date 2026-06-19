#!/usr/bin/env bash
# Unified qwc benchmark sweep. Drives benchmarks/bench.py across the six
# bench corpora and prints per-step wall time.
#
# Default: LTO candidate, qwc + latest-release columns only (no uu-wc /
# GNU wc), both locales (LC_ALL=C and LC_ALL=C.UTF-8) per
# benchmarks/bench.py's --locales default. ~3 min total on the i7-8700
# under --no-prep (164s measured 2026-06-19); slightly longer with
# bench-prep. Each cell runs once per locale; qwc adopts LC_CTYPE once
# at startup so the two locales exercise different kernel paths.
#
# Flags:
#   --with-competitors   re-add uu-wc + GNU wc columns. Adds several
#                        minutes -- GNU wc -m under C.UTF-8 is much
#                        slower than under C, so the C.UTF-8 rows
#                        dominate wall time.
#   --no-prep            skip 'sudo scripts/bench/prep.sh apply' (default:
#                        apply on Linux; macOS skips automatically).
#
# Override per-cell budget with WARMUP / RUNS env vars (defaults: 3 / 20).
# Single-locale: pass --locales c (or c.utf-8) through bench.py via
# editing the invocation below — there is no shell-level flag for it.
#
# Output: per-corpus rendered table to stdout + logs/<name>.json sidecars.
# logs/ is gitignored.
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

# --- flag parsing -----------------------------------------------------------
with_competitors=0
do_prep=1
for arg in "$@"; do
  case "$arg" in
    --with-competitors)  with_competitors=1 ;;
    --no-prep)           do_prep=0 ;;
    -h|--help)
      sed -n '2,/^set -euo pipefail/p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "sweep: unknown flag '$arg'" >&2; exit 2 ;;
  esac
done

WARMUP="${WARMUP:-3}"
RUNS="${RUNS:-20}"
candidate_bin="/tmp/qwc-bench-candidate"
sweep_start=$SECONDS

step_time() {
  # Print "  took Ns" using the difference between $SECONDS and the
  # caller-supplied start value. Bash's $SECONDS is a per-process counter.
  local start="$1"
  echo "    took $(( SECONDS - start ))s"
}

# --- system prep ------------------------------------------------------------
if [ "$(uname -s)" = "Linux" ] && [ "$do_prep" = "1" ]; then
  echo "=== Quieting the system (pass --no-prep to skip) ==="
  sudo -v
  sudo ./scripts/bench/prep.sh apply
  trap 'sudo ./scripts/bench/prep.sh restore' EXIT INT TERM
fi

# --- build candidate --------------------------------------------------------
echo "=== Building candidate ==="
step_start=$SECONDS
./scripts/bench/sync-current-build.sh
step_time "$step_start"
cp qwc "$candidate_bin"

# --- baseline ---------------------------------------------------------------
echo "=== Building / refreshing the latest-release baseline ==="
step_start=$SECONDS
./scripts/bench/sync-latest-release.sh
step_time "$step_start"
baseline_ver="$("$repo_root/qwc-latest-release" --version 2>/dev/null | awk '{print $2}')"
echo "candidate : $("$candidate_bin" --version 2>/dev/null)"
echo "baseline  : $baseline_ver"

mkdir -p logs

# --- per-corpus sweep -------------------------------------------------------
# name|corpus path (relative to repo root)|human title
corpora=(
  "big|benchmarks/test-data/big.txt|Single large file (256 MiB)"
  "long|benchmarks/test-data/long|Long lines (256 MiB)"
  "many|benchmarks/test-data/many|Many small files (256 MiB total)"
  "mixed|benchmarks/test-data/mixed|Mixed shape (256 MiB)"
  "short|benchmarks/test-data/short|Short lines (256 MiB)"
  "single-line|benchmarks/test-data/single-line|Single line (256 MiB)"
)

bench_args=()
if [ "$with_competitors" = "0" ]; then
  bench_args+=( --no-competitors )
fi

for entry in "${corpora[@]}"; do
  name="${entry%%|*}"; rest="${entry#*|}"
  data="${rest%%|*}"; title="${rest#*|}"
  if [ ! -e "$data" ]; then
    echo "WARNING: $data missing -- skipping $name " \
         "(regenerate with: python3 benchmarks/gen-data.py --bench-corpora --out-dir benchmarks/test-data)" >&2
    continue
  fi
  echo "=== Benchmarking $name ==="
  step_start=$SECONDS
  python3 benchmarks/bench.py \
    --qwc "$candidate_bin" --qwc-main "$repo_root/qwc-latest-release" \
    --qwc-main-name "$baseline_ver" \
    --warmup "$WARMUP" --runs "$RUNS" \
    --data "$data" --title "$title (candidate vs $baseline_ver)" \
    --json-out "logs/$name.json" \
    "${bench_args[@]}" \
    2>&1 | tee "logs/bench-$name.log"
  step_time "$step_start"
done

echo
echo "=== Sweep complete ==="
echo "    total wall: $(( SECONDS - sweep_start ))s"
echo "Tables in logs/bench-*.log, means in logs/*.json."
