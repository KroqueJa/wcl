#!/usr/bin/env bash
# Regenerate include/iswprint_table.h from the build machine's glibc.
# The C generator emits compact `1,0,1,...` rows; we post-process with
# clang-format so the committed header satisfies scripts/check-format.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."
CLANG_FORMAT="${CLANG_FORMAT:-clang-format-22}"
gcc -O2 -o /tmp/qwc-gen-iswprint scripts/bench/gen-iswprint-table.c
/tmp/qwc-gen-iswprint > include/iswprint_table.h
"$CLANG_FORMAT" -i include/iswprint_table.h
echo "wrote include/iswprint_table.h (clang-format-clean)"
