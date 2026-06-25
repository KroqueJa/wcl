#!/usr/bin/env python3
"""Deterministic CSV corpus generator for the `qwc --validate-csv` benchmark.

Produces rectangular (and deliberately-ragged) CSV files of a target size,
in shapes that exercise the two-phase validator differently:

  unquoted         -- no quote bytes at all: the Phase-1-only fast path, and the
                      dominant real-world case. The headline benchmark cell.
  sprinkled        -- one quoted free-text column (~1 of 6) carrying embedded
                      delimiters and newlines: a minority of chunks turn "dirty"
                      (Phase 2).
  heavy            -- every field quoted: almost every chunk is dirty.
  ragged           -- like unquoted but ragged (wrong field count) rows force a
                      reject. With --ragged-density 1 (default) the single bad row
                      is the very last line -- the worst case for the default-mode
                      inspection re-scan (Finding 18). With --ragged-density N>1 the
                      N bad rows are scattered evenly through the file, modelling a
                      real-world dirty export (the case where default `--all` must
                      name every bad row).
  ragged-everywhere -- every data row is ragged: the pathological enumeration
                      workload, where essentially every chunk holds violations.

The delimiter is selectable with --delim (e.g. '|' for the pipe-separated
real-world shape); the same value must be passed to qwc as `-d` and to
`zsv check` as `--delimiter`.

The same --seed / --size / --delim / --ragged-density always yield identical
bytes. Files are written to benchmarks/test-data/ by default.
"""
import argparse
import os
import random
import sys

WORDS = [
    "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
    "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa",
]

NFIELDS = 6  # field count of a well-formed record (and of the header)

# Filename tags for non-default delimiters, so a pipe corpus does not clobber the
# comma one. Anything else falls back to "-d<ordinal>".
DELIM_TAG = {",": "", "|": "-pipe", "\t": "-tab", ";": "-semi"}


def token(rng):
    return rng.choice(WORDS) + str(rng.randint(0, 9999))


def quoted_freetext(rng, delim, backslash=False):
    # A quoted field with embedded delimiters and the occasional embedded
    # newline, so the value spans column- and row-delimiters that must stay
    # masked.
    parts = [rng.choice(WORDS) for _ in range(rng.randint(2, 5))]
    sep = (delim + " ") if rng.random() < 0.7 else "\n"
    inner = sep.join(parts)
    if backslash:
        # Backslash-escaped quote and delimiter inside the quoted field: the
        # validator must mask these so they don't toggle / split.
        inner += '\\"' + rng.choice(WORDS) + "\\" + delim + rng.choice(WORDS)
    return '"' + inner + '"'


def make_row(rng, shape, delim, backslash):
    if shape == "heavy":
        return delim.join(
            quoted_freetext(rng, delim, backslash) for _ in range(NFIELDS))
    if shape == "sprinkled":
        cols = [token(rng) for _ in range(NFIELDS - 1)]
        cols.insert(2, quoted_freetext(rng, delim, backslash))  # one quoted col
        return delim.join(cols)
    return delim.join(token(rng) for _ in range(NFIELDS))  # unquoted / ragged


def make_ragged_row(rng, delim):
    # A row whose field count differs from the NFIELDS reference (so it is never
    # accidentally well-formed). Unquoted-style: raggedness is about field count,
    # not quoting.
    n = max(1, rng.choice([NFIELDS - 2, NFIELDS - 1, NFIELDS + 1, NFIELDS + 2]))
    return delim.join(token(rng) for _ in range(n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shape", required=True,
                    choices=["unquoted", "sprinkled", "heavy", "ragged",
                             "ragged-everywhere"])
    ap.add_argument("--size", type=int, default=256 * 1024 * 1024,
                    help="approximate target size in bytes")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--delim", default=",",
                    help="field delimiter (one byte; e.g. '|' for pipe-separated)")
    ap.add_argument("--ragged-density", type=int, default=1,
                    help="for --shape ragged: number of bad rows. 1 (default) "
                         "puts the single bad row last; N>1 scatters N evenly")
    ap.add_argument("--backslash", action="store_true",
                    help="quoted fields use backslash escapes (validate with --esc)")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    if len(args.delim) != 1:
        ap.error("--delim must be exactly one byte")
    if args.ragged_density < 1:
        ap.error("--ragged-density must be >= 1")
    delim = args.delim

    rng = random.Random(args.seed)
    here = os.path.dirname(os.path.abspath(__file__))
    suffix = DELIM_TAG.get(delim, f"-d{ord(delim)}")
    suffix += "-esc" if args.backslash else ""
    if args.shape == "ragged" and args.ragged_density > 1:
        suffix += f"-x{args.ragged_density}"
    out = args.out or os.path.join(
        here, "test-data", f"csv-{args.shape}{suffix}.csv")
    os.makedirs(os.path.dirname(out), exist_ok=True)

    header = delim.join(f"col{i}" for i in range(NFIELDS))

    # Scattered bad-row byte offsets for `ragged` with density > 1: N evenly
    # spaced interior thresholds. (density 1 is handled as a trailing bad row
    # after the loop, preserving the Finding-18 worst-case corpus.)
    thresholds = []
    if args.shape == "ragged" and args.ragged_density > 1:
        thresholds = [args.size * i // (args.ragged_density + 1)
                      for i in range(1, args.ragged_density + 1)]
    next_threshold = 0
    bad_emitted = 0

    written = 0
    buf = []
    buf_bytes = 0
    with open(out, "w", encoding="utf-8") as f:
        f.write(header + "\n")  # header (NFIELDS fields) = record 1 = reference
        while written < args.size:
            if args.shape == "ragged-everywhere":
                row = make_ragged_row(rng, delim) + "\n"
                bad_emitted += 1
            elif (next_threshold < len(thresholds)
                  and written >= thresholds[next_threshold]):
                row = make_ragged_row(rng, delim) + "\n"
                next_threshold += 1
                bad_emitted += 1
            else:
                row = make_row(rng, args.shape, delim, args.backslash) + "\n"
            buf.append(row)
            buf_bytes += len(row)
            written += len(row)
            if buf_bytes >= 1 << 20:  # flush every ~1 MiB
                f.write("".join(buf))
                buf.clear()
                buf_bytes = 0
        if args.shape == "ragged" and args.ragged_density <= 1:
            # Worst case for the default-mode re-scan: a lone bad row as the very
            # last line, so naming it costs ~a full extra pass (Finding 18).
            buf.append(make_ragged_row(rng, delim) + "\n")
            bad_emitted += 1
        f.write("".join(buf))

    note = f", {bad_emitted} bad rows" if bad_emitted else ""
    print(f"wrote {out} ({os.path.getsize(out)} bytes{note})", file=sys.stderr)


if __name__ == "__main__":
    main()
