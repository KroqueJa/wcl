#!/usr/bin/env bash
# Run benchmarks/bench.py against the mixed-shape corpus (512 MiB).
set -euo pipefail
"$(dirname "$0")/sync-latest-release.sh"
exec python3 benchmarks/bench.py \
  --qwc ./qwc --qwc-main ./qwc-latest-release --qwc-main-name latest-release \
  --warmup 1 \
  --data benchmarks/test-data/mixed-512MiB \
  --title "Mixed shape (512 MiB)" "$@"
