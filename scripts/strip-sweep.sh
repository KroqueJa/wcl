#!/usr/bin/env bash
# Strip-size sweep for the strip-mine change (Task 2). Builds a one-strip
# baseline (== pre-change behavior) and the candidate strip sizes from the
# CURRENT working tree (the uncommitted strip-mine edit), then benches each
# candidate vs the baseline across the corpora.
#
# macOS/NEON box: no `perf` here, so this is the wall-clock + NEON-confirmation
# leg. The perf-stat L1-miss mechanism leg runs on the Linux/AVX2 box.
#
# Run AFTER quieting the system (close apps, AC power). Throwaway script —
# do not commit it.
set -euo pipefail
cd /Users/jikrochmal/CLionProjects/qwc

DATA=benchmarks/test-data
CORPORA=(mixed-512MiB short-512MiB long-512MiB single-line-512MiB big.txt cjk-short.txt)
SIZES=(65536 32768 16384 8192)     # 64/32/16/8 KiB candidates
WARMUP=3
RUNS=20
LOCALE="${LOCALE:-C.UTF-8}"        # run again with LOCALE=C for the ASCII-fast-path picture
export LC_ALL="$LOCALE"

echo "== building one-strip baseline (./qwc-main) =="
cmake -S . -B build-main -DCMAKE_BUILD_TYPE=Release \
      -DQWC_DEFINES=QWC_STRIP_SIZE=262144 -DQWC_SUFFIX=main >/dev/null
cmake --build build-main --target qwc -j >/dev/null   # -> ./qwc-main

for s in "${SIZES[@]}"; do
  echo "== building candidate strip=$s (./qwc-s$s) =="
  cmake -S . -B "build-s$s" -DCMAKE_BUILD_TYPE=Release \
        -DQWC_DEFINES=QWC_STRIP_SIZE=$s -DQWC_SUFFIX=s$s >/dev/null
  cmake --build "build-s$s" --target qwc -j >/dev/null # -> ./qwc-s$s
done

OUT=strip-sweep-logs/$LOCALE
mkdir -p "$OUT"
echo "== benching (locale=$LOCALE, warmup=$WARMUP runs=$RUNS) =="
for s in "${SIZES[@]}"; do
  for c in "${CORPORA[@]}"; do
    echo "---- strip=$s  corpus=$c ----"
    python3 benchmarks/bench.py \
      --qwc "./qwc-s$s" --qwc-main "./qwc-main" --qwc-main-name "1strip" \
      --no-competitors --warmup "$WARMUP" --runs "$RUNS" \
      --data "$DATA/$c" \
      --flags "-l -w,,-l -c,-l,-m,-L" \
      --title "strip=$s vs 1strip  [$c, $LOCALE]" \
      --json-out "$OUT/strip$s-$c.json"
  done
done
echo
echo "DONE. JSON sidecars in $OUT/ — reopen Claude and point it at that dir."
