#!/usr/bin/env bash
# Unified qwc benchmark sweep. Drives benchmarks/bench.py across the six
# bench corpora and prints per-step wall time.
#
# Default: LTO-only candidate (no PGO), qwc + latest-release columns only
# (no uu-wc / GNU wc), ~2 min total on the i7-8700.
#
# Flags:
#   --pgo                build the candidate with PGO+LTO (build-pgo.sh)
#                        instead of plain Release. Adds ~90s build time.
#   --with-competitors   re-add uu-wc + GNU wc columns. Adds ~3 min.
#   --no-prep            skip 'sudo bench-prep apply' (default: apply on
#                        Linux; macOS skips automatically).
#
# Override per-cell budget with WARMUP / RUNS env vars (defaults: 3 / 20).
#
# Output: per-corpus rendered table to stdout + logs/<name>.json sidecars.
# logs/ is gitignored.
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

# --- flag parsing -----------------------------------------------------------
use_pgo=0
with_competitors=0
do_prep=1
for arg in "$@"; do
  case "$arg" in
    --pgo)               use_pgo=1 ;;
    --with-competitors)  with_competitors=1 ;;
    --no-prep)           do_prep=0 ;;
    -h|--help)
      sed -n '2,/^set -euo pipefail/p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "bench-sweep: unknown flag '$arg'" >&2; exit 2 ;;
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
  sudo ./scripts/bench-prep.sh apply
  trap 'sudo ./scripts/bench-prep.sh restore' EXIT INT TERM
fi

# --- build candidate --------------------------------------------------------
if [ "$use_pgo" = "1" ]; then
  echo "=== Building PGO+LTO candidate ==="
  step_start=$SECONDS
  QWC_BENCH_PGO=1 ./scripts/sync-current-build.sh
  step_time "$step_start"
else
  echo "=== Building LTO-only candidate ==="
  step_start=$SECONDS
  ./scripts/sync-current-build.sh
  step_time "$step_start"
fi
cp qwc "$candidate_bin"

# --- baseline ---------------------------------------------------------------
echo "=== Building / refreshing the latest-release baseline ==="
step_start=$SECONDS
./scripts/sync-latest-release.sh
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
if [ "$use_pgo" = "1" ]; then
  echo "Candidate was PGO -- confirm with: grep -o 'fprofile-use=[^ ]*' build-pgo/CMakeFiles/qwc.dir/flags.make"
fi
