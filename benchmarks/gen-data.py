#!/usr/bin/env python3
"""Deterministic UTF-8 corpus generator for the qwc benchmark suite.

Produces realistic `wc`-shaped text -- space-separated words, newline-terminated
lines, a sprinkling of multibyte UTF-8 so that `-m` differs from `-c` and
character-mode `-L` is exercised, and the occasional long line so `-L` is not
constant. The same --seed (and size args) always yield identical bytes, so
branch and main are measured on exactly the same input.

Two corpus shapes:
  * default      -- one file of --size (stresses in-file chunk parallelism + the
                    SIMD kernels).
  * --many       -- a directory of many files totalling ~--size, with sizes drawn
                    LOG-uniformly from [--min-file, --max-file] (stresses the
                    per-file open/fstat dispatch). Log-uniform spans the size
                    regimes evenly per octave, so the benchmark samples the whole
                    per-file-overhead-vs-throughput curve instead of one point.

Deliberately NOT random binary/control-byte data: that is not meaningful for
wc-style tools (the lesson from the legacy generator this replaces).
"""
import argparse
import math
import os
import random
import sys

# A small pool of multibyte tokens (Latin accents, a few CJK) mixed into the
# otherwise-ASCII word stream. Kept modest so the byte/char ratio stays realistic.
MULTIBYTE_WORDS = [
    "café", "naïve", "résumé", "Über", "jalapeño", "façade", "Zürich",
    "Москва", "日本語", "你好", "안녕하세요", "Ελλάδα", "emoji😀here",
]
ASCII_ALPHABET = "abcdefghijklmnopqrstuvwxyz"


def parse_size(text: str) -> int:
    """Parse a byte count: plain int, or with a KB/MB/GB or KiB/MiB/GiB suffix."""
    t = text.strip()
    units = {
        "kib": 1024, "mib": 1024**2, "gib": 1024**3,
        "kb": 1000, "mb": 1000**2, "gb": 1000**3,
        "k": 1024, "m": 1024**2, "g": 1024**3, "b": 1,
    }
    low = t.lower()
    for suffix, mult in sorted(units.items(), key=lambda kv: -len(kv[0])):
        if low.endswith(suffix):
            return int(float(low[: -len(suffix)]) * mult)
    return int(t)


def build_word_pool(rng: random.Random, n: int, multibyte_fraction: float) -> list:
    """Pre-generate a pool of words to sample from -- far faster than minting a
    fresh random word per token when writing hundreds of MiB."""
    pool = []
    for _ in range(n):
        if rng.random() < multibyte_fraction:
            pool.append(rng.choice(MULTIBYTE_WORDS))
        else:
            length = rng.randint(2, 12)
            pool.append("".join(rng.choices(ASCII_ALPHABET, k=length)))
    return pool


def write_text(f, target: int, pool: list, rng: random.Random,
               shape: str = "mixed") -> int:
    """Write newline-terminated word-lines to `f` until ~`target` bytes. Returns
    the number of characters produced (a close proxy for bytes; multibyte words
    make the file marginally larger). Termination counts the pending buffer too,
    so it works for KiB-sized targets, not just multi-MiB ones.

    `shape` selects the line-length distribution:
      mixed       -- the historical default: 3-18 words/line, with 1% spikes
                     of 80-200 words. Default so existing corpora are unchanged.
      short       -- 40-80 byte lines, no spikes. Hit-fallback stress.
      long        -- 2-8 KiB lines, no spikes. Clean-path stress.
      single-line -- one line of `target` bytes ending in a newline. Pathological
                     best-case ceiling."""
    buf = []
    pending = 0   # chars buffered since the last flush
    produced = 0  # total chars produced so far
    FLUSH = 1 << 20

    if shape == "single-line":
        # One long line; we still flush in chunks to keep RAM bounded. The
        # final newline is appended after the body.
        body_remaining = max(0, target - 1)
        chunk_words = 256
        while body_remaining > 0:
            line = " ".join(rng.choices(pool, k=chunk_words))
            # Strip a trailing space if the byte budget is tight; close enough.
            data = line[:body_remaining] if len(line) > body_remaining else line + " "
            buf.append(data)
            pending += len(data)
            produced += len(data)
            body_remaining -= len(data)
            if pending >= FLUSH:
                f.write("".join(buf).encode("utf-8"))
                buf = []
                pending = 0
        buf.append("\n")
        produced += 1
        f.write("".join(buf).encode("utf-8"))
        return produced

    while produced < target:
        if shape == "short":
            n_words = rng.randint(4, 10)        # ~40-80 bytes
        elif shape == "long":
            n_words = rng.randint(300, 1200)    # ~2-8 KiB
        else:  # mixed -- historical default
            # ~1% of lines are long (so -L is non-trivial); the rest are normal.
            n_words = (rng.randint(80, 200)
                       if rng.random() < 0.01
                       else rng.randint(3, 18))
        line = " ".join(rng.choices(pool, k=n_words)) + "\n"
        buf.append(line)
        pending += len(line)
        produced += len(line)
        if pending >= FLUSH:
            f.write("".join(buf).encode("utf-8"))
            buf = []
            pending = 0
    if buf:
        f.write("".join(buf).encode("utf-8"))
    return produced


def generate_single(out_path: str, size: int, seed: int, mbfrac: float,
                    shape: str = "mixed") -> None:
    rng = random.Random(seed)
    pool = build_word_pool(rng, 50_000, mbfrac)
    with open(out_path, "wb") as f:
        write_text(f, size, pool, rng, shape)
    actual = os.path.getsize(out_path)
    print(f"wrote {actual} bytes ({actual / 1024**2:.1f} MiB) to {out_path} "
          f"(seed={seed}, shape={shape})", file=sys.stderr)


def generate_many(out_dir: str, total: int, seed: int, mbfrac: float,
                  min_file: int, max_file: int, shape: str = "mixed") -> None:
    rng = random.Random(seed)
    pool = build_word_pool(rng, 50_000, mbfrac)
    os.makedirs(out_dir, exist_ok=True)

    lo, hi = math.log(min_file), math.log(max_file)
    produced = 0
    idx = 0
    # Zero-padded names so a shell/Python sort gives a stable, predictable order.
    while produced < total:
        fsize = int(math.exp(rng.uniform(lo, hi)))
        fsize = max(min_file, min(max_file, fsize))
        path = os.path.join(out_dir, f"f{idx:07d}")
        with open(path, "wb") as f:
            produced += write_text(f, fsize, pool, rng, shape)
        idx += 1

    print(f"wrote {idx} files (~{produced / 1024**2:.1f} MiB) to {out_dir}/ "
          f"with log-uniform sizes in [{min_file}, {max_file}] bytes "
          f"(seed={seed}, shape={shape})", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", default="512MiB",
                    help="total target size, e.g. 512MiB, 1GB (default 512MiB)")
    ap.add_argument("--seed", type=int, default=20260610,
                    help="RNG seed (default fixed, for determinism)")
    ap.add_argument("--multibyte-fraction", type=float, default=0.08,
                    help="fraction of pool words that are multibyte (default 0.08)")
    ap.add_argument("--out", required=True,
                    help="output file (default) or directory (--many)")
    ap.add_argument("--many", action="store_true",
                    help="generate many varied-size files into --out (a directory)")
    ap.add_argument("--min-file", default="4KiB",
                    help="--many: smallest file size (default 4KiB)")
    ap.add_argument("--max-file", default="1MiB",
                    help="--many: largest file size (default 1MiB)")
    ap.add_argument("--line-length", default="mixed",
                    choices=["short", "mixed", "long", "single-line"],
                    help=("line-length distribution: short (40-80 B), mixed "
                          "(default, historical 3-18 word lines with 1%% long "
                          "spikes), long (2-8 KiB), single-line (one line)"))
    args = ap.parse_args()

    if args.many:
        generate_many(args.out, parse_size(args.size), args.seed,
                      args.multibyte_fraction, parse_size(args.min_file),
                      parse_size(args.max_file), args.line_length)
    else:
        generate_single(args.out, parse_size(args.size), args.seed,
                        args.multibyte_fraction, args.line_length)


if __name__ == "__main__":
    main()
