#!/usr/bin/env python3
"""Differential conformance for `qwc --validate-csv`.

Generates random CSVs across both dialects (RFC-4180 doubled-quote and
backslash-escape) and asserts the qwc binary's verdict -- exit code and, when
invalid, the `bad row: N` line -- matches an independent Python oracle, under
both LC_ALL=C and LC_ALL=C.UTF-8 (validation is byte-oriented, so the locale
must not matter). Optionally cross-checks a second binary (e.g. qwc-scalar).

Usage:
  python3 conformance/csv/diff_csv.py [--qwc ./build/qwc]
        [--qwc-scalar ./build/qwc-scalar] [--iters 400] [--seed 1]

This is an independent third implementation of the semantics already pinned by
the C++ unit/fuzz tests (tests/validatecsv_test.cpp); a mismatch here points at
either the binary's CLI wiring or a genuine semantic divergence.
"""
import argparse
import random
import subprocess
import sys
import tempfile

WORDS = ["a", "bb", "ccc", "x1", "y22", "z333"]


def oracle(data, delim, quote, esc, quoting):
    """Mirror of validateCsvBuffer; returns (valid, bad_row_1based)."""
    inq = False
    escd = False
    rhc = False
    delims = 0
    row = 0
    expected = None
    for b in data:
        if escd:
            escd = False
            rhc = True
            continue
        if quoting and esc is not None and b == esc:
            escd = True
            rhc = True
            continue
        if quoting and b == quote:
            inq = not inq
            rhc = True
            continue
        if not inq and b == delim:
            delims += 1
            rhc = True
            continue
        if not inq and b == 0x0A:  # '\n'
            if expected is None:
                expected = delims
            elif delims != expected:
                return (False, row + 1)
            row += 1
            delims = 0
            rhc = False
            continue
        rhc = True
    if inq:
        return (False, row + 1)
    if rhc and expected is not None and delims != expected:
        return (False, row + 1)
    return (True, 0)


def rand_field(rng, backslash):
    inner = rng.choice(WORDS)
    if rng.random() < 0.5:
        return inner  # plain
    kind = rng.randint(0, 3)
    if kind == 0:
        inner += "," + rng.choice(WORDS)        # embedded delimiter
    elif kind == 1:
        inner += "\n" + rng.choice(WORDS)       # embedded newline
    elif kind == 2:
        inner += '""' + rng.choice(WORDS)       # doubled quote
    elif kind == 3 and backslash:
        inner += '\\"' + rng.choice(WORDS)      # escaped quote
    return '"' + inner + '"'


def gen_csv(rng, backslash):
    fields = rng.randint(1, 5)
    records = rng.randint(1, 20)
    ragged_at = rng.randint(0, records - 1) if rng.random() < 0.25 else -1
    rows = []
    for r in range(records):
        fc = fields
        if r == ragged_at:
            fc = fields - 1 if (fields > 1 and rng.random() < 0.5) else fields + 1
        rows.append(",".join(rand_field(rng, backslash) for _ in range(fc)))
    out = "\n".join(rows) + "\n"
    if rng.random() < 0.2:
        out = out[:-1]                       # drop final newline
    if rng.random() < 0.05:
        out += '"unterminated'               # rare open quote at EOF
    return out.encode()


def run(binary, data, backslash, locale):
    with tempfile.NamedTemporaryFile(suffix=".csv", delete=True) as tf:
        tf.write(data)
        tf.flush()
        args = [binary, "--validate-csv", "--first"]
        if backslash:
            args.append("--esc=\\")
        args.append(tf.name)
        env = {"LC_ALL": locale, "PATH": "/usr/bin:/bin"}
        p = subprocess.run(args, capture_output=True, env=env)
    row = None
    if p.returncode == 1:
        # `--first` prints one line per invalid file: "<name>: <row>"
        text = (p.stdout + p.stderr).decode("utf-8", "replace").strip()
        if text and ":" in text:
            try:
                row = int(text.rsplit(":", 1)[1].strip())
            except ValueError:
                pass
    return p.returncode, row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qwc", default="./build/qwc")
    ap.add_argument("--qwc-scalar", default=None)
    ap.add_argument("--iters", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    binaries = [args.qwc] + ([args.qwc_scalar] if args.qwc_scalar else [])
    fails = 0
    for it in range(args.iters):
        backslash = bool(it & 1)
        data = gen_csv(rng, backslash)
        esc = 0x5C if backslash else None  # '\\'
        ok, bad = oracle(data, 0x2C, 0x22, esc, quoting=True)
        want_rc = 0 if ok else 1
        for binary in binaries:
            for locale in ("C", "C.UTF-8"):
                rc, row = run(binary, data, backslash, locale)
                if rc != want_rc or (not ok and row != bad):
                    fails += 1
                    print(f"MISMATCH it={it} bin={binary} loc={locale} "
                          f"want=({want_rc},{bad}) got=({rc},{row})\n"
                          f"  data={data!r}", file=sys.stderr)
                    if fails > 20:
                        print("too many failures, aborting", file=sys.stderr)
                        return 1
    if fails:
        print(f"{fails} failed", file=sys.stderr)
        return 1
    print(f"{args.iters} cases x {len(binaries)} binaries x 2 locales: all matched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
