#!/usr/bin/env python3
"""Deterministic CSV corpus generator for the `qwc --validate-csv` benchmark.

Produces rectangular (and one deliberately-ragged) CSV files of a target size,
in four shapes that exercise the two-phase validator differently:

  unquoted  -- no quote bytes at all: the Phase-1-only fast path, and the
               dominant real-world case. The headline benchmark cell.
  sprinkled -- one quoted free-text column (~1 of 6) carrying embedded commas
               and newlines: a minority of chunks turn "dirty" (Phase 2).
  heavy     -- every field quoted: almost every chunk is dirty.
  ragged    -- like unquoted but one row near the end has a missing field, so
               the validator must reject (and, in default mode, name the row).

The same --seed and --size always yield identical bytes. Files are written to
benchmarks/test-data/ by default.
"""
import argparse
import os
import random
import sys

WORDS = [
    "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
    "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa",
]


def token(rng):
    return rng.choice(WORDS) + str(rng.randint(0, 9999))


def quoted_freetext(rng):
    # A quoted field with embedded commas and the occasional embedded newline,
    # so the value spans column- and row-delimiters that must stay masked.
    parts = [rng.choice(WORDS) for _ in range(rng.randint(2, 5))]
    sep = ", " if rng.random() < 0.7 else "\n"
    return '"' + sep.join(parts) + '"'


def make_row(rng, shape):
    if shape == "heavy":
        return ",".join('"' + token(rng) + '"' for _ in range(6))
    if shape == "sprinkled":
        cols = [token(rng) for _ in range(5)]
        cols.insert(2, quoted_freetext(rng))  # one quoted column of six
        return ",".join(cols)
    return ",".join(token(rng) for _ in range(6))  # unquoted / ragged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shape", required=True,
                    choices=["unquoted", "sprinkled", "heavy", "ragged"])
    ap.add_argument("--size", type=int, default=256 * 1024 * 1024,
                    help="approximate target size in bytes")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    here = os.path.dirname(os.path.abspath(__file__))
    out = args.out or os.path.join(here, "test-data", f"csv-{args.shape}.csv")
    os.makedirs(os.path.dirname(out), exist_ok=True)

    written = 0
    buf = []
    buf_bytes = 0
    with open(out, "w", encoding="utf-8") as f:
        f.write("col0,col1,col2,col3,col4,col5\n")  # header (6 fields)
        while written < args.size:
            row = make_row(rng, args.shape) + "\n"
            buf.append(row)
            buf_bytes += len(row)
            written += len(row)
            if buf_bytes >= 1 << 20:  # flush every ~1 MiB
                f.write("".join(buf))
                buf.clear()
                buf_bytes = 0
        if args.shape == "ragged":
            buf.append("oops,missing,a,field\n")  # 4 fields, not 6: bad row
        f.write("".join(buf))

    print(f"wrote {out} ({os.path.getsize(out)} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
