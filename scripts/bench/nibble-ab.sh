#!/usr/bin/env bash
# THROWAWAY (16-byte NEON words experiment). Wall-clock A/B of the nibble
# candidate (./qwc-nibble, QWC_NEON_NIBBLE=ON) vs baseline
# (./qwc-nibble-base, OFF) under LC_ALL=C, over the C-locale corpora and the
# Phase-1 flag set (the -w gate cells + layout sentinels). Remove with the
# branch if the gate fails. See
# qwc-companion/superpowers/plans/2026-06-21-16byte-neon-words-nibble.md.
set -euo pipefail
cd "$(dirname "$0")/../.."

[[ -x ./qwc-nibble && -x ./qwc-nibble-base ]] || {
  echo "build ./qwc-nibble (ON) and ./qwc-nibble-base (OFF) first" >&2
  exit 1
}

# Gate cells: -w, -l -w, default(bare). Sentinels: -l, -c, -L.
FLAGS="${FLAGS:--w,-l -w,-l,-c,-L,}"
RUNS="${RUNS:-20}"
WARMUP="${WARMUP:-3}"
LOCALES="${LOCALES:-C}"
CORPORA="${CORPORA:-mixed short long single-line big.txt many}"

for corpus in $CORPORA; do
  echo "===== corpus: $corpus (LC_ALL=$LOCALES) ====="
  uv run python benchmarks/bench.py \
    --qwc ./qwc-nibble --qwc-main ./qwc-nibble-base --qwc-main-name baseline \
    --no-competitors --locales "$LOCALES" --warmup "$WARMUP" --runs "$RUNS" \
    --flags="$FLAGS" \
    --data "benchmarks/test-data/$corpus" \
    --title "nibble A/B: $corpus"
done
