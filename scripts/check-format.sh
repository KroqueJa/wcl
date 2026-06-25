#!/usr/bin/env bash
# Formatting gate over every hand-written C++ file (the generated
# include/iswprint_table.h is also covered: its generator emits
# clang-format-clean output, so it never trips the check).
#
#   scripts/check-format.sh          # check, non-zero exit on any violation
#   scripts/check-format.sh --fix    # reformat in place instead
#
# Pinned to clang-format-22: different majors disagree on the .clang-format
# rules, so local and CI must run the exact same binary or the gate flaps.
# CI runs the check form on every push/PR (the clang-format job in
# .github/workflows/analysis.yml).
set -euo pipefail
cd "$(dirname "$0")/.."

CLANG_FORMAT="${CLANG_FORMAT:-clang-format-22}"

files=( src/*.cpp include/*.h tests/*.cpp tests/*.h )

if [[ "${1:-}" == "--fix" ]]; then
  "$CLANG_FORMAT" -i "${files[@]}"
else
  "$CLANG_FORMAT" --dry-run -Werror "${files[@]}"
fi
