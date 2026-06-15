#!/usr/bin/env bash
# Run benchmarks/bench.py against the many-small-files corpus.
set -euo pipefail
exec python3 benchmarks/bench.py \
  --qwc ./qwc --qwc-main ./qwc-main --qwc-main-name latest-release \
  --warmup 1 \
  --data benchmarks/test-data/many \
  --title "Many small files" "$@"
