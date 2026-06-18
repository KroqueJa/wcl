#!/usr/bin/env bash
# Full PGO+LTO benchmark sweep: the PGO candidate vs the latest release, across
# every corpus, into logs/.
#
# Why this exists instead of `for s in scripts/bench-*; do QWC_BENCH_PGO=1 $s; done`:
#   * That loop rebuilds the full PGO pipeline once PER corpus (6x ~60-120s).
#   * Worse, the plain bench-*.sh wrappers default to a NON-PGO (LTO-only)
#     build, so a sweep that forgets QWC_BENCH_PGO=1 silently measures
#     LTO-only vs the release -- which is ~1.00x everywhere and looks like
#     "PGO does nothing". The whole point of the PGO win lives in the PGO
#     build, not the LTO-only one.
#
# This script builds the PGO+LTO binary ONCE, stashes it where nothing can
# clobber it (both build-pgo.sh and the plain ninja build write ./qwc), then
# drives benchmarks/bench.py directly against the stash for every corpus. No
# per-corpus rebuild, no clobber.
#
# Usage:
#   ./scripts/bench-pgo-lto-full.sh            # warmup=3 runs=20
#   WARMUP=2 RUNS=30 ./scripts/bench-pgo-lto-full.sh
#
# Output: logs/bench-<name>.log (the rendered table) and logs/pgo-<name>.json
# (machine-readable means) per corpus.
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

WARMUP="${WARMUP:-3}"
RUNS="${RUNS:-20}"
pgo_bin="/tmp/qwc-pgo-bench"

echo "=== Building PGO+LTO candidate once ==="
QWC_BENCH_PGO=1 ./scripts/sync-current-build.sh
cp qwc "$pgo_bin"

echo "=== Building / refreshing the latest-release baseline ==="
./scripts/sync-latest-release.sh
baseline_ver="$("$repo_root/qwc-latest-release" --version 2>/dev/null | awk '{print $2}')"
echo "candidate : $("$pgo_bin" --version 2>/dev/null)"
echo "baseline  : $baseline_ver"

mkdir -p logs

# name|corpus path (relative to repo root)|human title
corpora=(
  "big|benchmarks/test-data/big.txt|Single large file"
  "long|benchmarks/test-data/long-512MiB|Long lines (512 MiB)"
  "many|benchmarks/test-data/many|Many small files"
  "mixed|benchmarks/test-data/mixed-512MiB|Mixed shape (512 MiB)"
  "short|benchmarks/test-data/short-512MiB|Short lines (512 MiB)"
  "single-line|benchmarks/test-data/single-line-512MiB|Single line (512 MiB)"
)

for entry in "${corpora[@]}"; do
  name="${entry%%|*}"; rest="${entry#*|}"; data="${rest%%|*}"; title="${rest#*|}"
  if [ ! -e "$data" ]; then
    echo "WARNING: $data missing -- skipping $name (generate it with gen-data.py)" >&2
    continue
  fi
  echo "=== Benchmarking $name ==="
  python3 benchmarks/bench.py \
    --qwc "$pgo_bin" --qwc-main "$repo_root/qwc-latest-release" \
    --qwc-main-name "$baseline_ver" \
    --warmup "$WARMUP" --runs "$RUNS" \
    --data "$data" --title "$title (PGO+LTO vs $baseline_ver)" \
    --json-out "logs/pgo-$name.json" \
    2>&1 | tee "logs/bench-$name.log"
done

echo
echo "Done. Tables in logs/bench-*.log, means in logs/pgo-*.json."
echo "Candidate was PGO -- confirm with: grep -o 'fprofile-use=[^ ]*' build-pgo/CMakeFiles/qwc.dir/flags.make"
