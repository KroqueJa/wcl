#!/usr/bin/env bash
# Run benchmarks/bench.py against the single big-file corpus.
set -euo pipefail
exec python3 benchmarks/bench.py \
  --qwc ./qwc --qwc-main ./qwc-main --qwc-main-name latest-release \
  --warmup 1 \
  --data benchmarks/test-data/big.txt \
  --title "Single large file" "$@"
