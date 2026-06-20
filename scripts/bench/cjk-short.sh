#!/usr/bin/env bash
# Run benchmarks/bench.py against the CJK 3-byte UTF-8 short-lines
# corpus (256 MiB). Mirrors big.sh / short.sh.
set -euo pipefail
"$(dirname "$0")/sync-current-build.sh"
"$(dirname "$0")/sync-latest-release.sh"
exec python3 benchmarks/bench.py \
  --qwc ./qwc --qwc-main ./qwc-latest-release --qwc-main-name latest-release \
  --warmup 1 \
  --data benchmarks/test-data/cjk-short.txt \
  --title "CJK 3-byte UTF-8 short lines (256 MiB)" "$@"
