# qwc benchmarking notes

This file records benchmark findings and the reasoning behind them. The numbers
below were measured on **2026-06-10** on an Intel Core i7-8700 (6C/12T, 3.2 GHz;
L1d 32 KiB/core, L2 256 KiB/core, L3 12 MiB shared; AVX2, no AVX-512) under
**WSL2**. Both `qwc` and `qwc-scalar` were built `-O3 -march=x86-64-v3` so the
comparison is controlled for ISA (see `CMakeLists.txt` — `qwc-scalar` is given
the same `QWC_SIMD_FLAGS` as `qwc`).

> ⚠️ These are WSL2 numbers. WSL's syscall and page-cache behaviour, and
> especially `/mnt/c` filesystem I/O, differ substantially from a native Linux
> host. A native (Dockerized) x86 `hyperfine` suite in CI is the planned source
> of truth; treat the file-level numbers here as indicative, not authoritative.

## TL;DR

- **Per core, the hand-written AVX2 kernels beat the scalar build by 4–43×.**
  They are never "on par."
- A full-pipeline run can still *look* "on par" — but only because something
  outside the kernel (here, `/mnt/c` filesystem I/O at ~0.27 GB/s) is the
  bottleneck and starves both builds equally.
- **`movemask` + `popcount` is the wrong idiom on this microarchitecture.** The
  NEON-derived byte-lane accumulation is 10–31% faster. We keep byte-lane.

## Methodology: per-core kernel micro-benchmark

The full binary mixes threading, file I/O and page-cache effects with the kernel,
which makes it useless for isolating kernel cost. To measure the kernels alone we
call `count` / `chars` / `words` directly on an in-memory buffer, in a loop, on a
single pinned core (`taskset -c 2`), fixing total work at ~8 GiB and sweeping the
buffer size from L1-resident to far past L3. Small buffers stay in cache
(compute-bound); large buffers stream from DRAM (bandwidth-bound). The kernels
live in their own translation units, so the compiler cannot inline or elide them.
Each cell is the best of 3 runs, in GB/s.

## Finding 1 — AVX2 vs scalar, per core (byte-lane kernels)

| working set    | count: scalar / avx2 | chars: scalar / avx2 | words: scalar / avx2 |
|----------------|----------------------|----------------------|----------------------|
| 16 KiB  (L1d)  |  4.36 / 66.80 (15×)  |  4.38 / 71.13 (16×)  |  0.55 / 23.57 (43×)  |
| 128 KiB (L2)   |  4.35 / 64.10 (15×)  |  4.34 / 53.54 (12×)  |  0.56 / 20.95 (37×)  |
| 4 MiB   (L3)   |  4.06 / 39.63 (10×)  |  4.18 / 38.23 ( 9×)  |  0.54 / 18.05 (33×)  |
| 256 MiB (DRAM) |  4.00 / 16.52 ( 4×)  |  4.00 / 15.89 ( 4×)  |  0.55 / 11.18 (20×)  |

Two regimes:

- **`count` / `chars` are clean reductions.** GCC *does* autovectorize them
  (`-fopt-info-vec`: *"loop vectorized using 32 byte vectors"*), so this is not
  an ISA-support gap. But it vectorizes them *naively*: it sign-extends every
  comparison result byte→word→dword→qword before summing (36 `vpmovsx*` /
  `vextracti128` ops per 32-byte iteration) because that is the only form it can
  prove cannot overflow. That widening chain caps the scalar build at ~4 GB/s.
  The hand-written kernel's whole advantage is one idiom the autovectorizer will
  not invent: accumulate the `0xFF` match masks in *narrow byte lanes*
  (`_mm256_sub_epi8`) and defer the widening reduction (`_mm256_sad_epu8`) to
  once every 255 iterations.
- **`words` does not autovectorize at all** (*"missed: couldn't vectorize loop …
  complicated access pattern"*): the loop-carried `inWord` state and the
  data-dependent branch defeat it, leaving genuinely scalar 0.55 GB/s. The
  hand-written kernel computes the whitespace→word transition mask branchlessly,
  hence the 20–43× gap.

The AVX2 speedup shrinks with working-set size (15×→4× for the reductions)
because large buffers become DRAM-bandwidth-bound: at 256 MiB a single AVX2 core
already pulls ~16 GB/s, a large fraction of this box's ~40 GB/s dual-channel
DDR4. `words` stays compute-bound even at DRAM (still 20×), because the scalar
side is so slow it never approaches the bus limit.

## Finding 2 — `movemask` + `popcount` is slower than byte-lane

The AVX2 kernels are byte-for-byte ports of the NEON kernels, which accumulate in
byte lanes because **NEON has no `movemask`**. The obvious "more x86-native"
rewrite — collapse each compare to a 32-bit mask with `_mm256_movemask_epi8` and
`popcount` it straight into a 64-bit counter — was implemented and measured. It is
**slower across the board** (AVX2-only, byte-lane → movemask):

| working set    | count        | chars        | words        |
|----------------|--------------|--------------|--------------|
| 16 KiB  (L1d)  | 66.80→60.06 (−10%) | 71.13→48.95 (−31%) | 23.57→19.05 (−19%) |
| 128 KiB (L2)   | 64.10→49.47 (−23%) | 53.54→37.52 (−30%) | 20.95→17.33 (−17%) |
| 4 MiB   (L3)   | 39.63→34.55 (−13%) | 38.23→29.37 (−23%) | 18.05→14.96 (−17%) |
| 256 MiB (DRAM) | 16.52→16.42 ( −1%) | 15.89→15.29 ( −4%) | 11.18→10.26 ( −8%) |

**Why:** `vpcmpeqb` + `vpsubb` stays entirely in the vector ALU (2 ops/vector
across three issue ports) and reduces only once per 255 iterations. The movemask
form adds, *per vector*, a `vpmovmskb` (vector→GPR, single port) feeding a
`popcnt` (single port) — a cross-domain latency chain on two contended ports. The
gap closes only at DRAM, where bandwidth hides the extra work. The thing NEON
*can't* do turns out to be the thing we *shouldn't* do here: the byte-lane idiom
is the better AVX2 kernel too. (For `words`, movemask does yield much cleaner code
— the cross-lane carry collapses from `permute2x128`+`alignr` to a one-bit scalar
shift — but it is still ~17% slower, so we keep byte-lane.)

**Decision: kernels stay byte-lane.** This section is the recorded negative result
so the experiment is not repeated.

## Finding 3 — full-binary runs are I/O-bound on `/mnt/c` (the "on par" trap)

Measuring the whole binary on the 703 MB `train.csv` gave the misleading
impression that AVX2 was "on par" with scalar for `-w` and `-L`. It is not: the
WSL `/mnt/c` 9p/drvfs bridge delivers only ~185 MB/s, so every build is throttled
to the filesystem and ties bit-for-bit. Copy the same file to local ext4 (`/tmp`,
page-cache-warm) and the kernel advantage reappears exactly as Finding 1 predicts:

| location                | raw read | qwc `-w` / scalar | qwc `-L` / scalar | qwc `-l` / scalar |
|-------------------------|----------|-------------------|-------------------|-------------------|
| `/mnt/c` (WSL 9p)       | ~185 MB/s | 0.27 / 0.27 (1.0×) | 0.27 / 0.27 (1.0×) | 0.27 / 0.27 (1.0×) |
| `/tmp` (ext4, cached)   | 4.2 GB/s | 10.82 / 2.08 (5.2×) | 9.65 / 3.85 (2.5×) | 12.55 / 9.47 (1.3×) |

**Takeaways:**

- Never benchmark qwc against a file on `/mnt/c`. Use local ext4/tmpfs and warm
  the page cache first, or the result measures the 9p bridge, not qwc.
- `-l` shows only 1.3× even on fast storage because line counting is a clean
  reduction that becomes DRAM-bandwidth-bound; `-w` shows 5× because word counting
  stays compute-bound and the scalar build can't vectorize it.
- This also explains the cross-platform puzzle (NEON beats scalar on Apple
  Silicon, AVX2 "ties" under WSL): it was storage, not ISA or compiler. The Mac
  reads from fast native APFS and is not I/O-bound; WSL `/mnt/c` is.

## Finding 4 — the unicode/printability words kernel on AVX2

Measured **2026-06-11**, after the `-w` rewrite (unicode separators + the
≥1-printable-char rule, see `docs/superpowers/specs/2026-06-10-unicode-whitespace-words-design.md`).
`qwc` (AVX2 words kernel) vs `qwc-scalar` (unified scalar kernel), 256 MiB
`gen-data.py` corpora on `/tmp` (ext4, page-cache-warm), `hyperfine -m 8`,
threaded full-binary wall clock:

| corpus / locale                  | qwc      | qwc-scalar | speedup |
|----------------------------------|----------|------------|---------|
| ASCII, `LC_ALL=C`                | 6.1 GB/s | 0.89 GB/s  | 6.9×    |
| ASCII, `LC_ALL=C.UTF-8`          | 6.2 GB/s | 0.98 GB/s  | 6.3×    |
| mixed UTF-8, `LC_ALL=C.UTF-8`    | 2.4 GB/s | 0.94 GB/s  | 2.6×    |

**How the kernel gets there:** per 32-byte block it builds separator/printable
bitmasks in vectors, then a bit-parallel fast path (popcount of nonsep→sep
transitions) consumes blocks with no barren bytes — i.e. all real text — in a
handful of scalar ops. Blocks containing anything the vector model can't
classify bit-identically to the scalar kernel (3/4-byte UTF-8, invalid bytes,
leads with unassigned holes like 0xCE, C1 controls) fall back per-block: first
to a run-granular mask walk, ultimately to the scalar classifier. That is why
mixed text with CJK shows 2.6× rather than ~7×: its multibyte-bearing blocks
take the scalar path by design (correctness first — the AVX2 and scalar
kernels agree on *all* input, not just valid UTF-8).

**Context vs the old kernel:** the pre-unicode `-w` numbers in Finding 3
(10.82 GB/s) measured a much simpler problem — maximal non-whitespace runs,
no printability rule, no UTF-8. The new semantics cost ~40% of the old
headline throughput but make `-w` byte-for-byte wc-faithful in both locales.

## Finding 5 — many small files: the harness, not the kernel, was the bottleneck

Measured **2026-06-12** on the standard many-files corpus (`gen-data.py --many
--size 512MiB`, 2,719 files, log-uniform 4KiB–1MiB) on `/tmp` (ext4,
page-cache-warm), `hyperfine --shell=none -w 2 -r 10`. Before the fix, `qwc -l`
was **1.7× slower than GNU wc** (236 ms vs 137 ms) despite the AVX2 kernel —
and `qwc-scalar` was slower still (252 ms), proving the kernel wasn't the
problem. `/usr/bin/time -v` attributed it: ~5,200 voluntary context switches
(vs wc's 1) and ~9,000 minor faults (vs 221).

**Root cause — per-file fixed costs in `processFile`, paid 2,719 times:**

1. a `std::thread` spawn + join *per file*, even when `numThreads == 1`
   (i.e. for every file under `--bytes-per-thread` — on this corpus, all of
   them): two context switches plus stack/TLS setup per file;
2. a fresh **value-initialized 1 MiB `std::vector` buffer per file** — a 1 MiB
   memset (plus allocator churn) to scan a few-KiB file;
3. two `posix_fadvise` syscalls per file, useless for files one read consumes.

**Fix:** a one-chunk file is scanned inline on the calling `mapFiles` worker
into a per-thread reused buffer (`threadBuffer()`), and the readahead advice is
only issued for files larger than one scan buffer. The multi-chunk (large-file)
path is unchanged.

| `-l`, 2,719 small files | before | after | speedup |
|-------------------------|--------|-------|---------|
| qwc (AVX2)              | 236 ms | 43 ms | 5.6×    |
| vs GNU wc (135 ms)      | 0.57×  | **3.1×** | —    |
| vs qwc-scalar (52 ms)   | —      | 1.2×  | —       |

The win is workload-agnostic: bare `qwc` on the same corpus went 293→136 ms
and `-w` 289→130 ms. The single-large-file leg is unchanged (within noise on
`-l` and `-w`).

**Negative result — a dedicated lone-`-l` fast path buys nothing.** With the
per-file thread and allocation gone, a specialized lines-only path (no seam
context, no `ScanState`, no merge — just `pread` + `count()`) benchmarked at
exactly **1.00× ± 0.06** against the generic scan on this corpus: the generic
machinery costs a few predictable branches per 1 MiB buffer. Recorded here so
the specialization is not re-attempted; the remaining wall time is read()
copy-out (system time), which no dispatch shortcut touches.

**Also dropped:** the alphabetizing `std::sort` in `collectFiles` for sortless
`-r` runs (output order without a sort flag is now explicitly unspecified, and
a bare `--reverse` is a documented no-op). On 2,719 names the sort itself was
sub-millisecond, so this is a contract simplification more than a speed win.

## Finding 6 — Scan-buffer size: the 1 MiB bounce buffer was costing every byte two extra trips through DRAM

Measured **2026-06-13** on the native Linux box (Intel i7-8700, 6C/12T, L1d
32 KiB/core, **L2 256 KiB/core**, L3 12 MiB; AVX2; kernel 7.0.12-arch1-1; root
`/dev/sdb2` ext4) over a 512 MiB single file and a 2,719-file ÷ 512 MiB many
corpus (`gen-data.py --many --size 512MiB`), page-cache-warm,
`hyperfine --shell=none -w 2 -r 10`, sweep driver
`benchmarks/sweep.py`. Comparators in the same hyperfine invocation: GNU `wc`
9.11 and `uu-wc` (uutils 0.4.x). The TODO entry's framing — *the scoreboard is
real-world multi-core wall-clock; per-core efficiency is a diagnostic* —
selects the all-thread topology as the primary table; the 4-vCPU pinned
topology is reported because it mimics the EPYC 7763 CI runner and pins down
the success criterion.

**TL;DR.** Shrinking the per-thread scan buffer from 1 MiB to **256 KiB**
collapses big-file `-l` at 12 threads from 31.5 ms to 20.5 ms (35% faster),
turns the 4-vCPU pinned big-file `-l` from a 1.07× loss vs uu-wc into a
**1.77× win** (29.9 ms vs 53.0 ms), and recovers the same fraction on `-m`
(big-file 4-vCPU: 25.3 ms vs uu-wc 44.8 ms = 1.77×). LLC-load-misses on the
4-vCPU `-l` workload fall from 90,838 at 1 MiB to **668 at 256 KiB** (a 136×
collapse, and 6× fewer than uu-wc's 4,210). Counts and conformance unchanged.

**Hypothesis (H1, from the design spec).** `pread`'s `copy_to_user` reads the
warm page cache (trip 1) and writes the scan buffer; at 1 MiB the buffer
spills out of the 256 KiB L2, so those writes drain to L3/DRAM (trip 2) and
the kernels read them back (trip 3). At ≤256 KiB trips 2–3 stay in L2.
Prediction: big-file `-l` at 4 vCPUs drops from 62 ms to 25–35 ms with no
kernel changes, and LLC-miss traffic (misses × 64 B) falls from ~3× file size
to ~1×. H1 confirmed end-to-end.

**Wall-clock sweep, 12 threads (the user's machine, the spec's "real
scoreboard"):**

| ms                    | qwc@64K | qwc@128K | qwc@256K | qwc@512K | qwc@1M | wc     | uu-wc |
|-----------------------|--------:|---------:|---------:|---------:|-------:|-------:|------:|
| big 512 MiB · -l      | 24.7    | 21.3     | **20.5** | 23.9     | 31.5   | 51.2   | 48.4  |
| big 512 MiB · -m      | **18.7**| 18.8     | 18.9     | 19.3     | 25.9   | 1923.6 | 45.7  |
| big 512 MiB · -w      | 85.9    | 85.1     | **84.9** | 87.2     | 92.2   | 1945.3 | 1876.8|
| many 2,719 · -l       | 21.6    | **21.2** | 22.7     | 23.3     | 27.8   | 72.0   | 67.0  |
| many 2,719 · -m       | 21.8    | 22.0     | **21.1** | 22.3     | 27.4   | 1982.7 | 68.1  |
| many 2,719 · -w       | **71.6**| 72.3     | 74.2     | 74.5     | 88.3   | 1941.2 | 1847.3|

The shape is unambiguous: 1 MiB is the worst point everywhere by a wide
margin; the curve crosses the wall-clock floor in the 64K–256K band. 64K
hurts the 4-vCPU big-file rows (syscall overhead per pread surfaces with
fewer threads sharing the cost — see the 4-vCPU table below); 256K wins
or ties on every all-thread row whose floor lies above the 2% noise band,
and is at most 7% off the floor anywhere.

**Wall-clock sweep, `taskset -c 0-3` (CI-runner mimic, success criterion):**

| ms                    | qwc@64K | qwc@128K | qwc@256K | qwc@512K | qwc@1M | wc     | uu-wc |
|-----------------------|--------:|---------:|---------:|---------:|-------:|-------:|------:|
| big 512 MiB · -l      | 35.4    | 30.1     | 29.9     | 28.3     | **28.2** | 57.2 | 53.0  |
| big 512 MiB · -m      | 24.9    | 28.1     | 25.3     | **23.7** | 24.2   | 1955.9 | 44.8  |
| big 512 MiB · -w      | 140.7   | 137.8    | 138.4    | **134.4**| 138.5  | 1958.2 | 1858.0|
| many 2,719 · -l       | 26.1    | 25.9     | 26.0     | **25.7** | 26.6   | 67.4   | 64.6  |
| many 2,719 · -m       | 26.6    | **26.5** | 29.1     | 30.5     | 31.1   | 2006.9 | 65.8  |
| many 2,719 · -w       | 135.6   | 134.9    | 135.7    | **133.5**| 136.2  | 1978.2 | 1877.9|

At 4 vCPUs the optimum drifts up to 512 KiB on the big rows (1 MiB is within
0.1 ms — noise), because fewer concurrent threads mean less L3/DRAM
contention and the larger buffer amortizes syscall fixed costs better. At
256 KiB the cost vs that local optimum is small (29.9 ms vs 28.2 ms on `-l`,
25.3 ms vs 23.7 ms on `-m` — at most 1.6 ms / 6.8%), and the success
criterion is met with margin: qwc-256K beats uu-wc by **1.77×** on both `-l`
and `-m` at 4 vCPUs on the big file.

**Mechanism counters (`perf stat -x, -e cycles,instructions,LLC-loads,LLC-load-misses,minor-faults`, big-file `-l`, 4-vCPU pinned, userspace-only under `perf_event_paranoid=2`):**

| size  | cycles      | instructions | LLC-loads  | LLC-load-misses | minor-faults |
|-------|------------:|-------------:|-----------:|----------------:|-------------:|
| 64 K  | 34,888,980  | 77,151,521   | 286,434    | 1,761           | 1,582        |
| 128 K | 37,348,614  | 74,351,358   | 1,238,840  | 2,896           | 1,678        |
| 256 K | 46,894,165  | 72,895,182   | 3,925,294  | **668**         | 2,031        |
| 512 K | 50,839,557  | 72,250,727   | 3,755,269  | 9,961           | 2,607        |
| 1 M   | 64,118,966  | 71,900,084   | 3,644,032  | **90,838**      | 3,757        |
| uu-wc | 25,306,752  | 55,391,959   | 129,259    | 4,210           | 370          |

LLC-load-misses are the decisive column. At 1 MiB the bounce buffer spills
L2 so hard that 90,838 LLC misses pour out to DRAM (90,838 × 64 B ≈ 5.8 MiB
of DRAM traffic just on the bounce side, for a 512 MiB file already in page
cache). At 256 KiB the same workload generates **668** misses — the buffer
fits L2, the kernel's `copy_to_user` writes stay L2-resident, and the scan
reads back from L2 instead of DRAM. The 64 K row's even lower miss count
(1,761) reflects that the buffer also fits L1d, but the cycles + the 4-vCPU
wall-clock both penalize the syscall overhead per pread; 256 KiB is the
sweet spot where memory traffic AND syscall amortization are both good.
minor-faults rise monotonically with buffer size because larger preads
fault in more page-cache pages per call — small extra confirmation of the
same mechanism.

**Cold-cache sanity:** skipped (`drop_caches` requires sudo and was traded
against engineer time). fadvise `SEQUENTIAL` readahead absorbs smaller
preads in the I/O-bound regime; the project's history does not show a
counter-example. If cold-cache regression turns up in the field, the small
worsening of syscall count per page is the first place to look.

**Decision.** `BUF_SIZE` becomes **256 KiB**. One value, robust across both
machines that matter: the user's native i7-8700 (256 KiB L2/core × 12
threads = 3 MiB total per-thread buffers, fits L2 at full thread count) and
the EPYC 7763 CI runner (512 KiB L2/core; 4 threads × 256 KiB = 1 MiB
total, fits with headroom). Per-host auto-tuning is YAGNI; one constant
wins everywhere we measure.

**Why 256 KiB and not 512 KiB (the literal "smallest within 2% on the 4-vCPU
success-criterion rows" fallback in the plan).** The spec's primary rule is
"smallest within 2% across both corpora and both workloads", with the
tiebreaker "smaller wins (more L2 headroom)", and explicitly names the
all-thread topology as "the real scoreboard". On that scoreboard 256 KiB
wins the big-file `-l` floor outright (20.5 ms vs 512 KiB's 23.9 ms — a
17% gap) and ties on every other floor within 7%. 512 KiB is faster on the
4-vCPU pinned rows by ~1.6 ms but takes that 17% all-thread `-l` hit. The
4-vCPU pinned mode is the runner-mimic, not the user's machine. The
mechanism counters cinch it: 256 KiB sits on the L2-resident side of the
cliff (668 LLC misses); 512 KiB is already 15× past it (9,961).

**Not a fix for:** per-buffer flag dispatch (still 1.00× per Finding 5) and
the WCTX context shuffle. They were held in reserve as Approach B for a
null-result branch and stay there — H1 was the larger lever, and the
remaining items now have a clean baseline to re-measure against if anything
reopens this work.

## Finding 7 — 4-wide (128 B) newline-free fast path for the AVX2 longest-line scanner: null result

Measured **2026-06-14** on the native Linux box (Intel i7-8700, 6C/12T,
3.2 GHz; AVX2; kernel 7.0.12-arch1-1; root `/dev/sdb2` ext4), warm page cache,
`hyperfine --warmup 5 --runs 30`, four 512 MiB corpora generated by
`gen-data.py --line-length {short,mixed,long,single-line}`, all four wc flags
(`-L`, `-L -m`, `-l`, `-m`) in each cell, both locales. Branch
`newline-free-fast-path` (4×32 outer loop in `src/maxlinelen_avx2.cpp`'s
`maxLineLen` and `maxLineLenChars`) vs `main` (1×32). Spec:
`docs/superpowers/specs/2026-06-14-newline-free-fast-path-design.md`.

**TL;DR.** The 4×32 widening behaves exactly as the spec predicted at the
kernel level (instructions/byte and branches/byte both drop ~60% on long
lines), but the win is invisible at the binary because `maxLineLen` is
already at DRAM bandwidth on long-line input (~530 MB / 19 ms ≈ 28 GB/s);
once IO and orchestration are factored in, the per-byte kernel cost is no
longer the gating term. Meanwhile the extra newline-test branch
mispredicts on short-line input — H2's falsifier — making short-line input
**5–7% slower**. Null result: the 1×32 main loop stays.

**Wall-clock matrix (ms, qwc-branch vs qwc-main, `vs main` column is the
campaign signal):**

`long-512MiB` (the H1 cell):

| flag    | qwc (ms) | main (ms) | vs main |
|---------|---------:|----------:|--------:|
| C      / -L    | 18.9 | 19.2 | 1.02× |
| C      / -L -m | 19.1 | 19.0 | 0.99× |
| C.UTF-8/ -L    | 18.6 | 18.8 | 1.01× |
| C.UTF-8/ -L -m | 20.0 | 20.2 | 1.01× |

`short-512MiB` (the H2 cell):

| flag    | qwc (ms) | main (ms) | vs main |
|---------|---------:|----------:|--------:|
| C      / -L    |  71.6 |  67.7 | 0.95× |
| C      / -L -m |  69.7 |  65.7 | 0.94× |
| C.UTF-8/ -L    |  69.7 |  65.8 | 0.94× |
| C.UTF-8/ -L -m | 104.8 |  97.9 | 0.93× |

`mixed-512MiB` (historical default — drift check):

| flag    | qwc (ms) | main (ms) | vs main |
|---------|---------:|----------:|--------:|
| C      / -L    | 47.3 | 45.1 | 0.95× |
| C      / -L -m | 47.3 | 45.1 | 0.95× |
| C.UTF-8/ -L    | 47.0 | 45.0 | 0.96× |
| C.UTF-8/ -L -m | 70.0 | 64.5 | 0.92× |

`single-line-512MiB` (ceiling check — H1's most favorable shape):

| flag    | qwc (ms) | main (ms) | vs main |
|---------|---------:|----------:|--------:|
| C      / -L    | 18.4 | 19.2 | 1.04× |
| C      / -L -m | 18.6 | 18.8 | 1.01× |
| C.UTF-8/ -L    | 18.6 | 18.9 | 1.02× |
| C.UTF-8/ -L -m | 19.5 | 19.8 | 1.02× |

`-l` and `-m` flags (different translation units, expected within ±1%):
flat across every cell at the same noise floor as the long/single-line `-L`
rows, except for two cells on `single-line-C.UTF-8` (-l 1.11×, -m 0.92×)
where 2 ms absolute differences on 19 ms baselines surface as percentage
swings; these confirm the noise floor rather than indicating a change.

**Mechanism counters** (`perf stat -e cycles,instructions,branches,branch-misses -r 10`,
`-L`):

`long-512MiB`:

| metric         | qwc-branch | qwc-main |    Δ |
|----------------|-----------:|---------:|------|
| cycles         |  112,320,089 |  168,591,293 | −33% |
| instructions   |  102,644,601 |  233,696,413 | **−56%** |
| branches       |   16,083,726 |   41,162,649 | **−61%** |
| branch-misses  |      411,946 |      263,829 | +56% |
| time elapsed   |     23.3 ms  |     27.4 ms  | −15% |

`short-512MiB`:

| metric         | qwc-branch    | qwc-main      |    Δ |
|----------------|--------------:|--------------:|------|
| cycles         | 2,145,647,868 | 2,014,592,197 |  +7% |
| instructions   | 2,750,696,540 | 2,732,707,648 |  +0.7% |
| branches       |   703,209,905 |   680,584,138 |  +3% |
| branch-misses  |    31,542,840 |    26,003,696 | **+21%** |
| time elapsed   |     71.8 ms   |     67.3 ms   |  +7% |

The kernel-level prediction in H1 lands cleanly on `long`:
instructions/byte fell from 0.42 → 0.18 (−56%), branches/byte from 0.074 →
0.029 (−61%); the 4-wide outer loop does exactly the work the spec said it
would. But the binary-level `vs main` for `long/-L` is 1.02× (within run
noise) — the per-byte cost is no longer the gating term once `pread`,
thread orchestration, and DRAM bandwidth are added in, and a faster kernel
shaves cycles off a non-critical path. The wall-clock signal on
`single-line` (the spec's predicted "ceiling" at ≥10%) is 1.02–1.04× for
the same reason.

On `short`, the H2 falsifier fires: the new outer loop adds one
`vpor + vpmovmskb + branch` per 128 B, and on short-line input that branch
mispredicts often (lines are short enough that a 128 B window's
"any-newline" outcome flips unpredictably). Branch-misses rise **21%**;
cycles and wall-clock rise ~7%. The spec's H2 prediction was ≤2%
regression with branch-misses "roughly flat"; both are missed.

**Decision: do not ship.** The spec's null-result trigger fires twice
independently — `long` improves < 3% (rule a) AND `short` regresses > 5%
(rule b). The 1×32 main loop in `maxLineLen` / `maxLineLenChars` stays.
The kernel improvement is real but doesn't translate to the binary; the
short-line regression is real and is customer-visible.

**Why the kernel win doesn't make it to the binary.** Long-line `-L` on
this host has the kernel comfortably at >100 GB/s on its own (Finding 1's
microbench peaked ~71 GB/s with denser per-byte work), but the binary
makes one round trip per byte through `pread`'s `copy_to_user` + the
256 KiB scan buffer (Finding 6's mechanism) before the kernel sees it.
On a 530 MB warm file at 28 GB/s binary throughput, the kernel's per-byte
cost is already <30% of the wall-clock; halving it shaves a few percent
off the total at most, which the noise floor swallows.

**Approach C (sub-block isolation) deferred.** The spec carved it out as
an open follow-up if H2 looked marginal. H2 didn't just look marginal,
it falsified — at the mechanism level, not the noise level. The extra
branch per 128 B is the cost; sub-block isolation would still pay it.
Reopening that direction now would chase the same falsified prediction
with more code.

**NEON sibling.** The sibling TODO ("Mirror the 4-wide newline-free fast
path on NEON") was gated on a positive AVX2 result. The kernel-level
mechanism is ISA-symmetric, so it would shave the same per-byte
instructions on NEON — but the same IO-bound binary ceiling and the same
H2 branch-miss regression would apply. The bar to re-open is a credible
story for why the NEON-side ceiling would be elsewhere (e.g. a measured
NEON `maxLineLen` binary that is _not_ IO-bound). Until then, the entry
moves to `Not doing` alongside this finding.

**Conformance:** the rewrite is bit-identical to scalar on all four
corpora in both locales; the conformance suite passed all 13,542 required
comparisons before this null-result decision was made.

## Finding 8 — Kernel fusion: fold a newline tally into the AVX2 words kernel (`-lw`): null result

Measured **2026-06-17** on the same native Linux box as Findings 6–7
(Intel i7-8700, 6C/12T, 3.2 GHz; AVX2; kernel 7.0.12-arch1-1; root
`/dev/sdb2` ext4), warm page cache, `hyperfine --warmup 1 --runs 10`,
the six 512 MiB `gen-data.py` corpora plus a new `cjk-short.txt`
(256 MiB, 100 % 3-byte CJK words, short lines), both locales for the
mechanism-relevant cells. Branch `words-kernel-newline-fusion`
(`wordsAndLines` sibling free function in `src/words_{scalar,avx2}.cpp`
called from `scanBuffer` when both `-l` and `-w` are requested) vs
`main` (separate `words()` + `count('\n')` passes). Spec:
`docs/superpowers/specs/2026-06-16-words-kernel-newline-fusion-design.md`.

**TL;DR.** The fusion's predicted mechanism — "one buffer pass instead
of two, saving the DRAM trip" — does not apply on this host. Finding 6's
256 KiB per-thread scan buffer is already L2-resident, so the second
`count('\n')` pass costs nothing the fusion can save; meanwhile the
fused inner loop pays an extra `vpcmpeqb + vpsubb` per 32-byte block
plus a per-block drain branch, adding **+89 M instructions (+5 %) and
+45 M branches (+13 %)** to `-lw` without any LLC-load-miss drop.
Wall-clock `-lw` and bare `-lwc` regress **5–6 % across every corpus
shape**, on a quiet system with sub-1 % run-to-run variance. The
CJK / `C.UTF-8` punt-path cell shows a small ~3 % win because
`scalarUtf8Lines` absorbs the newline tally for free during scalar
walking, but that is a niche corner (`--utf8-class 3byte` short-line
input) against a 5 % regression on the common case. Null result: the
unfused two-call dispatch in `scanBuffer` stays.

**Wall-clock matrix** (ms, `qwc-branch` vs the `v0.2.0` release tagged
as `main`, `vs main` is the campaign signal). All cells `LC_ALL=C`, the
default invocation context for the regression-sensitive cells. Fused
cells (driven by this branch) are **bold**; the others are no-regression
guards. `mixed-512MiB` (the historical default — the most-common-case
cell):

| flag      | qwc (ms) | main (ms) | vs main |
|-----------|---------:|----------:|--------:|
| **(default = -lwc)** | 91.9 | 86.4 | **0.94×** |
| **-l -w** | 91.3 | 86.5 | **0.95×** |
| **-l -w -m** | 93.1 | 87.4 | **0.94×** |
| -w        | 84.2 | 84.4 | 1.00× |
| -l        | 18.7 | 18.5 | 0.99× |
| -L        | 45.2 | 45.2 | 1.00× |
| -L -m     | 63.6 | 63.6 | 1.00× |
| -l -L     | 48.0 | 48.1 | 1.00× |
| -m        | 18.7 | 18.6 | 1.00× |

`short-512MiB`, `long-512MiB`, `single-line-512MiB`, `big.txt` (single
512 MiB file), `many` (small-files corpus): same shape — the three
fused cells all regress 0.90–0.95×, every guard cell sits at
0.99–1.02×. The per-corpus spread is in the second decimal place; the
consistency across **six** corpus shapes is what makes the regression
load-bearing rather than noise.

**Mechanism counters** (`perf stat -e cycles,instructions,branches,
branch-misses,LLC-load-misses -r 10`, `-l -w` on `mixed-256MiB`,
`LC_ALL=C`):

| metric         | qwc-branch    | qwc-main      |    Δ |
|----------------|--------------:|--------------:|------|
| cycles         | 1,068,162,776 |   992,688,197 | **+7.6 %** |
| instructions   | 1,884,207,366 | 1,795,186,316 | **+5.0 %** |
| branches       |   400,967,876 |   356,268,929 | **+12.6 %** |
| branch-misses  |    14,157,306 |    13,834,196 | +2.3 % |
| LLC-load-misses|          ~877 |          ~922 | flat (within ~6 % run variance) |
| time elapsed   |       57.5 ms |       53.9 ms | +6.7 % |

Instructions and branches both rise (the per-block `vpcmpeqb + vpsubb`
plus the drain-check branch), branch-misses don't (the drain branch
is predicted correctly 254/255 iterations), and LLC-load-misses don't
drop — the predicted "second buffer pass eliminated" never
materializes as a DRAM saving because the second pass was already
L2-resident.

`-w` alone is **byte-identical** between the two builds at the
instruction level (1,759,390,283 vs 1,759,390,293 — a 10-instruction
diff out of 1.76 G), confirming the regression is localised to the new
fused dispatch and not any incidental codegen drift on the existing
kernel. `-l` alone is unchanged too.

**The punt-path cell** (`cjk-short.txt`, `LC_ALL=C.UTF-8`, `-l -w`):
branch 209 ms vs main 217 ms — **branch faster by 3.7 %**. Mechanism:
the AVX2 UTF-8 driver punts CJK-heavy blocks to `scalarUtf8Lines`,
which walks byte-by-byte and tallies newlines as part of the same
walk — no extra SIMD cost. Meanwhile the unfused baseline still pays a
full `count('\n')` pass over the corpus. On `cjk-short.txt` under
`LC_ALL=C` the CJK is just bytes, the fast path engages, and the cell
regresses ~0.95× like the rest. So the win is locked to one niche:
UTF-8 mode + a corpus dense in punt-class multibyte. Not a ship case.

**Decision: do not ship.** The campaign's spec defined the primary
win as "≥ 15 % on `-l -w` ASCII" and the falsifier as ">3 % regression
on `cjk-short.txt / -l -w`". The actual landing is the **inverse**:
a 5–6 % regression on the primary cell, a 3.7 % gain on the falsifier.
That inversion alone — never mind the missed primary — is decisive:
the spec's mental model of where the cost lives (cjk punt path) and
where the win lives (ASCII fast path) was backwards on this hardware.

**Why the predicted mechanism doesn't fire.** Finding 6 tuned the
per-thread scan buffer to 256 KiB so it stays L2-resident on every
host the project ships against. The second `count('\n')` pass is
therefore already cache-resident — the fusion's "save the DRAM trip"
claim presupposes a DRAM trip that Finding 6 already eliminated. The
remaining cost of running `count_avx2.cpp` over a warm L2 buffer is
dwarfed by the extra per-block work the fused inner loop has to do.
The fusion is chasing savings Finding 6 already made.

**Why ASCII regresses and UTF-8 punts win.** ASCII fast path: the
words driver's per-block work is already tightly compute-bound on
`stepMasks` plus the per-block mask construction; adding one more
compare and one more accumulator update per block costs ~25 % more
inner-loop work, which the per-buffer count savings (negligible on
warm L2) cannot recover. UTF-8 punt path: the scalar walker already
pays per-byte cost per code point; folding a `newlines += (b ==
'\n')` increment into that walk is free, while the saved `count('\n')`
pass is real work the unfused baseline still performs at the
parallel-scanner level. Same fusion idea, two different cost regimes,
opposite outcomes.

**Structural fix attempted.** The first measurement was on the spec's
chunked-outer-loop drain shape (batched 255 blocks per outer iter, the
`count_avx2.cpp` discipline). Replacing it with a per-iter drain
check (single counter + predicted-not-taken branch, mirroring the
UTF-8 path) was tried as a second pass: the cycle count moved by
< 1 % and the instruction / branch counts were identical to within
0.001 %. The cost is the per-block work itself, not the loop shape.
No further iteration is worth attempting on this mechanism.

**NEON sibling.** The spec called out a release-blocker NEON sibling
entry once the AVX2 result lands. The mechanism here is ISA-symmetric
— the same `vpcmpeqb + vpsubb` instructions exist as `vceqq_u8 +
vsubq_u8` on NEON, and Apple Silicon's per-core L2 is even larger
than i7-8700's, so the "second pass already L2-resident" condition is
at least as strong. There is no credible story for why NEON would
behave qualitatively differently. The sibling Now entry is **not
added**; both the AVX2 entry and the never-created NEON sibling move
to `Not doing` together.

**What stays.** This campaign produced four reusable artifacts even
though the fusion itself doesn't ship:

1. `benchmarks/gen-data.py --utf8-class {mixed,2byte,3byte,4byte}` —
   filters `MULTIBYTE_WORDS` by UTF-8 encoding length. Needed for the
   `cjk-short.txt` corpus here; useful for future punt-path
   measurements (e.g. the open "expand AVX2 words to 3-byte
   sequences" TODO entry).
2. `benchmarks/test-data/cjk-short.txt` — 256 MiB, 100 % 3-byte CJK,
   short lines. Permanent fixture for future kernel campaigns; the
   recipe is the `--utf8-class 3byte --line-length short` line above.
3. `bench.py` `DEFAULT_FLAGS` now includes `-l -w`, `-l -L`,
   `-l -w -m` so the next release-readiness run shows the
   planned-fusion cells — establishing baselines for the two remaining
   `Next` fusion entries (`-l -L`, `-l -w -m`) so they don't have to
   redo this campaign's matrix work.
4. `scripts/perf-ab.sh` — A/B `perf stat -r N` wrapper for the
   working-tree `qwc` against `./qwc-main`, complementing `bench.py`
   (which gives wall-clock) with cycles / instructions / branches /
   LLC-load-misses (which gives mechanism). Reusable for any kernel
   campaign.

**Conformance:** before the null-result decision was made, the
conformance suite passed all required comparisons (24,411 matched,
0 failed) against the fused build. The fusion was bit-faithful; it
just didn't pay off.

## Finding 9 — PGO + LTO on release builds: ship; the win is concentrated on `-L`

> **Provisional.** The wall-clock cells below are an indicative NEON run on a
> non-quiet system (hyperfine flagged statistical outliers); they are to be
> re-measured on a quiet system on **both** hosts (i7-8700 / AVX2 and Apple
> Silicon / NEON) before the CHANGELOG numbers land. The falsification result
> and the qualitative shape survive re-measurement; the second-decimal cell
> values do not.

Measured **2026-06-18** on Apple Silicon (arm64, NEON, 14 logical CPUs), warm
page cache, `hyperfine --warmup 2 --runs 8`, against the `v0.2.0` release tag
built as `qwc-latest-release` — which **predates all PGO/LTO scaffolding**, so
it is a faithful "before" baseline (a plain `-O3` Release). Candidate is
`./qwc` from `scripts/build-pgo.sh` (`-flto=thin` + `-fprofile-use`, profile
from the `gen-data.py --pgo-training` corpus). Spec:
`docs/superpowers/specs/2026-06-17-pgo-lto-design.md`.

**TL;DR.** PGO+LTO ships. Two questions gated the decision: (1) is the bench
sensitive enough to attribute a single-digit-percent codegen delta, and (2)
where does the delta land. **(1)** was settled by falsification — neutering
every NEON kernel (an early `return` before any byte is scanned) collapsed
`-l -w` on `big.txt` from **48 ms to 6 ms (8.2×)**, output going to `0 0`; a
benchmark that swings 8× when the kernel is deleted is measuring the kernel.
**(2)** PGO+LTO is a wash on the read-bound reductions (`-l`, `-m`, `-c` ≈
1.00×) and a consistent **~7–11 % on the longest-line family** (`-L`, `-L -m`,
`-l -L`) across short / mixed / CJK corpora, with `-w` a smaller 1–6 %. The
win lands exactly where profile-guided block layout has something to chew on —
`-L`'s branchy per-byte state machine — and is absent where the kernel already
runs at I/O speed.

**The falsification (why the delta is trustworthy).** The motivating worry was
that PGO showed `1.00×` against `v0.2.0` everywhere first looked, which is
indistinguishable from a bench that isn't measuring the binary at all. To
separate "no effect" from "no measurement", every NEON kernel (`count`,
`words`, `chars`, `maxLineLen`, `maxLineLenChars`) was temporarily shorted to
return before touching the buffer, and the binary re-benched:

| `-l -w` on `big.txt` (555 MiB, warm) | mean | output |
|--------------------------------------|-----:|--------|
| kernels neutered | 5.9 ms | `0 0` (proves the short fired) |
| `v0.2.0` (real kernels) | 48.2 ms | correct |
| `cat` (pure serial read) | 27.5 ms | — |

The neutered binary is **8.2× faster** than the real one — the bench is
sensitive. It is also faster than `cat`, which first read as "qwc must mmap
and never fault the pages." It does not: qwc **`pread`s** each 256 KiB chunk
into a reused per-thread buffer (`processfile.cpp:scanRange`, which explicitly
avoids "faulting an mmap page-by-page"). The neutered binary beats `cat`
because it `pread`s **in parallel across worker threads** while `cat` reads
serially — confirmed by `qwc -l` (full read, trivial newline SIMD) at
**6.7 ms** vs serial `cat` at 27.5 ms. That ~6 ms is the parallel-read floor,
and it is why `-l` / `-m` / `-c` sit pinned at it: I/O-bound, with no compute
headroom for PGO to recover.

**Wall-clock matrix** (provisional NEON; ms, `qwc` PGO+LTO vs `v0.2.0`,
`vs rel` = `v0.2.0 / qwc`, >1 means PGO faster). `short-512MiB`, the cleanest
case; the `-L`-family cells where the win lives are **bold**:

| flag | qwc (ms) | v0.2.0 (ms) | vs rel | bound by |
|------|---------:|------------:|-------:|----------|
| (default = -lwc) | 48.6 | 49.0 | 1.01× | word scan |
| -l | 6.7 | 6.8 | 1.02× | parallel read |
| -w | 47.7 | 50.4 | 1.06× | word state machine |
| -c | 1.4 | 1.2 | 0.89× | fstat (noise; ~1 ms cell) |
| -m | 6.7 | 6.7 | 1.01× | parallel read |
| **-L** | **22.1** | **24.5** | **1.11×** | longest-line machine |
| **-L -m** | **29.3** | **32.0** | **1.09×** | fused -L/-m |
| -l -w | 48.2 | 48.8 | 1.01× | word scan |
| **-l -L** | **22.7** | **24.9** | **1.10×** | -L dominates |
| -l -w -m | 48.7 | 49.8 | 1.02× | word scan |

The `-L` family repeats on the other two trained-shape corpora: `mixed-512MiB`
`-L`/`-L -m`/`-l -L` = 1.09× / 1.07× / 1.08×; `cjk-short.txt` = 1.11× / 1.10× /
1.10×. Nine `-L`-family cells across three corpora all land **1.07–1.11×**; the
noise floor (visible in the ~1 ms `-c` cell bouncing 0.89–1.01×) only bites the
sub-millisecond cells, so the `-L` band is signal, not jitter.

**Why `-L` benefits and the reductions don't.** `-l`, `-m`, `-c` are tight
branchless SIMD reductions (or, for `-c`, an `fstat`); they already run at the
parallel-`pread` floor, so there is no scalar control flow for profile-guided
layout to improve. `-L` (`maxLineLen`) is the opposite — a per-byte scalar
state machine for every 16-byte block that contains a newline, alongside the
vectorized newline-free fast path — exactly the branchy code where PGO's block
ordering and branch-weighting pay. `-w` sits between (the word kernel is mostly
vectorized; the scalar UTF-8 punt is rare on these corpora), hence its smaller
1–6 %. LTO's cross-TU inlining of the kernels into `scanBuffer` rides along on
all of them but is, like PGO, only visible where the call isn't already
amortized over a long scan.

**Decision: ship.** PGO+LTO stays on by default for release builds
(`scripts/build-pgo.sh`, wired into `release.yml` for both linux/x86_64 and
macos/arm64). It is correctness-neutral (the `use` binary's counts match
`v0.2.0` on every measured cell; unit tests + conformance gate it in CI), costs
~60–120 s of release-build time and a `.text` growth of `<TBD>` %, and buys a
reliable ~10 % on the `-L` family at the user's end. The earlier mental model —
that the headline would show on `-lwc` / `-l -w` — was wrong; those cells are
word-scan-bound and move ~1 %. The CHANGELOG headline cell is `-L`.

**Pending (user, both hosts).** Re-measure on quiet systems on i7-8700 (AVX2)
and Apple Silicon (NEON); the AVX2 kernels are different code, and the GCC PGO
path (reads `.gcda` in place, no `llvm-profdata` merge) has not yet been
exercised end-to-end. Fill the CHANGELOG `<TBD>` perf + `.text` numbers from the
`-L` cells, then move the companion `TODO.md` bullet `Next → Done`.

**Conformance.** The PGO+LTO build is bit-faithful: counts match `v0.2.0` on
every cell measured, and the build is gated on the unit-test + conformance
suites in `release.yml` (Linux conformance, both-platform unit tests).

## Reproducing

The per-core sweep uses a throwaway harness that `#include`s the kernel headers
and links one kernel TU per build, e.g.:

```sh
g++ -O3 -march=x86-64-v3 -std=c++17 -Iinclude harness.cpp \
    src/count_scalar.cpp src/chars_scalar.cpp src/words_scalar.cpp -o bench_scalar
g++ -O3 -march=x86-64-v3 -std=c++17 -Iinclude harness.cpp \
    src/count_avx2.cpp   src/chars_avx2.cpp   src/words_avx2.cpp   -o bench_avx2
# pin a core; sweep buffer size (L1→DRAM) with fixed total work; best of N
taskset -c 2 ./bench_avx2 <buffer-bytes> <count|chars|words>
```

The full-binary numbers are just `qwc <flag> <file>` vs `qwc-scalar <flag> <file>`
wall-clock over the file size, on local ext4 with the page cache warmed.
