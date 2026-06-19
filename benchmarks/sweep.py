#!/usr/bin/env python3
"""qwc per-byte pipeline sweep driver.

Runs the QWC_BUF_SIZE matrix for the campaign described in
docs/superpowers/specs/2026-06-12-per-byte-pipeline-design.md. Two phases:

  walltime : hyperfine sweep across {size x workload x corpus x topology},
             with wc and uu-wc as comparators in each hyperfine invocation.
  perf     : `perf stat` on the candidate sizes for the headline workload
             (-l, big single file, 4-vCPU pinned), gathering the mechanism
             counters that confirm or refute H1.

Both phases write a markdown table and a JSON sidecar to OUT_DIR.

Pure stdlib + hyperfine + perf. Designed to be re-run on the same warmed
corpora -- generates no data of its own.
"""
import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Dict, List, Tuple

# Default sweep axes. The spec calls for 64K/128K/256K/512K/1M and adds 2M only
# if the curve is still falling at 1M -- start with the five and let the user
# rerun with --sizes if needed. Workloads -l and -m are the success criteria;
# -w is the no-regression sanity row.
DEFAULT_SIZES_KIB = [64, 128, 256, 512, 1024]
DEFAULT_FLAGS = ["-l", "-m", "-w"]
DEFAULT_TOPOLOGIES = [("4cores", "taskset -c 0-3 "), ("all", "")]
PERF_EVENTS = "cycles,instructions,LLC-loads,LLC-load-misses,minor-faults"


def sh(cmd, **kw):
    """Subprocess wrapper that surfaces failures with full stderr."""
    return subprocess.run(cmd, check=True, **kw)


def have(cmd: str) -> bool:
    if not cmd:
        return False
    return shutil.which(shlex.split(cmd)[0]) is not None


def fmt_size(kib: int) -> str:
    return f"{kib}K" if kib < 1024 else f"{kib // 1024}M"


def hyperfine_means(commands: List[Tuple[str, str]], warmup: int,
                    runs: int) -> List[float]:
    """Run one hyperfine over `commands` (label, cmdline); return mean seconds."""
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
        json_path = tf.name
    argv = ["hyperfine", "--shell=none", "--warmup", str(warmup),
            "--runs", str(runs), "--export-json", json_path]
    for label, cmdline in commands:
        argv += ["-n", label, cmdline]
    sh(argv, stdout=subprocess.DEVNULL)
    with open(json_path) as f:
        data = json.load(f)
    os.unlink(json_path)
    return [r["mean"] for r in data["results"]]


def build_targets(corpus_path: str, out_dir: str) -> Dict[str, str]:
    """The argv tail each tool gets to mean "count all files in this corpus".

    A file path: every tool takes the inline path -- one short arg, no risk.
    A directory: inline expansion of thousands of paths blows Linux's
    MAX_ARG_STRLEN (32 * PAGE_SIZE = 128 KiB *per argv string*; the smoke
    corpus produced a 174 KiB string and exec returned E2BIG). Tool-specific
    chunk-safe forms instead -- the only ones each tool supports:
      qwc:   `-r DIR`                 (recursive walk; flat dir -> same files)
      wc:    `--files0-from=MANIFEST` (NUL-separated path list)
      uu-wc: `--files0-from=MANIFEST` (same as wc)
    The walk adds one readdir() to qwc that wc doesn't pay -- irrelevant
    relative to scanning hundreds of MiB. Manifest is written once, stably
    named, into out_dir so reruns reuse it (and a reader can audit it)."""
    if os.path.isfile(corpus_path):
        q = shlex.quote(corpus_path)
        return {"qwc": q, "wc": q, "uuwc": q}
    files = sorted(os.path.join(corpus_path, f) for f in os.listdir(corpus_path)
                   if os.path.isfile(os.path.join(corpus_path, f)))
    base = os.path.basename(os.path.normpath(corpus_path))
    manifest = os.path.join(out_dir, f"{base}.manifest")
    with open(manifest, "wb") as fh:
        for p in files:
            fh.write(p.encode())
            fh.write(b"\0")
    qm = shlex.quote(manifest)
    return {
        "qwc":  f"-r {shlex.quote(corpus_path)}",
        "wc":   f"--files0-from={qm}",
        "uuwc": f"--files0-from={qm}",
    }


def walltime_phase(args) -> None:
    """Phase 1: the wall-clock sweep matrix."""
    corpora = [("big", args.big), ("many", args.many)]
    rows: List[Dict] = []

    for locale in args.locales:
        for corpus_label, corpus_path in corpora:
            tgt = build_targets(corpus_path, args.out)
            for flag in args.flags:
                for topo_label, topo_prefix in args.topologies:
                    # One hyperfine invocation = one row in the output: qwc at every
                    # size, plus wc and uu-wc, all benchmarked within the same warm
                    # cache pass for apples-to-apples comparison.
                    commands: List[Tuple[str, str]] = []
                    for kib in args.sizes:
                        # env -- sets the env for the one binary invocation;
                        # hyperfine spawns each command directly so env survives.
                        # LC_ALL pins the locale axis (the kernel branch in
                        # words/maxLineLen is decided once at qwc startup).
                        env = f"QWC_BUF_SIZE={fmt_size(kib)} LC_ALL={shlex.quote(locale)} "
                        label = f"qwc@{fmt_size(kib)}"
                        cmd = (f"{topo_prefix}env {env}{args.qwc} {flag} {tgt['qwc']}").strip()
                        commands.append((label, cmd))
                    # Comparators, same topology, same hyperfine warmup pass,
                    # same locale (so the qwc-vs-comparator ratio compares
                    # like-for-like inputs).
                    cmp_env = f"LC_ALL={shlex.quote(locale)} "
                    commands.append(("wc",
                        f"{topo_prefix}env {cmp_env}{args.gwc} {flag} {tgt['wc']}".strip()))
                    commands.append(("uu-wc",
                        f"{topo_prefix}env {cmp_env}{args.uuwc} {flag} {tgt['uuwc']}".strip()))

                    means = hyperfine_means(commands, args.warmup, args.runs)
                    row = {
                        "locale": locale,
                        "corpus": corpus_label,
                        "flag": flag,
                        "topology": topo_label,
                        "results": dict(zip([c[0] for c in commands], means)),
                    }
                    rows.append(row)
                    print(f"[walltime] {locale} {corpus_label} {flag} {topo_label}: "
                          + " ".join(f"{k}={v*1000:.1f}ms" for k, v in row["results"].items()),
                          file=sys.stderr)

    write_walltime_markdown(rows, args.sizes, args.out)
    with open(os.path.join(args.out, "sweep-walltime.json"), "w") as f:
        json.dump(rows, f, indent=2)


def write_walltime_markdown(rows: List[Dict], sizes: List[int], out_dir: str) -> None:
    """One table per (locale, corpus, topology), rows by flag, columns by size + comparators."""
    by_group: Dict[Tuple[str, str, str], List[Dict]] = {}
    for r in rows:
        by_group.setdefault((r["locale"], r["corpus"], r["topology"]), []).append(r)

    lines: List[str] = ["# qwc scan-buffer sweep: wall-clock (ms)\n"]
    for (locale, corpus, topo), group in by_group.items():
        lines.append(f"## locale={locale} · corpus={corpus} · topology={topo}\n")
        size_labels = [f"qwc@{fmt_size(k)}" for k in sizes]
        cols = ["flag"] + size_labels + ["wc", "uu-wc"]
        lines.append("| " + " | ".join(cols) + " |")
        lines.append("|" + "|".join(["---"] * len(cols)) + "|")
        for r in group:
            cells = [r["flag"]]
            for col in cols[1:]:
                v = r["results"].get(col)
                cells.append("-" if v is None else f"{v*1000:.1f}")
            lines.append("| " + " | ".join(cells) + " |")
        lines.append("")

    out_path = os.path.join(out_dir, "sweep-walltime.md")
    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    print(f"[walltime] wrote {out_path}", file=sys.stderr)


def perf_phase(args) -> None:
    """Phase 2: mechanism counters for the headline workload."""
    tgt = build_targets(args.big, args.out)
    rows: List[Dict] = []
    topo_prefix = "taskset -c 0-3 "  # the runner-mimic topology; H1's regime

    for kib in args.sizes:
        env = f"QWC_BUF_SIZE={fmt_size(kib)} "
        cmd = f"{topo_prefix}perf stat -x, -e {PERF_EVENTS} env {env}{args.qwc} -l {tgt['qwc']}"
        # Warm pass (uncounted) + perf pass.
        sh(["sh", "-c", f"{topo_prefix}env {env}{args.qwc} -l {tgt['qwc']} > /dev/null"])
        proc = subprocess.run(["sh", "-c", cmd], capture_output=True, text=True)
        if proc.returncode != 0:
            print(proc.stderr, file=sys.stderr)
            raise SystemExit(f"perf stat failed at size {kib}K -- "
                             "check /proc/sys/kernel/perf_event_paranoid")
        counters = parse_perf_csv(proc.stderr)
        rows.append({"size_kib": kib, "counters": counters})
        print(f"[perf] size={fmt_size(kib)}: {counters}", file=sys.stderr)

    # Also one comparator row for uu-wc, same workload, same topology.
    cmd = f"{topo_prefix}perf stat -x, -e {PERF_EVENTS} {args.uuwc} -l {tgt['uuwc']}"
    sh(["sh", "-c", f"{topo_prefix}{args.uuwc} -l {tgt['uuwc']} > /dev/null"])
    proc = subprocess.run(["sh", "-c", cmd], capture_output=True, text=True)
    if proc.returncode == 0:
        rows.append({"size_kib": "uu-wc", "counters": parse_perf_csv(proc.stderr)})

    write_perf_markdown(rows, args.out)
    with open(os.path.join(args.out, "sweep-perf.json"), "w") as f:
        json.dump(rows, f, indent=2)


def parse_perf_csv(stderr: str) -> Dict[str, float]:
    """`perf stat -x,` writes CSV to stderr: value,unit,event,run-time,...
    Lines beginning with a non-digit (#, blank, time-elapsed line) are skipped.
    Strip the `:u` / `:k` / `:uk` event suffix that perf adds under
    perf_event_paranoid >= 2, so downstream lookups by the requested event
    name (e.g. `cycles`) work either way."""
    out: Dict[str, float] = {}
    for line in stderr.splitlines():
        if not line or not line[0].isdigit():
            continue
        parts = line.split(",")
        if len(parts) < 3:
            continue
        try:
            value = float(parts[0])
        except ValueError:
            continue
        event = parts[2].strip().split(":", 1)[0]
        out[event] = value
    return out


def write_perf_markdown(rows: List[Dict], out_dir: str) -> None:
    cols = ["size"] + PERF_EVENTS.split(",")
    lines: List[str] = ["# qwc scan-buffer sweep: mechanism counters (big file, -l, 4-vCPU pinned)\n",
                        "| " + " | ".join(cols) + " |",
                        "|" + "|".join(["---"] * len(cols)) + "|"]
    for r in rows:
        size_cell = (fmt_size(r["size_kib"]) if isinstance(r["size_kib"], int)
                     else str(r["size_kib"]))
        cells = [size_cell]
        for ev in cols[1:]:
            v = r["counters"].get(ev)
            cells.append("-" if v is None else f"{v:.0f}")
        lines.append("| " + " | ".join(cells) + " |")
    out_path = os.path.join(out_dir, "sweep-perf.md")
    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    print(f"[perf] wrote {out_path}", file=sys.stderr)


def parse_sizes(text: str) -> List[int]:
    """Parse a comma-separated KiB list, allowing K/M suffix on the values."""
    out: List[int] = []
    for tok in text.split(","):
        tok = tok.strip().lower()
        if tok.endswith("m"):
            out.append(int(tok[:-1]) * 1024)
        elif tok.endswith("k"):
            out.append(int(tok[:-1]))
        else:
            out.append(int(tok))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qwc", required=True, help="qwc binary under test")
    ap.add_argument("--big", required=True, help="single big-file corpus path")
    ap.add_argument("--many", required=True, help="many-files corpus directory")
    ap.add_argument("--out", required=True, help="output directory for tables + JSON")
    ap.add_argument("--gwc", default="wc", help="GNU wc invocation (default: wc)")
    ap.add_argument("--uuwc", default="uu-wc", help="uutils wc invocation (default: uu-wc)")
    ap.add_argument("--sizes", default=",".join(f"{k}K" for k in DEFAULT_SIZES_KIB),
                    help="comma-separated buffer sizes (e.g. 64K,128K,256K,512K,1M,2M)")
    ap.add_argument("--flags", default=",".join(DEFAULT_FLAGS),
                    help="comma-separated qwc flag sets (default: -l,-m,-w)")
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--phase", choices=["walltime", "perf", "both"], default="both")
    ap.add_argument("--locales", default="C,C.UTF-8",
                    help="comma-separated LC_ALL values to measure each cell "
                         "under. Default 'C,C.UTF-8' runs both qwc kernel "
                         "families. Pass a single value (e.g. 'C') for legacy "
                         "single-locale sweeps.")
    args = ap.parse_args()

    if not have(args.gwc):
        sys.exit(f"missing GNU wc invocation: {args.gwc!r}")
    if not have(args.uuwc):
        sys.exit(f"missing uutils wc invocation: {args.uuwc!r}")
    os.makedirs(args.out, exist_ok=True)

    args.sizes = parse_sizes(args.sizes)
    args.flags = [f.strip() for f in args.flags.split(",") if f.strip()]
    args.topologies = DEFAULT_TOPOLOGIES
    args.locales = [loc.strip() for loc in args.locales.split(",") if loc.strip()]
    if not args.locales:
        sys.exit("--locales: empty list")

    if args.phase in ("walltime", "both"):
        walltime_phase(args)
    if args.phase in ("perf", "both"):
        perf_phase(args)


if __name__ == "__main__":
    main()
