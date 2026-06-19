#!/usr/bin/env bash
# Run benchmarks/bench.py against the long-lines corpus (256 MiB).
set -euo pipefail
"$(dirname "$0")/sync-current-build.sh"
"$(dirname "$0")/sync-latest-release.sh"
exec python3 benchmarks/bench.py \
  --qwc ./qwc --qwc-main ./qwc-latest-release --qwc-main-name latest-release \
  --warmup 1 \
  --data benchmarks/test-data/long \
  --title "Long lines (256 MiB)" "$@"
