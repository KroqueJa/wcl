#!/usr/bin/env bash
# Run benchmarks/bench.py against the short-lines corpus (512 MiB).
set -euo pipefail
exec python3 benchmarks/bench.py \
  --qwc ./qwc --qwc-main ./qwc-main --qwc-main-name latest-release \
  --warmup 1 \
  --data benchmarks/test-data/short-512MiB \
  --title "Short lines (512 MiB)" "$@"
