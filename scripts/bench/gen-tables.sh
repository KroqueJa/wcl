#!/usr/bin/env bash
# Regenerate include/iswprint_table.h from the build machine's glibc.
set -euo pipefail
cd "$(dirname "$0")/../.."
gcc -O2 -o /tmp/qwc-gen-iswprint scripts/bench/gen-iswprint-table.c
/tmp/qwc-gen-iswprint > include/iswprint_table.h
echo "wrote include/iswprint_table.h"
