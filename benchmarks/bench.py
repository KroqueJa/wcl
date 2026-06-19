#!/usr/bin/env python3
"""qwc benchmark orchestrator.

Runs hyperfine across a matrix of wc flags, comparing the branch qwc against:
  - a qwc baseline (the "did this branch improve?" signal; optional). Typically
    qwc @ origin/main; on main runs the workflow points this at the latest
    published release binary instead, so main still carries a regression
    signal across release cycles. The rendered column label is overridable
    with --qwc-main-name so it reads honestly in either case.
  - uu-wc        (uutils coreutils, the closest competitor)
  - GNU wc       (the headline number)

Renders a markdown table of mean times plus relative-speedup columns. Prints to
stdout and, when running under GitHub Actions, appends to $GITHUB_STEP_SUMMARY.

Speed only -- correctness/parity with wc is the conformance suite's job (and
qwc's -L intentionally diverges from GNU wc on wide characters).
"""
import argparse
import glob
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

# Default flag matrix. '' is the bare invocation (wc prints lines/words/bytes).
# -c is kept for completeness; all three tools take the fstat fast path for it,
# so it mostly measures process startup.
# `-l -w`, `-l -L`, `-l -w -m` cover the planned-fusion combos in TODO.md's
# `Next` list. Before each fusion lands, the cell establishes the two-pass
# baseline; after, it's the regression-monitoring surface. `-L -m` is the
# existing fused cell. The `-c` combos and `-wL` are deliberately not in the
# matrix -- we are not committing to ship every flag combination as a fused
# kernel.
DEFAULT_FLAGS = ["", "-l", "-w", "-c", "-m", "-L", "-L -m",
                 "-l -w", "-l -L", "-l -w -m"]


def have(cmd: str) -> bool:
    """Is the first token of a command string an executable on PATH?"""
    if not cmd:
        return False
    return shutil.which(shlex.split(cmd)[0]) is not None


def validate_locales(locales: list) -> None:
    """Hard-error if `locale -a` is available and any requested locale isn't
    listed. Warn-and-proceed if `locale -a` itself isn't available (musl /
    minimal containers). Prevents silent fall-through to the C default
    mid-sweep when the user typo'd a locale name.

    `locale -a` output varies by distro: glibc canonicalises 'C.UTF-8' as
    'C.utf8', musl writes 'C.UTF-8' as-is, macOS writes 'C.UTF-8'. Compare
    lowercased and with '-' / '_' stripped so the aliases match."""
    try:
        proc = subprocess.run(["locale", "-a"], capture_output=True,
                              text=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        print("note: 'locale -a' unavailable; skipping locale validation",
              file=sys.stderr)
        return

    def norm(s: str) -> str:
        return s.lower().replace("-", "").replace("_", "")

    raw_available = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    available_norm = {norm(loc) for loc in raw_available}
    missing = [loc for loc in locales if norm(loc) not in available_norm]
    if missing:
        sys.exit(f"missing locale(s) on this host: {', '.join(missing)}. "
                 f"Install with localedef / locale-gen, or pass --locales "
                 f"with a subset of: {', '.join(sorted(raw_available))}")


def run_hyperfine(commands, warmup: int, runs: int,
                  env: dict | None = None) -> list:
    """Run one hyperfine invocation over `commands` (list of (label, cmdline));
    return the per-command mean times in seconds, in the same order. If `env`
    is given it replaces the child process environment; hyperfine inherits it
    and propagates via --shell=none exec to every tool subprocess."""
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
        json_path = tf.name
    # --shell=none: hyperfine parses the command itself and execs directly, so
    # there is no per-run shell startup polluting the (often sub-10ms) timings.
    # Every command prefix here is a real executable, so this is safe.
    argv = ["hyperfine", "--shell=none", "--warmup", str(warmup),
            "--runs", str(runs), "--export-json", json_path]
    for label, cmdline in commands:
        argv += ["-n", label, cmdline]
    subprocess.run(argv, check=True, stdout=subprocess.DEVNULL, env=env)
    with open(json_path) as f:
        data = json.load(f)
    os.unlink(json_path)
    return [r["mean"] for r in data["results"]]


def fmt_ratio(slower: float, faster: float) -> str:
    """Speedup of `faster` relative to `slower` (>1 means faster is quicker)."""
    if faster <= 0:
        return "-"
    return f"{slower / faster:.2f}x"


def describe_host() -> str:
    """A one-line host summary. The logical-CPU count is the key context for
    reading the table: qwc threads to hardware_concurrency(), so its win on the
    bandwidth-bound flags (-l, -c) is largely a parallelism win and collapses on
    a core-starved runner. Degrades gracefully off Linux."""
    logical = os.cpu_count() or 0
    model = ""
    mem_gib = None
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    model = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal"):
                    mem_gib = int(line.split()[1]) / 1024 / 1024
                    break
    except OSError:
        pass
    parts = [f"{logical} logical CPUs (qwc threads to this)"]
    if model:
        parts.append(model)
    if mem_gib:
        parts.append(f"{mem_gib:.1f} GiB RAM")
    return " · ".join(parts)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qwc", required=True, help="branch qwc binary")
    ap.add_argument("--qwc-main",
                    help="baseline qwc binary for the 'did this branch improve?' "
                         "column. Typically built from origin/main on a feature "
                         "branch; on main runs it is the latest published "
                         "release. Omit to drop the column entirely.")
    ap.add_argument("--qwc-main-name", default="main",
                    help="display label for the --qwc-main column (default 'main'). "
                         "Override to e.g. 'latest-release' when the baseline isn't "
                         "literally main, so the rendered column reads honestly.")
    ap.add_argument("--uuwc", default=None,
                    help="uutils invocation (default: auto-detect 'uu-wc' or 'coreutils wc')")
    ap.add_argument("--gwc", default="wc", help="GNU wc invocation")
    ap.add_argument("--no-competitors", action="store_true",
                    help="skip uu-wc and GNU wc columns (and their autodetect). "
                         "Used by scripts/bench-sweep.sh for the fast inner-loop "
                         "sweep; the per-corpus wrappers keep them by default.")
    ap.add_argument("--locales", default="C,C.UTF-8",
                    help="comma-separated LC_ALL values to measure each cell "
                         "under. Default 'C,C.UTF-8' runs both qwc kernel "
                         "families. Pass a single value (e.g. 'C') for legacy "
                         "single-locale runs.")
    ap.add_argument("--data", required=True,
                    help="corpus file, or a directory (all files in it are counted)")
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--flags", help="comma-separated flag sets (overrides default)")
    ap.add_argument("--json-out", help="also write {flag: {tool: mean_sec}} JSON here")
    ap.add_argument("--title", default="qwc benchmark",
                    help="heading for the rendered table / step summary")
    args = ap.parse_args()

    locales = [loc.strip() for loc in args.locales.split(",") if loc.strip()]
    if not locales:
        sys.exit("--locales: empty list")
    validate_locales(locales)

    flags = ([f.strip() for f in args.flags.split(",")] if args.flags
             else DEFAULT_FLAGS)
    # A directory expands to all the files in it (the many-small-files profile),
    # sorted for a stable order; a plain file is just itself. Either way the same
    # target list is handed to every tool.
    if os.path.isdir(args.data):
        targets = sorted(p for p in glob.glob(os.path.join(args.data, "*"))
                         if os.path.isfile(p))
    else:
        targets = [args.data]
    targets_str = " ".join(shlex.quote(t) for t in targets)

    # Build the column set, skipping any tool that isn't available.
    columns = [("qwc", args.qwc)]
    if args.qwc_main:
        columns.append(("main", args.qwc_main))
    if not args.no_competitors:
        # uutils ships as either a standalone 'uu-wc' or the multi-call 'coreutils
        # wc' dispatcher, depending on distro packaging. Try both when not pinned.
        uuwc = args.uuwc
        if uuwc is None:
            for candidate in ("uu-wc", "coreutils wc"):
                if have(candidate):
                    uuwc = candidate
                    break
        if uuwc and have(uuwc):
            columns.append(("uu-wc", uuwc))
        else:
            print(f"note: '{uuwc or 'uu-wc / coreutils wc'}' not found; "
                  "skipping uu-wc column", file=sys.stderr)
        if have(args.gwc):
            columns.append(("GNU wc", args.gwc))
        else:
            print(f"note: '{args.gwc}' not found; skipping GNU wc column",
                  file=sys.stderr)

    headers = [c[0] for c in columns]
    # rows[(locale, flag)] = {header: mean_seconds}
    rows: dict = {}
    for locale in locales:
        # Build a fresh env per locale: copy the parent env, then explicitly
        # set LC_ALL and pop the lower-precedence LC_CTYPE / LANG so a stray
        # value in the parent shell cannot corrupt the cell.
        cell_env = os.environ.copy()
        cell_env["LC_ALL"] = locale
        cell_env.pop("LC_CTYPE", None)
        cell_env.pop("LANG", None)
        for flag in flags:
            commands = []
            for header, prefix in columns:
                cmdline = " ".join(p for p in (prefix, flag, targets_str) if p)
                commands.append((header, cmdline))
            means = run_hyperfine(commands, args.warmup, args.runs, env=cell_env)
            rows[(locale, flag)] = dict(zip(headers, means))

    if args.json_out:
        # Nested by locale → flag → tool. Top-level locale key is the new axis;
        # the inner shape matches the pre-locale-split sidecar exactly so
        # downstream consumers (none today — verified by grep across the repo)
        # only need to look one layer deeper to find the existing tool means.
        payload: dict = {loc: {} for loc in locales}
        for (locale, flag), row in rows.items():
            payload[locale][flag or "(default)"] = row
        with open(args.json_out, "w") as f:
            json.dump(payload, f, indent=2)

    render(locales, flags, headers, rows, args.title, args.qwc_main_name)


def render(locales, flags, headers, rows, title, main_label: str = "main") -> None:
    has_main = "main" in headers
    has_uu = "uu-wc" in headers
    has_gwc = "GNU wc" in headers

    # The 'main' key is the stable internal slot (row lookups, has_main); only
    # the rendered header is overridden, so the JSON sidecar keeps its existing
    # shape regardless of the display label.
    cols = ["flag"] + [
        f"{main_label if h == 'main' else h} (ms)" for h in headers
    ]
    if has_main:
        cols.append(f"vs {main_label}")
    if has_uu:
        cols.append("vs uu-wc")
    if has_gwc:
        cols.append("vs GNU wc")

    # Pad the locale field to the widest locale so the `/` divider lines up,
    # matching the existing locale-flag row idiom in benchmarks/README.md
    # Findings 7 and 8.
    loc_width = max(len(loc) for loc in locales)

    lines = ["| " + " | ".join(cols) + " |",
             "|" + "|".join(["---"] * len(cols)) + "|"]
    for locale in locales:
        for flag in flags:
            row = rows[(locale, flag)]
            flag_label = flag if flag else "(default)"
            label = f"{locale.ljust(loc_width)} / {flag_label}"
            cells = [label] + [f"{row[h] * 1000:.1f}" for h in headers]
            qwc = row["qwc"]
            if has_main:
                cells.append(fmt_ratio(row["main"], qwc))
            if has_uu:
                cells.append(fmt_ratio(row["uu-wc"], qwc))
            if has_gwc:
                cells.append(fmt_ratio(row["GNU wc"], qwc))
            lines.append("| " + " | ".join(cells) + " |")

    table = "\n".join(lines)
    host = describe_host()
    print(f"# {title}")
    print(f"host: {host}")
    print(table)

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(f"## {title}\n\n")
            f.write(f"**Host:** {host}\n\n")
            f.write("Speedup columns are relative to qwc (>1.00x means qwc is "
                    "faster). Measured with hyperfine; runner noise applies. "
                    "qwc's edge on bandwidth-bound flags (-l, -c) scales with "
                    "CPU count, so read those against the logical-CPU count "
                    "above. Locale shown alongside flag — qwc adopts LC_CTYPE "
                    "once at startup, so the C and C.UTF-8 rows are different "
                    "code paths.\n\n")
            f.write(table + "\n")


if __name__ == "__main__":
    main()
