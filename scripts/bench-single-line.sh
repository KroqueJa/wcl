#!/usr/bin/env bash
# Run benchmarks/bench.py against the single-line corpus (512 MiB, one logical line).
set -euo pipefail
"$(dirname "$0")/sync-current-build.sh"
"$(dirname "$0")/sync-latest-release.sh"
exec python3 benchmarks/bench.py \
  --qwc ./qwc --qwc-main ./qwc-latest-release --qwc-main-name latest-release \
  --warmup 1 \
  --data benchmarks/test-data/single-line-512MiB \
  --title "Single line (512 MiB)" "$@"
