#!/usr/bin/env bash
# Run benchmarks/bench.py against the single-line corpus (512 MiB, one logical line).
set -euo pipefail
exec python3 benchmarks/bench.py \
  --qwc ./qwc --qwc-main ./qwc-main --qwc-main-name latest-release \
  --warmup 1 \
  --data benchmarks/test-data/single-line-512MiB \
  --title "Single line (512 MiB)" "$@"
