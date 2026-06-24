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
4. `scripts/bench/ab.sh` — A/B `perf stat -r N` wrapper for the
   working-tree `qwc` against `./qwc-main`, complementing `bench.py`
   (which gives wall-clock) with cycles / instructions / branches /
   LLC-load-misses (which gives mechanism). Reusable for any kernel
   campaign.

**Conformance:** before the null-result decision was made, the
conformance suite passed all required comparisons (24,411 matched,
0 failed) against the fused build. The fusion was bit-faithful; it
just didn't pay off.

**Amended by Finding 16** (TMA redux): both short and long `-l -w`
classify as ~26% Bad_Speculation on the baseline at the cycle level —
the misprediction cost is flag-specific (per-word-boundary branch in
the run state machine), not geometry-specific as Finding 14's `-L`
contrast had suggested. The rescue shape is the existing "Branchless
WordScan" TODO entry, not a refined fusion shape; Finding 8's `-l -w`
fusion stays dropped.

## Finding 9 — PGO + LTO on release builds: LTO stays on (neutral); PGO dropped

Measured **2026-06-18** on the i7-8700 (AVX2, 12 logical CPUs, GCC 15.3.1).
`v0.2.0` source built plain `-DCMAKE_BUILD_TYPE=Release` (no LTO, no PGO) as
`./qwc-latest-release` — the "before" baseline. Candidate `./qwc` built from
HEAD via the new `scripts/bench/sweep.sh`, configured three ways:
(a) LTO-only (`-flto=auto`), (b) PGO + LTO with the legacy ~32 MiB synthetic
training corpus and stage-1 LTO off, (c) PGO + LTO with stage-1 LTO on and
training on the actual bench corpora (~1.5 GB across the six shapes).
Hyperfine `--warmup 3 --runs 20`, no bench-prep (turbo / SMT / ASLR all on);
ratios within each sweep are honest because both candidate and baseline see
the same system state. Spec:
[`qwc-companion/superpowers/specs/2026-06-17-pgo-lto-design.md`].

### TL;DR

- **LTO is a wash but ships.** Every cell across all six corpora is within ±1%
  of the v0.2.0 baseline. No measurable user-visible win; no measurable
  regression. It stays on by default because (a) the binary-size delta is
  trivial, (b) `release.yml` is already structured around it, and (c) it
  costs nothing.
- **PGO is dropped.** Across every training configuration and every corpus,
  GCC PGO regresses the headline word-counting paths (default, `-w`, `-l -w`,
  `-l -w -m`) by **7–10 %** while gaining 7–12 % on the rarer `-L` family.
  It is consistently spending optimization budget on the wrong kernel.
  Removed: `scripts/build-pgo.sh`, `QWC_PGO` in `CMakeLists.txt`,
  `--pgo` on `bench-sweep.sh`, `--pgo-training` on `gen-data.py`,
  and the release-workflow PGO step.

### The LTO result (neutral across the matrix)

LTO-only vs `v0.2.0`, ratio = baseline-mean / candidate-mean (>1 = candidate
faster). Ten flag bundles × six corpora = 60 cells:

- 58 cells in `[0.94×, 1.02×]` — every cell within noise.
- Two `-c` outliers at 0.94× — the `-c` mean is sub-millisecond (~0.7 ms),
  so an absolute ±50 µs noise floor reads as a ±7% ratio. Hyperfine flags
  statistical outliers in these cells.
- No same-direction drift across cells in a corpus.

LTO is correctness-neutral and perf-neutral on this kernel surface. The
project is already at the per-thread-pread parallel-read floor on `-l` / `-m`
/ `-c` (Finding 6), at DRAM bandwidth on `-L` (Finding 7), and at the
per-byte-pipeline floor on `-w` (the per-byte regime Finding 6 also closed)
— there is no remaining headroom on the candidate corpora for the linker's
cross-TU inlining to recover.

### The PGO result (regresses the headline path, helps `-L`)

PGO + LTO vs `v0.2.0`, same hyperfine budget. The pattern is identical
across all three PGO configurations tried (the legacy 32 MiB training, the
1.5 GB bench-corpora training, and stage-1 LTO on vs off):

| flag class | PGO vs v0.2.0 | PGO vs LTO-only |
|---|---|---|
| default, `-w`, `-l -w`, `-l -w -m`, `-L -m` | 0.89–0.94× | **–7 to –10 pp** |
| `-L`, `-l -L` | 1.07–1.13× | **+7 to +13 pp** |
| `-l`, `-c`, `-m` | within noise | within noise |

Worked example (`big.txt`, 256 MiB, no bench-prep, ratio = baseline / qwc):

| flag | LTO-only | PGO+LTO (bench-corpora training) |
|---|---|---|
| (default) | 0.99× | **0.98×** |
| `-w` | 0.99× | **0.89×** |
| `-l -w` | 1.00× | **0.90×** |
| `-l -w -m` | 0.99× | **0.90×** |
| `-L` | 1.00× | **1.07×** |
| `-l -L` | 1.00× | **1.08×** |
| `-L -m` | 0.99× | **0.89×** |

The word-path regression survives every PGO knob that was tried. It is the
optimization choice GCC is asked to make, not a build artifact.

### Why PGO biases toward `-L`

PGO weights basic blocks by their measured execution count. `-L`
(`maxLineLen`) is a tight per-byte scalar state machine alongside a
vectorized newline-free fast path — small code, simple control flow, every
byte goes through one of two well-defined blocks, counts accumulate cleanly.
The word kernel is a UTF-8-aware run state machine over a vectorized ASCII
classifier with a scalar punt path for 3- and 4-byte sequences — many more
basic blocks, branchier, with the SIMD inner loop "running fewer times" per
byte than `-L`'s per-byte machine. Per byte of training input, `-L`'s hot
blocks rack up more "this is hot" signal than the word kernel's hot blocks
do, and PGO follows the signal.

### What we tried before dropping it

1. **Stage-1 LTO on (match stage 4).** The original `build-pgo.sh` ran the
   instrumented stage 1 with `QWC_LTO=OFF` on a defensive "some toolchain
   combos corrupt the instrumented binary" comment. Flipping it on so the
   profile counters attribute to the same symbol/inlining layout the final
   build uses was the cheapest possible test — and it had no effect on
   the regression shape.
2. **Training on the bench corpora directly (~50× more training bytes).**
   Replaced the synthetic 32 MiB six-shape training corpus with the actual
   256 MiB bench corpora — same shapes, same flag matrix, just ~50× more
   bytes per file. The hope was that the word kernel's hot loop would
   accumulate enough samples to outweigh `-L`'s tighter loop in the
   profile. It did not: the headline path stayed at –7 to –10 %, and a
   couple of cells (`-L -m`) actually got worse.
3. **A biased flag distribution (5× more weight on word-path flags during
   training) was considered and rejected.** It would likely move the
   tradeoff, but only by hand-managing PGO around its mistake. At that
   point we are picking which flags get faster manually — not what PGO
   is for.

### Decision

PGO is dropped. LTO stays on (default-on via `QWC_LTO` per
[`CMakeLists.txt`]). Release builds become a plain
`cmake -DCMAKE_BUILD_TYPE=Release` (LTO applied automatically per the
`option(QWC_LTO ... ON)`); the release workflow no longer runs the
three-stage PGO pipeline.

Pre-existing Apple Silicon (NEON / Clang PGO) numbers in an earlier draft
of this Finding showed ~10 % wins on the `-L` family there — but those
numbers were explicitly provisional, taken on a non-quiet system with
hyperfine statistical-outlier warnings, and the same `-L` shift relative
to `-w` is visible there too. We are not re-running NEON on a quiet
system: the AVX2 + GCC PGO result is bad enough to drop the tooling on
both ISAs rather than maintain two separate PGO stories. If someone
re-opens this on macOS/NEON later, the `gen-data.py --bench-corpora`
generator and `scripts/bench/sweep.sh` flow still apply.

### Conformance

The dropped PGO+LTO build was bit-faithful in the brief window it
existed: counts matched `v0.2.0` on every measured cell. The
LTO-only candidate is identically bit-faithful (LTO does not change
codegen semantics).

**Amended by Finding 16** (TMA redux): the regression mechanism on
`long -w` classifies as **Frontend.Fetch_Bandwidth.MITE** at the cycle
level — PGO's profile-driven inlining bloats the hot path past the DSB
(uop cache), forcing fetch through the legacy decoder. Frontend rises
+13pp under PGO; Bad_Speculation drops −6pp; net regression. The
mechanism is sharper than Finding 9's wall-clock-deduced story, but
the rescue shape ("constrain inlining to fit DSB") is the same
family of trade-offs Finding 9's config sweep already explored.
Finding 9 stays dropped.

## Finding 10 — Strip-mining `scanBuffer` to L1-resident strips: null result

Measured **2026-06-19** on the native Linux box (Intel i7-8700, 6C/12T,
3.2 GHz; AVX2; kernel 7.0.12-arch1-1; root `/dev/sdb2` ext4) with warm
page cache via `scripts/bench/sweep.sh`, plus a corroborating run on the
Apple Silicon NEON box. Six 256 MiB corpora (`big.txt`, `long`, `many`,
`mixed`, `short`, `single-line`) × the 10-cell `DEFAULT_FLAGS` grid =
60 cells per host. Candidate is `scanBuffer` strip-mined into 32 KiB
tiles per the `## Next` TODO bullet ("Strip-mine multi-counter scans");
baseline is the `v0.2.0` release binary. Loop reordering only — the
carries thread through one `ScanState` exactly as they did across the
existing per-buffer seams (any chunk > `BUF_SIZE` already drove the
inner kernel sequence repeatedly on one `ScanState`), so the candidate
is bit-identical to main by construction.

**TL;DR.** The predicted mechanism — "Finding 6 one cache level up: the
second kernel reloads its strip from L2, so tile the buffer to L1 and
run all enabled kernels per strip" — does not surface at wall-clock on
either host. Across the 60-cell AVX2 grid, 59 cells sit in **0.98–1.02×**
of `v0.2.0`; the lone outlier is `big.txt` (default) at 1.10×, and it is
contradicted on the same file by `-l` (0.95×) and `-w` (0.98×). A real
strip-mining win would be consistent across the bundle and the
single-counter cells of the same corpus, not concentrated on one cell.
NEON shows the same flat shape across all corpora and flags (no cell
outside 0.98–1.02×). Hyperfine emitted 2–5 statistical-outlier warnings
on every AVX2 corpus log this round — the bench harness was noisier than
the Finding 7/8 runs were — but the cross-corpus pattern is identical
on the quieter NEON box, so the null result is load-bearing even with
the noise discount. Null result: the un-tiled single-pass `scanBuffer`
stays.

**Wall-clock matrix** (ms, `qwc` candidate vs `v0.2.0` release, `vs v0.2.0`
is the campaign signal). The `(default)` row is the bundle the proposal
predicted would benefit most; the `-l -w`, `-l -w -m`, `-l -L` rows are
the other multi-counter cells the L1-tiling story applies to.

`mixed-256MiB` (historical default corpus, the most-common-case cell):

| flag        | qwc (ms) | v0.2.0 (ms) | vs v0.2.0 |
|-------------|---------:|------------:|----------:|
| (default)   |     69.5 |        70.0 |     1.01× |
| -l          |     11.2 |        11.1 |     1.00× |
| -w          |     70.2 |        69.2 |     0.99× |
| -c          |      0.9 |         0.9 |     1.00× |
| -m          |     11.4 |        11.3 |     0.99× |
| -L          |     37.4 |        37.4 |     1.00× |
| -L -m       |     51.0 |        51.2 |     1.00× |
| -l -w       |     69.7 |        70.8 |     1.02× |
| -l -L       |     38.4 |        38.6 |     1.00× |
| -l -w -m    |     71.2 |        71.4 |     1.00× |

`big.txt` (the only corpus with a > 1.05× cell — and it doesn't generalize):

| flag        | qwc (ms) | v0.2.0 (ms) | vs v0.2.0 |
|-------------|---------:|------------:|----------:|
| **(default)**  | **70.4** | **77.4** | **1.10×** |
| -l          |     12.2 |        11.6 |     0.95× |
| -w          |     71.4 |        70.0 |     0.98× |
| -c          |      0.9 |         0.9 |     1.02× |
| -m          |     11.5 |        11.4 |     0.99× |
| -L          |     37.6 |        37.8 |     1.00× |
| -L -m       |     51.4 |        51.2 |     1.00× |
| -l -w       |     69.9 |        70.3 |     1.01× |
| -l -L       |     38.6 |        38.9 |     1.01× |
| -l -w -m    |     71.0 |        71.7 |     1.01× |

`long-256MiB`, `many-256MiB`, `short-256MiB`, `single-line-256MiB`: same
shape as `mixed-256MiB` — `(default)` flat at 1.00–1.01×, every other
cell within 0.98–1.02× of `v0.2.0`, no cell breaks the 1.02× ceiling.
The four-corpus matrices are in `logs/bench-{long,many,short,single-line}.log`;
omitted from this finding for brevity since they don't differ from the
`mixed` row above.

The `big.txt` `(default)` cell is the one place the strip-mining
proposal would have shown up if real. It does not survive the same-corpus
consistency check: the bundle's component kernels (`-l` 0.95×, `-w`
0.98×, the related multi-counter `-l -w` 1.01× and `-l -w -m` 1.01×)
all sit at or below `v0.2.0`. The 1.10× is a single-cell measurement
artifact (two outlier warnings in the corresponding hyperfine log), not
a workload class the tiling unlocks.

**No perf counters this round.** Finding 7 and Finding 8 both pinned
their nulls with `perf stat` (instructions/byte, branches, branch-misses,
LLC-load-misses) before deciding. This finding does not — the wall-clock
shape across 120 cells (60 × 2 ISAs) was flat enough, and consistent
enough across two unrelated microarchitectures, that walking the
mechanism counters wouldn't change the decision. The cost the
proposal would have had to recover — Finding 6's L2-load second-pass
trip — was wall-clock-flat across the entire matrix; the per-strip
overhead (extra loop iteration count, extra kernel-dispatch cascade
per strip) at minimum equals that saving on these hosts. A reopening
would want to start by establishing whether that saving is even
visible at the binary on any corpus / ISA, not by trying a smaller
strip size.

**Decision.** Strip-mining `scanBuffer` is dropped. `scanBuffer` stays
as the single per-buffer kernel cascade it was on `v0.2.0`. The
candidate was bit-identical to main by construction; the conformance
suite was not re-run as a separate gate (loop reordering with the
same `ScanState` cannot perturb counts), but the wall-clock matrix is
itself a 60-cell consistency check across the `DEFAULT_FLAGS` grid.
No separate design spec exists — the hypothesis lived only as the
`## Next` TODO bullet ("Strip-mine multi-counter scans") and the
throwaway `scripts/strip-sweep.sh`; both are removed alongside this
writeup. The companion roadmap entry moves to `## Not doing`.

**Amended by Finding 16** (TMA redux): baseline `(default = -lwc)` shows
**BE/Memory consistently below display threshold** (sub-5% slots) on
both `mixed-256MiB` and `big.txt` across 5 runs. When Backend surfaces
at all, it's BE/Core (Ports_Utilization), not BE/Memory. The proposed
L1-tiling rescue had no cycle-level deficit to recover — the per-byte
pipeline is branch-bound and core-bound, not memory-bound, on the
multi-counter default workload at Finding 6's L2-resident scan buffer.
Finding 10 stays closed on cleaner evidence than the original 60-cell
wall-clock sweep.

## Finding 11 — io_uring depth-2 read/scan overlap: null result, Finding 6 ate the slack

Measured **2026-06-19** on the native Linux box (Intel i7-8700, 6C/12T,
3.2 GHz; L1d 32 KiB/core, **L2 256 KiB/core**, L3 12 MiB; AVX2; kernel
7.0.12-arch1-1; root `/dev/sdb2` ext4) with warm page cache via an
ad-hoc per-corpus driver around `benchmarks/bench.py` (same protocol as
`scripts/bench/sweep.sh` but with the `--qwc-main` slot bound to a
QWC_URING=OFF build of the same working tree, so the A/B is uring vs
pread on byte-identical kernels rather than against the latest release).
Six 256 MiB corpora (`big.txt`, `long`, `many`, `mixed`, `short`,
`single-line`) × the 10-cell `DEFAULT_FLAGS` grid × `{LC_ALL=C,
C.UTF-8}` × `{12-thread, taskset -c 0-3}` = **240 cells**. Candidate is
the depth-2 io_uring scan path per spec
`superpowers/specs/2026-06-19-io-uring-overlap-design.md`: two 128 KiB
scan halves, `IOSQE_ASYNC` on every SQE, raw `io_uring_setup` /
`io_uring_enter` syscalls (no liburing). Baseline is the same working
tree with `QWC_URING=OFF` so the dispatcher falls through to the
pre-existing pread loop, the only difference between binaries.
Conformance was already green on both binaries (`fuzz 1000`, two
separate runs).

**TL;DR.** 142 of 240 cells regress by more than 3 %; 1 improves (a
1.03× outlier on the sub-4 ms stat-only `-c` cell, dominated by
fstat-only fixed cost); the remaining 97 are flat. The headline
big-file warm 12-thread `-l` lands at **0.775×** (15.06 ms vs
11.67 ms pread) and the bundled-default `(default)` cell at 0.924× C
/ 0.957× C.UTF-8 — i.e. the *exact* cells the design targeted are
where the regression is worst. The 4-vCPU mimic shows the same shape
at smaller magnitude (`-l` 0.858× on big), because pread there has
less head room and the per-submission ring overhead is a smaller
fraction of a longer baseline. Bandwidth-bound pure-stat cells (`-c`,
`-m` under C — both fstat-only, no scan path) sit at 1.00× across the
matrix; a useful sanity check that the dispatcher gate is doing its
job. Null result: the io_uring path is removed; pread stays.

**Mechanism.** The spec's prediction was that `IOSQE_ASYNC` would hide
the kernel's `copy_to_user` behind the AVX2 scan on a different
logical CPU. That prediction assumed the copy was *measurably
expensive* in the first place. Finding 6 (2026-06-13) already closed
that door by tuning the per-thread scan buffer down to 256 KiB so the
copy and the scan both stay L2-resident — at which point
`copy_to_user` for one 128 KiB submission is on the order of an L2
memcpy of 128 KiB, well under the cost of a single context switch
into a freshly-woken iowq worker. The io_uring path then trades a
near-free L2 memcpy for **two context switches per 128 KiB chunk**
(qwc-thread → iowq worker → qwc-thread again at completion), and the
trade is plainly the wrong direction.

The size-of-regression-per-cell ordering makes the mechanism vivid:

- **Worst regressions:** the cells where pread is *fastest* — `-l`
  (newline-counting AVX2 SIMD streaming bytes at near-DRAM bandwidth)
  drops to 0.78× because the per-128-KiB ring overhead is a huge
  relative fraction of a 12 ms baseline.
- **Mid regressions:** the bundled cells (`(default)`, `-w`, `-lw`)
  drop to 0.92-0.96× because they have more per-byte work, so the
  same absolute ring overhead is a smaller relative fraction.
- **Flat:** the no-scan cells (`-c`, `-m` in C) at 1.00× — the
  dispatcher routes those past the io_uring path entirely (no
  scanBuffer call), confirming the gate works as intended.

Same story shows up in the spec design's own hedge: dropping
`IOSQE_ASYNC` was floated as a rescue but rules itself out — without
ASYNC the warm-cache reads complete inline in `io_uring_enter` and
the depth-2 ping-pong collapses to "serialised pread with extra
syscall ceremony," which is at best zero gain and at worst still a
net loss to the syscall overhead. There is no salvage path that
recovers the missing copy-to-user cost the design was supposed to
hide, because that cost is no longer there to hide.

**Wall-clock matrix, `big.txt` warm 12-thread** (the corpus the
overlap mechanism was most likely to win on; uring vs pread in ms,
ratio = pread / uring so > 1 means uring wins):

| flag        | uring (C) | pread (C) | ratio | uring (C.UTF-8) | pread (C.UTF-8) | ratio |
|-------------|----------:|----------:|------:|----------------:|----------------:|------:|
| (default)   |      49.4 |      45.7 | 0.92× |            80.0 |            76.6 | 0.96× |
| -l          |      15.1 |      11.7 | **0.78×** |        15.5 |            12.0 | **0.78×** |
| -w          |      47.3 |      44.5 | 0.94× |            78.7 |            74.1 | 0.94× |
| -c          |       0.9 |       0.9 | 0.99× |             0.9 |             0.9 | 0.99× |
| -m          |       0.9 |       0.9 | 1.00× |            16.5 |            13.0 | 0.79× |
| -L          |      41.3 |      37.5 | 0.91× |            41.6 |            37.5 | 0.90× |
| -L -m       |      40.2 |      37.8 | 0.94× |            53.3 |            49.1 | 0.92× |
| -l -w       |      48.8 |      45.6 | 0.94× |            78.5 |            75.3 | 0.96× |
| -l -L       |      42.5 |      40.0 | 0.94× |            42.0 |            38.9 | 0.93× |
| -l -w -m    |      50.9 |      51.4 | 1.01× |            79.0 |            76.4 | 0.97× |

**Wall-clock matrix, `many` warm 12-thread** (the regression
guardrail, since the spec's contingent V2 idea was a per-invocation
gate for many-tiny-files):

| flag        | uring (C) | pread (C) | ratio | uring (C.UTF-8) | pread (C.UTF-8) | ratio |
|-------------|----------:|----------:|------:|----------------:|----------------:|------:|
| (default)   |      46.3 |      41.0 | 0.89× |            70.5 |            67.4 | 0.96× |
| -l          |      16.6 |      12.8 | **0.77×** |        16.6 |            12.9 | **0.78×** |
| -w          |      44.4 |      39.6 | 0.89× |            69.5 |            65.5 | 0.94× |
| -c          |       3.3 |       3.3 | 1.01× |             3.3 |             3.3 | 1.00× |
| -m          |       3.3 |       3.2 | 0.99× |            17.6 |            14.0 | 0.80× |
| -L          |      39.5 |      34.8 | 0.88× |            40.3 |            34.8 | 0.86× |
| -L -m       |      40.1 |      34.9 | 0.87× |            50.9 |            44.7 | 0.88× |
| -l -w       |      45.8 |      40.9 | 0.89× |            72.7 |            68.2 | 0.94× |
| -l -L       |      40.4 |      36.1 | 0.89× |            41.1 |            36.0 | 0.88× |
| -l -w -m    |      46.2 |      41.0 | 0.89× |            72.1 |            68.0 | 0.94× |

The many-files row is the V2-gate falsifier: the spec carved out an
"ambiguous (gate)" branch in §5 for the case where big-file improved
and many-files regressed. Many-files regressed regardless; big-file
also regressed; there is no remaining ambiguity to gate around.

`long`, `mixed`, `short`, `single-line` all show the same shape as
`big`; the per-corpus tables are in the campaign artifacts at
`/tmp/uring-results/` (`{corpus}-warm-{12t,4t}.{log,json}`) and not
reproduced here.

**4-vCPU mimic** (`taskset -c 0-3`): smaller magnitudes everywhere —
`big -l` regresses to 0.86× instead of 0.78×, headline `(default)`
sits flat at 1.00× — because the pread baseline is itself slower at
4 vCPUs (less concurrency, more serialised IO) so the same
per-submission ring overhead is a smaller fraction of a longer
budget. The shape is the same as 12-thread; the conclusion would
not change.

**No perf counters this round.** Like Finding 10, the wall-clock
matrix is uniform enough and the mechanism story is concrete enough
(post-Finding-6 warm-cache copy_to_user is an L2 memcpy; iowq
per-submission context switches are not) that pinning the regression
to `context-switches` and the absent improvement to LLC counters
would not change the decision. A reopening would want to start by
establishing whether the warm-cache `copy_to_user` is even visible
at the binary on any corpus — Finding 6 already collapsed that, and
no new mechanism has emerged that would re-expose it.

**Decision.** The io_uring scan path is dropped. The CMake
`QWC_URING` option, the `src/processfile_uring.cpp` translation unit,
the `include/processfile_uring.h` declaration, the
`src/processfile_internal.h` cross-TU header, the dispatcher branch
in `src/processfile.cpp`, the `tests/processfile_uring_test.cpp`
divergence test, and the `tests/CMakeLists.txt` plumbing are all
removed alongside this writeup; the `qwc` and `qwc-scalar` targets
return to the pre-branch shape. Spec and plan stay as a historical
record at `superpowers/specs/2026-06-19-io-uring-overlap-design.md`
and `superpowers/plans/2026-06-19-io-uring-overlap.md`. The
companion roadmap entry moves to `## Not doing`. Linux-only campaign
(macOS was carved out from the start), no NEON port implication.

## Finding 12 — Per-instruction attribution of the AVX2 words path on CJK

Rung 1 of the "observe before guessing" upgrade lands a `RelWithDebInfo`
build of qwc (`./qwc-perf`) plus a thin `scripts/bench/perf-annotate.sh`
wrapper, then uses them to attribute cycles per instruction on the AVX2
`words` kernel under 3-byte-UTF-8 input. This is observation, not a
hypothesis test: the output feeds the open "Expand AVX2 words classifier
to 3-byte sequences" go/no-go on a future branch — no threshold is
pre-committed here.

### Methodology

Build (binary-size flags inherited from the existing `Release|RelWithDebInfo`
gate, plus `-g`; the annotated binary is the shipping Release shape with
debug info):

```sh
cmake -B build-perf -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQWC_SUFFIX=perf
cmake --build build-perf --target qwc
```

Corpus (256 MiB, 100% 3-byte UTF-8 short lines — Han + Hangul, ASCII
spaces, no ASCII filler words):

```sh
uv run python3 benchmarks/gen-data.py --bench-corpora \
    --out-dir benchmarks/test-data
# yields benchmarks/test-data/cjk-short.txt (--line-length short,
# --utf8-class 3byte, --multibyte-fraction 1.0)
```

Three runs, all under `LC_ALL=C.UTF-8` (the locale where the
`scalarUtf8` punt is live):

```sh
sudo sysctl kernel.perf_event_paranoid=1   # one-time, per boot
scripts/bench/perf-annotate.sh -w     benchmarks/test-data/cjk-short.txt
scripts/bench/perf-annotate.sh -w     benchmarks/test-data/mixed
scripts/bench/perf-annotate.sh '-l -w' benchmarks/test-data/cjk-short.txt
```

`scripts/bench/prep.sh apply` was NOT applied for these runs (SMT,
turbo, and the cpufreq governor were at OS defaults). Reproducibility
of the headline percentages is quantified below; longer-term variance
across sessions / cold caches is out of scope for Rung 1.

### Results

`words(char const*, ...)` (the AVX2 words kernel, with LTO having
folded in the `scalarUtf8` punt and its `qwcIswprint` callee) dominates
in every run. Top-level shares:

| Run                    | `words` symbol | `rep_movs_alternative` |
|------------------------|---------------:|-----------------------:|
| CJK, `-w`              |     **96.60%** |                  1.84% |
| CJK, `-l -w`           |     **96.53%** |                  1.63% |
| Mixed, `-w` (baseline) |     **80.60%** |                  4.35% |

The headline metric is cycles per byte, which absorbs the percentage
normalization and frames the gap a 3-byte extension would be closing:

| Run            | Total cycles | Cycles / byte |
|----------------|-------------:|--------------:|
| CJK, `-w`      |        3.56B |     **13.30** |
| CJK, `-l -w`   |        3.54B |         13.22 |
| Mixed, `-w`    |        1.32B |      **4.95** |

**CJK is 2.7× slower per byte than mixed.** That is the budget a 3-byte
AVX2 extension would be trying to claw back.

### Where the cycles go inside `words`

Source mapping comes from `perf annotate --source` against the
RelWithDebInfo binary; offset → source line is direct.

| Offset | Instruction                       | Source                                       | CJK `-w` | Mixed `-w` |
|--------|-----------------------------------|----------------------------------------------|---------:|-----------:|
| 0x6604 | `vmovdqu (%rdx),%ymm0`            | `words_avx2.cpp:106` (AVX2 prologue load)    |    1.30% |      4.33% |
| 0x6608 | `vpcmpgtb %ymm0,%ymm5,%ymm2`      | AVX2 classifier (asciiSep / asciiPrint)      |    1.05% |      3.53% |
| 0x6848 | `movzbl (%rdx),%ecx`              | `b0 = p[0]` in `scalarUtf8` while-loop       |    2.89% |      3.17% |
| 0x6854 | `lea -0x9(%rcx),%edx`             | `cp >= 0x09 && cp <= 0x0D` (ASCII sep)       |    5.88% |     (<2%)  |
| 0x6876 | `movzwl (%r14,%rdx,2),%edx`       | `qwcIswprint`: `kIswprintIndex[cp >> 8]`     |    6.88% |     (<2%)  |
| 0x6886 | `movzbl (%rdx,%r10,1),%edx`       | `qwcIswprint`: `block[(cp & 0xFF) >> 3]`     |**11.19%**| **10.73%** |
| 0x6b80 | `cmp $0x205f,%ecx`                | `isSepCp(U+205F)`                            |    3.72% |     (<2%)  |

Two structural observations:

1. **The AVX2 prologue runs on every block in both corpora.** Absolute
   `vmovdqu` cycles are 1.30% × 3.56B ≈ 46M (CJK) vs 4.33% × 1.32B ≈
   57M (mixed); the difference is not "AVX2 only fires on mixed", it's
   that mixed's total cycles are smaller so a constant-per-block load
   is a larger fraction. The percentage gap is normalization, not
   activity. The mechanism that actually differs is what fraction of
   blocks survive the `clean == high == (lead2 | cont)` test at
   `words_avx2.cpp:117–118`: a block with any 3-byte (E0–EF) or 4-byte
   (F0–F7) lead fails the test and forces `scalarUtf8` on every byte
   of the block.

2. **The scalar punt is dominated by `qwcIswprint`, not by sequence
   decode.** Combined `qwcIswprint` index + bitmap load (0x6876 +
   0x6886) is **18.07% on CJK** and **~13.6% on mixed**. The
   `isSepCp(U+205F)` check at 0x6b80 is visible on CJK (3.72%) and
   cold on mixed — consistent with U+205F being a 3-byte code point
   that only the CJK corpus reaches.

`-l -w` vs `-w` on CJK is structurally identical (0x6886 = 11.32%,
`isSepCp` = 4.00%); bundling `-l` adds a 25%-share `vpcmpeqb` in a
separate countlines symbol but does not shift which lines own the work
on the words side.

### Reproducibility

Two consecutive `-w` records on the CJK corpus, no bench-prep, no
intervening workload:

| Quantity                                | Run 1   | Run 1b  | Δ       |
|-----------------------------------------|--------:|--------:|--------:|
| `words` % of cycles                     |  96.60% |  97.24% |  +0.64pp|
| `0x6886` (`qwcIswprint` bitmap load)    |  11.19% |  12.07% |  +0.88pp|

Within the ±2pp budget set in the design spec. Useable as Finding 12's
headline numbers.

### Reading: what the open 3-byte-classifier question gets out of this

The TODO entry "Expand the AVX2 words vectorized classifier to 3-byte
sequences" is asking whether to fold E0–EF lead-byte classification
into the AVX2 fast path instead of punting to `scalarUtf8`. This
Finding gives that branch its measured baseline:

- **Upper-bound speedup on CJK is ~2.7×** (13.3 → ~4.95 cy/B if the
  extension fully closed the gap to mixed). Realistic Asian-language
  text — Japanese prose has substantial ASCII (spaces, numbers,
  punctuation, embedded English) and the multibyte words are
  predominantly 3-byte — would land somewhere between mixed (5 cy/B)
  and pure CJK (13 cy/B), so the realized speedup on realistic
  workloads is bounded by where the workload sits on that axis.
- **Most of the scalar cost is `qwcIswprint`, not sequence decode.**
  ~18% of CJK's cycles go to the two-level Unicode bitmap. A 3-byte
  AVX2 classifier that still has to call `qwcIswprint` (because most
  3-byte code points are printable and the bitmap is the source of
  truth) does not eliminate that 18%; it eliminates the
  per-byte-loop overhead around it. The win is real but smaller than
  "all scalar cycles disappear".
- **`isSepCp` is the wrinkle the TODO entry already calls out.** The
  3-byte separator code points (U+1680, U+2000–200A, U+205F, U+3000,
  plus the no-break-space-mode set U+2007 / U+202F / U+2060) sit
  under lead bytes E1 / E2 / E3, so a vectorized 3-byte path needs
  either a continuation-byte compare in the vector domain or a
  per-block scalar fallback on those leads. The U+205F compare at
  `0x6b80` (3.72% on CJK) is the visible cost of that case today.

The threshold that turns these numbers into a go/no-go belongs to the
3-byte-ext branch. This Finding ships the data.

### Per-instruction observation workflow

Workflow doc (permanent infrastructure — read this before the next
`perf annotate` campaign):

**Build.** `cmake -B build-perf -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
-DQWC_SUFFIX=perf` produces `./qwc-perf` next to `./qwc`. The existing
`Release|RelWithDebInfo` gate in `CMakeLists.txt` covers RelWithDebInfo,
so the annotated binary inherits all binary-size flags
(`-fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables
-fno-unwind-tables`) — it is structurally the shipping Release binary,
plus `-g`. Don't profile Debug builds: their hotspots look different
(unhardened inlining, no LTO, kept exception edges).

**Wrapper.** `scripts/bench/perf-annotate.sh <flag> <corpus> [<symbol>]`
runs `perf record -e cycles:pp` then a flat top-symbols report; if a
symbol is given, it also runs `perf annotate <symbol>`. Env knobs:
`QWC_PERF_LOCALE` (default `C.UTF-8`), `QWC_BIN` (default `./qwc-perf`).
Perf data lands at `/tmp/qwc-perf.data` and is reused if you re-annotate
a different symbol — `perf annotate -i /tmp/qwc-perf.data --stdio
--no-source` is the no-record one-liner. Use `--source` to interleave
source lines with the disassembly (the offset → source mapping this
Finding's results table relies on).

**`perf_event_paranoid` gotcha.** Arch defaults to 4; the wrapper
demands `≤ 1` and prints a one-line fix. `sudo sysctl
kernel.perf_event_paranoid=1` is per-boot; pin it in
`/etc/sysctl.d/` if you want it permanent.

**Callgraph caveat.** The binary-size flags strip unwind tables, so
`--call-graph dwarf` is unusable. `--call-graph lbr` works without
unwind tables and is the escape hatch when caller attribution is
needed. The Rung 1 wrapper omits `--call-graph` entirely; annotate
doesn't need it.

**Symbol naming after LTO.** LTO folds `scalarUtf8` and `qwcIswprint`
into their callers. `perf annotate scalarUtf8` will fail with "no
samples"; the symbol that actually owns the cycles is the outer kernel
function (here, `words(char const*, ...)`). The
`scripts/bench/perf-annotate.sh` wrapper documents this in its help.
The escape hatch is `perf annotate -i /tmp/qwc-perf.data --stdio
--no-source` with no symbol filter — that prints every symbol with
samples.

**Bench-prep.** `scripts/bench/prep.sh apply` is recommended for any
measurement whose result is going to be quoted as authoritative —
SMT off, turbo off, performance governor, page-cache drop. Rung 1's
own measurements skipped it (the workflow doc is the deliverable, not
a finely-quantified number); future campaigns using this workflow
should apply prep first.

**CJK corpus.** `benchmarks/test-data/cjk-short.txt` is the 3-byte
UTF-8 stress corpus (Han + Hangul, 256 MiB, `--line-length short`,
`--utf8-class 3byte`, `--multibyte-fraction 1.0`). Regen recipe:
`uv run python3 benchmarks/gen-data.py --bench-corpora --out-dir
benchmarks/test-data` — idempotent on existing corpora.

**What Rung 1 doesn't answer.** "Frontend-Bound / Backend-Bound /
Bad-Speculation / Retiring?" is a top-down microarchitecture question
that `perf annotate` can't answer. Rung 2 (`toplev.py` TMA setup,
separate branch) is the next escalation when the per-instruction view
runs out of explanatory power.

## Finding 13 — 3-byte UTF-8 vectorization in the AVX2 words kernel

Closes the open question Finding 12 framed: a `kCandLead3[1024]`
two-level (lead, cont1) cleanness table now gates direct in-vector
classification of clean 3-byte sub-rows, in addition to the existing
clean 2-byte rows. 4-byte sequences still punt. The change is
bit-identical to scalar on all input; the win is path-switching for
CJK / Hangul / Devanagari blocks (plus an incidental
inlining-heuristic recovery on every other block — see "Layout-stabilizer"
below).

**Spec:** `qwc-companion/superpowers/specs/2026-06-20-3byte-utf8-avx2-words-design.md`.
**Plan:** `qwc-companion/superpowers/plans/2026-06-20-3byte-utf8-avx2-words.md`.
**Branch:** `three-byte-extension` (AVX2 only; NEON port is the
release-parity blocker, flagged in `qwc-companion/TODO.md`).

### Headline

`cjk-short.txt -w`, `LC_ALL=C.UTF-8`, `qwc-perf` cycles event:

| Quantity                                | Finding 12 | Finding 13 |    Δ |
|-----------------------------------------|-----------:|-----------:|-----:|
| Total cycles                            |      3.56B |      1.11B |  −69% |
| Cycles / byte                           |      13.30 |       4.12 |  −69% |
| `words` symbol % of cycles              |     96.60% |     47.84% | −49pp |
| `lead3SubrowDirty` % (new helper)       |          — |     41.73% |    — |
| `qwcIswprint` bitmap load (offset hot)  |     11.19% |      <1%   |  gone |

The CJK win predicted by Finding 12's mixed/CJK gap (~2.7× upper bound)
is comfortably exceeded. Wall-clock on the bench harness (`scripts/bench/cjk-short.sh`,
`prep.sh apply` applied): 219.5 ms → 70.6 ms (**3.11× wall speedup**).

### Per-instruction shift

The Finding 12 hotspot was `movzbl (%rdx,%r10,1),%edx` at offset `0x6886`
inside `words` — the second-level bitmap byte load in `qwcIswprint`
(11.19% of cycles). That instruction's call chain (`scalarUtf8` →
`classifyUtf8` → `qwcIswprint`) no longer fires per byte on clean
3-byte sub-rows; the AVX2 path classifies the whole 32-byte block
directly. `qwcIswprint` falls below the 1% reporting threshold.

The new top-of-list is the `kCandLead3` cell test inside the extracted
walker:

| Symbol               | Top instruction                       | Share (of fn) | Share (of total) |
|----------------------|---------------------------------------|--------------:|-----------------:|
| `lead3SubrowDirty`   | `cmpb $0x0,(%rcx,%rax,1)` at `0x2244` |        33.76% |           14.09% |
| `words` (vector body)| `vmovdqu (%rdx),%ymm1` (block load)   |         ~3.5% |            ~1.7% |

The shift is what Finding 12 predicted: the kernel still has to consult
a per-codepoint table, but it does so once per LEAD (≤4 leads per
32-byte block on CJK content) instead of once per BYTE through
`qwcIswprint`'s two-level bitmap.

### No-regression matrix (160 cells)

Sweep run with `scripts/bench/sweep.sh` (no `prep.sh apply`; the
delta is large enough to survive desktop noise). Each cell is
qwc / v0.2.1 ratio (>1.0 = qwc faster):

| Corpus         | C / -w | C.UTF-8 / -w | C / default | C.UTF-8 / default |
|----------------|-------:|-------------:|------------:|------------------:|
| big            |  1.05× |        1.34× |       1.06× |             1.33× |
| long           |  1.05× |        1.34× |       1.04× |             1.35× |
| many           |  1.06× |        1.36× |       1.07× |             1.33× |
| mixed          |  1.04× |        1.35× |       1.04× |             1.34× |
| short          |  1.07× |        1.34× |       1.05× |             1.33× |
| single-line    |  1.05× |        1.36× |       1.05× |             1.34× |
| cjk-short      |  1.04× |    **3.02×** |       1.04× |         **2.97×** |
| cyrillic-short |  1.03× |        1.06× |       1.02× |             1.06× |

All `-l` / `-c` / `-m` / `-L` / `-L -m` cells (160 total minus the
words-touching ones) sit at 1.00× ± noise. The non-CJK 1.32–1.36× wins
on C.UTF-8 are not free lunches — those corpora have 8% multibyte
fraction in the `mixed` UTF-8 class, so a meaningful slice of their
blocks have 3-byte leads that the new path certifies clean instead of
punting. cyrillic-short is the control: 100% 2-byte Cyrillic, no
3-byte content for the new path to vectorize, so its win (1.06× on
C.UTF-8 `-w`) is purely the layout/inlining recovery described below —
useful as evidence that the layout fix is not corpus-specific. CJK is
the headline; the realistic-text band is well above the no-op floor.

### Layout-stabilizer: a finding inside the finding

The first cut inlined the lead3 walk directly into `words()`. It
passed unit tests, matched conformance, and posted the CJK win — but
the no-regression matrix came back with a uniform ~4% slowdown on
every C-locale `-w` cell of the sweep. `ab.sh -r 10` on `big -w`
confirmed: **identical executed-instruction count** (868.5M vs 868.4M
between branch and v0.2.1), with the 4% gap entirely in cycles. The
regression was binary layout, not algorithm — `words()` had grown
from 1051 to 1140 instructions (+8.5%), which shifted the C-locale
exit branch from `0x7010` to `0x71c0` and perturbed alignment / icache
for the unrelated C-locale loop.

Extracting the walk to a `static [[gnu::noinline, gnu::cold]]` helper
flipped the C-locale regression to a **4.2% win** (cycles 669M → 621M;
wall 34.1ms → 31.0ms on the same `big -w` ab). Two effects compounded:

1. **Layout.** `words()` shrank to 1113 instructions; `lead3SubrowDirty`
   (18 instructions) landed in `.text.unlikely` (`.cold` partition),
   off the hot icache path.
2. **Inlining heuristic.** `[[gnu::cold]]` dropped the helper's size
   from GCC's whole-function inliner estimate for `words()`. That
   changed inlining decisions in unrelated branches of `words()` —
   notably the C-locale block — and saved roughly one instruction per
   32-byte iteration. The 256 MiB `big -w` run executed **9M fewer
   instructions** (868.5M → 859.4M) on a path that never calls the
   helper.

The CJK win was preserved (~3.15× on `ab.sh`); the per-block function
call into `lead3SubrowDirty` is invisible next to the `kCandLead3`
cache probes inside. Net trade: −62 inst growth in `words()` (vs +89
inlined), one cold-partitioned 18-inst helper, and a uniform recovery
across the matrix. Diagnosis workflow captured in `qwc-companion/CLAUDE.md`
("Diagnosing small regressions") for the next time `sweep.sh` shows a
sub-5% regression on a cell that doesn't exercise the changed path.

### NEON port status

Not started. The mechanism is ISA-symmetric — the `kCandLead3` table
is generated, locale-invariant, and orthogonal to the vector primitives;
the NEON kernel needs the same lead3 mask, edge-byte check, carry
widening, and noinline+cold helper-extraction. Flagged in
`qwc-companion/TODO.md` as a release-parity blocker for the next
release that ships either ISA: shipping AVX2-only would put Apple
Silicon users a meaningful generation behind on CJK throughput.

### Follow-ons enabled or invalidated

- **Pre-AVX2 short-circuit.** Finding 12 framed the kernel's open
  expansions against the 13.30 cy/B baseline. That baseline is dead;
  the new floor on CJK is 4.12 cy/B, and any further work in this lane
  (4-byte vectorization, emoji corpus, lookup-table densification)
  re-baselines against this Finding.
- **4-byte path.** Still scalar. The cost calculus has shifted: a
  4-byte AVX2 extension would need its own carry-quad and a (lead, cont1,
  cont2) cleanness table — and the corpora that exercise it (heavy
  emoji, supplementary planes) are rarer in real `wc` workloads. The
  open question becomes how much realistic-text speedup is left to
  claw back vs the engineering cost. The Finding 12 → Finding 13 path
  was viable because CJK is common; emoji-only stress is the borderline
  case and TODO carries it as deferred.
- **`qwcIswprint` table reshape.** With the bitmap byte-load no longer
  hot, the prior "dense bitmap is too big" line item drops in priority.
  TODO accordingly.

## Finding 14 — TMA classification of the `-L` kernel on long-line input

Rung 2 of the "observe before guessing" upgrade lands a thin
`scripts/bench/toplev.sh` wrapper around `toplev.py` (Andi Kleen's
`pmu-tools`), then uses it to classify pipeline slots on the AVX2
`maxLineLen` kernel at the microarchitecture level — one rung above
Finding 12's per-instruction attribution. This is observation, not a
hypothesis test: the output amends **Finding 7**'s wall-clock-derived
ceiling claim and feeds a future "TMA redux" branch that would
re-examine Findings 8/9/10 with the same workflow.

### Methodology

Binary: the `./qwc-perf` `RelWithDebInfo` build from Rung 1.
Wrapper: `scripts/bench/toplev.sh <flag> <corpus>` which invokes
`toplev.py -l3 --no-desc --global -- ./qwc-perf <flag> <corpus>`.
Setup: `pmu-tools` cloned to any path with `toplev.py` symlinked onto
PATH; `kernel.perf_event_paranoid=-1` (CPU event access; `≤1` is
sufficient for Rung 1 but not for Rung 2). `scripts/bench/prep.sh
apply` was NOT applied — Finding 12 also ran at OS defaults; the
reproducibility check below quantifies what that costs.

Three measurements + one reproducibility re-run, all under `LC_ALL=C.UTF-8`:

```sh
sudo sysctl kernel.perf_event_paranoid=-1   # one-time, per boot
scripts/bench/toplev.sh -L benchmarks/test-data/long   # headline
scripts/bench/toplev.sh -L benchmarks/test-data/short  # geometry contrast
scripts/bench/toplev.sh -l benchmarks/test-data/long   # cross-flag sanity
scripts/bench/toplev.sh -L benchmarks/test-data/long   # repro
```

**The `--global` is load-bearing.** A first pass without it produced
per-core breakdowns where the same workload classified C0 as 38%
Backend on one run and 78% Backend on the next — that's worker-thread
migration across cores, not noise. `--global` aggregates all CPUs into
one readout, which is the right view for qwc's `hardware_concurrency()`-
sized worker pool. The wrapper bakes this in; the variation is in the
script comment for cases that genuinely want per-core / per-thread.

### Results

Top-level slot percentages (multiplex error in `[brackets]`):

| Run                  | Backend | BE/Memory | BE/Core | Frontend.FL | Bad_Spec |
|----------------------|--------:|----------:|--------:|------------:|---------:|
| long `-L` (run 1)    |   76.2% |    56.6%  |   19.6% |       23.2% |     0.0% |
| long `-L` (run 2)    |   77.2% |    58.1%  |   19.1% |       24.1% |     0.0% |
| long `-l`            |   67.5% |    51.2%  |   16.3% |       18.5% |     0.0% |
| short `-L` (contrast) |  25.1% |     —     |   14.1% |       17.2% |  **23.4%** |

Multiplex error is 6–7% on the long-input cells (the cell ran for ≈1.2 s
total wall, which is a borderline budget for `-l3`'s event-group count);
2% on short `-L`. The leaf decomposition under `BE/Memory` is muddy on
every run — `L1_Bound` / `L2_Bound` / `L3_Bound` / `DRAM_Bound` all
report zero or near-zero counts because the leaf events fell in
multiplex slices that didn't sample them. The top-level `BE/Memory`
number is the load-bearing signal; reading down to specific cache
levels is out of reach with default counter scheduling.

### Reading vs Finding 7

Finding 7 concluded that the AVX2 newline-free fast path was a null
because the `maxLineLen` kernel on long input was already at the DRAM
bandwidth ceiling — a conclusion *deduced* from wall-clock bandwidth
matching the platform's memory throughput. TMA confirms the upstream
half of that deduction directly: **76–77% of pipeline slots on long
`-L` are Backend-Bound, of which 57% are Memory-Bound and 20% are
Core-Bound.** The bound is real and dominant at the microarchitecture
level. The downstream half — *which* level of memory hierarchy (L3 vs
DRAM) — is multiplex-blocked here and remains an inference rather than
a measurement. Net effect on Finding 7's claim: stronger evidence that
the kernel is binary-level-ceiling-bound, no new information about
which cache level the bound sits at.

### Geometry contrast: short `-L`

The contrast cell is the most surprising number in the table. Same
kernel, same byte distribution (both `long` and `short` corpora are
~8% multibyte), opposite line geometry — and the **top bound flips
from Backend-Memory (long) to Bad_Speculation (short)**. On short
input, `maxLineLen`'s per-byte newline-locating step hits frequent
mispredicted branches (23.4% of slots is Branch_Mispredicts;
`Branch_Resteers` accounts for another 7.3% of clock cycles under
Frontend.Fetch_Latency). The kernel cost-model is not a property of
the kernel; it is a property of the workload geometry, and the
geometry switches the binary-level ceiling story.

This is directly relevant to Finding 8 (the `-l -w` fusion null on
short input). Finding 8's wall-clock conclusion was that fusing the
newline tally into the words kernel cost more than it saved on short
ASCII; the Bad_Speculation signal here suggests the cost-model is
specifically branch-misprediction-driven, which is a different rescue
shape than Finding 8 considered.

### Cross-flag sanity: long `-l`

`-l` and `-L` share the per-byte newline-locating step on AVX2; the
TMA profile should look similar. It does: `-l` lands at 67.5%
Backend / 51.2% Memory_Bound / 16.3% Core_Bound — 5–8pp lower than
`-L` on every metric, which tracks with `-l`'s simpler per-byte work
(count newlines; no running max). No surprise here; the sanity check
passes.

### Reproducibility

The two long `-L` runs land within ±1.5pp on every top-level slot
category (76.2% vs 77.2% Backend; 56.6% vs 58.1% BE/Mem; 19.6% vs
19.1% BE/Core; 23.2% vs 24.1% Frontend.FL). The 7% multiplex error is
visible in the per-run variance but does not flip rankings or change
the qualitative story. **The reading is signal, not noise** at the
top-level slot category — adequate for the Finding 7 amendment, the
geometry-contrast story, and the redux verdict below.

Caveat for finer-grained work: the multiplex-blocked leaf
decomposition (which cache level under BE/Memory) is NOT
reproducible — `L1_Bound` was 0.0 on run 1 and 11.2 on run 2. A leaf-
accurate measurement would need `--no-multiplex` with a restricted
counter group, taking multiple runs to cover the level-3 hierarchy
piecewise. Out of scope here; flagged for the redux branch if a
specific question demands it.

### Verdict on the future "TMA redux" branch

**Worth doing.** Top-level slot classification reads sharp and
decisive at qwc's wall-time scale (~1.2 s on the 256 MiB headline
cell); reproducibility is within ±2pp; the geometry-contrast finding
on short `-L` is a positive signal that TMA generates net-new
information rather than re-derivating per-instruction attribution at
a different resolution. Specifically:

- **Finding 8 redux** (`-l -w` fusion on short input) gets a sharp
  predicted classification: Bad_Speculation, driven by per-byte
  newline-boundary mispredicts. That's a different rescue shape from
  Finding 8's wall-clock-only consideration and worth the redux pass.
- **Finding 9 redux** (PGO `-w` regression) and **Finding 10 redux**
  (strip-mine) — the long-input cell is the tractable substrate;
  both should get clean readings.

Caveat: the leaf decomposition (which cache level, which port) is
multiplex-blocked at `-l3` on the current wall-time budget. The redux
branch should plan to run `--no-multiplex` on a restricted counter
group for any question that needs leaf-accurate signal — that's
multiple runs covering the hierarchy piecewise, not a single
invocation.

### TMA observation workflow

Workflow doc (permanent infrastructure — read this before the next
`toplev.py` campaign):

**One-time setup.** `pmu-tools` is not packaged on Arch (no `extra/`,
no AUR). Clone the upstream repo somewhere stable and symlink
`toplev.py` onto your PATH:

```sh
git clone https://github.com/andikleen/pmu-tools <somewhere>
ln -sfn <somewhere>/toplev.py <a-dir-on-your-PATH>/toplev.py
```

The wrapper resolves `toplev.py` via PATH and doesn't care where the
clone lives. `pmu-tools` ships event JSONs that update with new
microarchitectures; a `git pull` keeps current — no version pinning
in the wrapper.

**Wrapper.** `scripts/bench/toplev.sh <flag> <corpus>` runs
`toplev.py -l3 --no-desc --global -- ./qwc-perf <flag> <corpus>`.
Env knobs: `QWC_PERF_LOCALE` (default `C.UTF-8`), `QWC_BIN` (default
`./qwc-perf` — reused from Rung 1; no separate build).

**`--global` is load-bearing.** qwc spawns `hardware_concurrency()`
worker threads that migrate across cores; per-core TMA breakdowns are
dominated by worker-migration variance rather than kernel-cost signal
(Finding 14's first-pass measurement saw 38% Backend on C0 in run 1,
78% on the same core in run 2). `--global` aggregates all CPUs into
one readout — the right view for the workload. Override via the
common variations below if you need per-core / per-thread instead.

**`perf_event_paranoid` gotcha.** Rung 2 needs CPU event access,
which the kernel blocks at `≥1` — *stricter than Rung 1's
`cycles:pp`-only requirement*. The wrapper demands `≤ -1` and prints
a one-line fix. `sudo sysctl kernel.perf_event_paranoid=-1` is
per-boot; pin in `/etc/sysctl.d/` for permanence. (Sysadmins running
shared boxes have a real reason to keep this high; `-1` lifts almost
all restrictions.)

**Reading the four top categories.** Every slot is classified into
one of four:

- **Frontend_Bound** — pipeline is starved waiting for instructions
  (icache, ITLB, branch resteers, decoder issues).
- **Backend_Bound** — pipeline has instructions but can't retire
  them. Two sub-categories: **Memory_Bound** (caches, DRAM) and
  **Core_Bound** (execution-port saturation, divider, serializing
  ops).
- **Bad_Speculation** — pipeline ran instructions that turned out to
  be on the wrong branch (mispredict) or had to be flushed
  (machine_clears).
- **Retiring** — useful work. Higher is better.

The level-3 view drills into the dominant category's sub-leaves
(e.g., `BE/Memory.L1_Bound`, `BE/Memory.L3_Bound`, `BE/Memory.DRAM_Bound`).
Escalate to `-l6` only if `-l3` doesn't decompose far enough; the
default starting point is `-l3`.

**Multiplex caveat.** `-l3` needs more counter groups than the CPU
has hardware slots, so toplev multiplexes — time-slices counters
across the run and extrapolates. On sub-2-second qwc workloads the
multiplex error on top-level slot categories is around 6–7%
(quantified in Finding 14's reproducibility check), and leaf
categories under the top can swing wildly between runs (Finding 14's
`L1_Bound` was 0.0 on one run and 11.2 on the next). The fallback
when leaf-accurate signal is needed: `--no-multiplex` with a
restricted counter group, taking multiple runs to cover the level-3
hierarchy piecewise.

**Bench-prep.** `scripts/bench/prep.sh apply` is recommended for any
measurement whose number is going to be quoted as authoritative.
Finding 14 skipped it (matching Finding 12's precedent — the
workflow doc is the deliverable, not a precision number); future
campaigns using this workflow should apply prep first if a leaf-level
number needs defending.

**Common variations** (run `toplev.py` directly for these):

- `-l1` / `-l6` — different drill-down depth.
- `--per-thread` — per-CPU-thread breakdown instead of global.
- `--core C0-C5` — limit to specific cores.
- `--no-multiplex` — accuracy at the cost of hierarchy depth.
- `--csv ';'` — machine-parsable output.

**What Rung 2 doesn't answer.** Region-scoped instrumentation
(measure a specific kernel function rather than the whole binary)
and static port-pressure analysis on a single inner loop are
out-of-band questions. Rung 3 (`likwid` markers + `llvm-mca`,
separate branch in `TODO.md`'s `## Next`) is the escalation when
TMA's whole-binary view runs out of explanatory power.

## Finding 15 — NEON words movemask via the `vshrn` nibble trick (shipped, dense-2-byte regression)

NEON has no native movemask. The words kernel built its 32-bit S/P bit
masks by `AND`-ing each lane with a per-lane power of two and summing
the groups — **4× `vaddv_u8` per 32-byte block**, a cross-lane
horizontal reduction with multi-cycle latency. The `vshrn` shift-narrow
"nibble" trick replaces it: `vshrn_n_u16(reinterpret_u16(cmp), 4)` +
`vget_lane_u64` yields a `u64` where input byte *i* lands in nibble *i*
(`0xF`/`0x0`) — one shift-narrow + one GPR move, **no reduction**. The
mask is now 4-bits-per-byte, so the state machine and the UTF-8
clean-check arithmetic move to nibble stride (`<<1`→`<<4`, `>>31`→`>>60`,
`popcount>>2`, `ctz>>2`) in a NEON-local `stepMasksNibble` — the shared
bit-per-byte `stepMasks` is left untouched so the AVX2 kernel can't be
perturbed. One `uint8x16_t` per block (16 bytes → one `u64`).

### Headline (20-run wall-clock, ON vs the `vaddv` baseline)

| corpus / locale | `-w` | what it is |
|---|---|---|
| mixed, short, big — `C.UTF-8` | **1.20×** | common UTF-8: ASCII-dominant + multibyte |
| all corpora — `C` | **1.04×** | the common C-locale word count |
| cjk-short — `C.UTF-8` | 1.00× | dense 3-byte: bottlenecked on `kCandLead3`, not movemask |
| **cyrillic-short — `C.UTF-8`** | **0.87×** | dense 2-byte: **regression** |

The UTF-8 win is large because that path issues ~3–9 movemasks per
block (vs C-mode's 2), so the `vshrn` saving is amplified. `-l`, `-L`
and `-c` are unchanged (their kernels don't move); binary size is
unchanged (91 KB, same as the `vaddv` build — the dead `vaddv` helpers
are `#if !QWC_NEON_NIBBLE`-guarded out).

### The regression, and why N32 can't fix it

16-byte blocks double the per-block fixed cost — carries, the
clean/cont mask comparison, edge-byte validation — relative to the old
32-byte blocks. On ASCII-dominant input the cheap `high == 0` fast path
skips almost all of it and the movemask saving dominates; on **dense
2-byte** input (every block runs the full machinery) the doubled
overhead outweighs the saving, hence −13% on the all-Cyrillic corpus.

The obvious fix — keep 32-byte blocks (restoring edge frequency) but
build the masks with `vshrn` into a 128-bit nibble mask — was
implemented and **falsified**: `__uint128_t` arithmetic on AArch64
(the `u128` state machine, `popcount128`/`ctz128`, the 128-bit
clean-check) costs *more* than the movemask saves. "N32" regressed
**C `-w` to 0.82×** and **cyrillic to 0.58×** while halving the UTF-8
win to ~1.08×. So the nibble win exists only while the masks stay in
native `u64` (16-byte blocks), and the dense-2-byte regression is
intrinsic to that form. The dense-2-byte recovery is left as an open
follow-on (`TODO.md` `## Next`).

### Why it shipped anyway

Even regressed, qwc on the all-Cyrillic corpus is **~12× `uu-wc`**
(39.7 ms vs 480.7 ms `-w`) and ~70× GNU `wc`; the regression moves us
from ~14× to ~12× ahead of the real competitor, not into "slow"
territory. Shipped **default ON**; `-DQWC_NEON_NIBBLE=OFF` restores the
`vaddv` path (the A/B baseline for the follow-on). **AVX2 has no
counterpart** — `_mm256_movemask_epi8` is already one instruction — so
this is NEON-only and **not a release-parity blocker**.

### Validation

`qwc_tests` 215/215 (+1 env-skip), `scripts/check-format.sh` clean, and
conformance `0 failed` under both `LC_ALL=C` and `C.UTF-8`, including an
89,928-case chunk-boundary-stress fuzz (the chunk stress is what
exercises the nibble carry/edge logic). Spec + plan:
`qwc-companion/superpowers/{specs,plans}/2026-06-21-16byte-neon-words-nibble*`.

## Finding 16 — TMA redux: re-examining Findings 8, 9, 10 with `toplev.py`

Measured **2026-06-22** on the i7-8700 (AVX2, 12 logical CPUs, GCC 16.1.1)
via `scripts/bench/toplev.sh` (Rung 2 workflow, Finding 14). `./qwc-perf`
RelWithDebInfo build for Findings 8 and 10; `/tmp/qwc-pgo-redux-bin`
(resurrected PGO build at commit `a0a0245`, three-stage `build-pgo.sh` with
the `gen-data.py --pgo-training` synthetic 32 MiB training corpora) for
Finding 9. Two consecutive runs per cell (a third for any cell whose
top-level slot categories disagreed by > 3pp between runs). All cells
under `LC_ALL=C` — the regression-sensitive locale for the word kernel.
Spec + plan:
`qwc-companion/superpowers/{specs,plans}/2026-06-22-tma-redux-findings-8-9-10*`.

### TL;DR

- **Finding 8 redux:** Short `-l -w` is **Bad_Speculation-dominated** at
  25.8–26.0% (Branch_Mispredicts), confirming Finding 14's geometry-
  contrast prediction. *But the long-`-l -w` contrast lands at the same
  ~26% BadSpec* — the misprediction cost is **not geometry-specific** for
  `-l -w`; it's flag-specific, driven by the per-word-boundary branch in
  the run state machine that fires every byte transition. Reopens the
  rescue shape: the existing
  [[branchless-wordscan-state-update]] TODO entry is the cycle-level
  follow-on, and Finding 16 supplies the empirical motivation.
- **Finding 9 redux:** PGO regression on `long -w` classifies cleanly as
  **Frontend.Fetch_Bandwidth.MITE** — PGO's profile-driven inlining
  bloats the hot path past the DSB (uop cache) capacity, forcing fetch
  through the legacy decoder. Frontend shifts +13pp (LTO-only ~18% →
  PGO ~31%) while Bad_Speculation drops −6pp (PGO does improve branch
  prediction). The MITE penalty outweighs the mispredict win. **Finding
  9 stays dropped** with sharper mechanism evidence — the rescue
  shape ("constrain inlining to fit DSB") is the same family of
  trade-offs Finding 9 already explored across configs (a)/(b)/(c).
- **Finding 10 redux:** **BE/Memory is consistently below display
  threshold** (sub-5%) across all 5 baseline runs on `mixed-256MiB` and
  `big.txt` `(default = -lwc)`. When Backend surfaces (one `big.txt`
  run at 25.8%), it's **BE/Core (Ports_Utilization)**, not BE/Memory.
  The proposed L1-tiling rescue has no cycle-level deficit to recover.
  **Finding 10 stays closed** on cleaner evidence than the original
  60-cell wall-clock sweep.

### Methodology

Three measurement substrates, four total cells:

| Redux | Primary cell | Geometry contrast | Builds compared |
|---|---|---|---|
| 8 | `short -l -w` | `long -l -w` | `./qwc-perf` only |
| 9 | `long -w` | — | `./qwc-perf` (LTO-only) vs `/tmp/qwc-pgo-redux-bin` (PGO) |
| 10 | `mixed -lwc` | `big.txt -lwc` | `./qwc-perf` only |

All cells run with `LC_ALL=C` and `toplev.sh` defaults
(`toplev.py -l3 --no-desc --global`). Multiplex error 1–3% per
top-level slot category; the per-run output suppresses categories below
a (toplev-internal) display threshold, so a "missing" Backend / Frontend
in the table below means slot share fell below the threshold on that
run, not that no measurement happened.

### Finding 8 redux — `-l -w` is BadSpec-dominated on both geometries

Finding 14's geometry-contrast on `-L` showed long-`-L` was Backend-
Memory-bound (76%) while short-`-L` flipped to Bad_Speculation (23.4%
Branch_Mispredicts). The redux prediction was that short `-l -w` would
inherit that geometry-driven BadSpec shape, suggesting a misprediction
rescue for Finding 8's fusion regression.

Top-level slot percentages, both runs per cell:

| Cell | Run | Frontend.FL | BE/Memory | BE/Core | Bad_Speculation | Notes |
|---|---|---|---|---|---|---|
| short `-l -w` | 1 | 17.3% | — | — | **25.8%** | Branch_Mispredicts 25.7% |
| short `-l -w` | 2 | 16.9% | — | — | **26.0%** | Branch_Mispredicts 26.0% |
| long `-l -w` | 1 | 17.4% | — | — | **26.1%** | Branch_Mispredicts 26.1% |
| long `-l -w` | 2 | (n/a) | — | (12.6%) | **25.7%** | BE=22.7% reported this run; FE.FL=33.2% of FE |

Top-level reproducibility ±0.4pp on Bad_Speculation across all cells —
inside the ±3pp floor.

**The prediction lands on short, but the long-input contrast invalidates
the "geometry-specific" half of it.** On `-L`, Finding 14 saw a clean
flip between long-`-L` (Memory-bound) and short-`-L` (BadSpec); on
`-l -w`, both geometries are BadSpec-dominated at the same ~26%. The
implication is that the unpredictable branch is *not* the per-line
newline-locating step (which would vary with line geometry) but the
per-word-boundary check in the run state machine (which fires every byte
transition regardless of line length). Finding 8's fusion regression
was localized in the wrong place — the fusion added per-block branches
to a kernel already saturated on per-byte branch unpredictability,
which TMA now shows is a cycle-level fact rather than a wall-clock
artifact.

**Verdict: confirmed, but rescue shape is broader than Finding 14
sketched.** The cycle-level rescue is **branchless WordScan state-machine
updates** (the existing `## Next` TODO entry, "Branchless state-machine
update for `WordScan` flags") — Finding 16 supplies the empirical
motivation for elevating it. Finding 8's original `-l -w` fusion stays
dropped: fusing more branches into a BadSpec-saturated kernel is the
wrong direction.

### Finding 9 redux — PGO regression is Frontend.Fetch_Bandwidth.MITE

Finding 9 dropped GCC PGO because it regressed the word-counting path
7–10% while gaining 7–12% on `-L`. The mechanism story ("PGO weights
`-L`'s tight loop more heavily, biases inlining away from the word
kernel") was plausible but second-order — wall-clock plus
instruction-count deltas, not direct pipeline observation. The redux
classifies the regression at the cycle level.

Top-level slot deltas, `long -w` under `LC_ALL=C`:

| metric | LTO-only run 1 | LTO-only run 2 | PGO run 1 | PGO run 2 | Δ (PGO − LTO) |
|---|---:|---:|---:|---:|---:|
| Frontend.FL | 17.8% | 18.1% | **30.2%** | **31.7%** | **+13pp** |
| Bad_Speculation | 26.6% | 26.3% | 20.4% | 20.5% | −6pp |
| FE.Fetch_Latency | 27.2% | 25.5% | 11.1% | 15.7% | (multiplex-suppressed on PGO; total FE consistent) |
| FE.Fetch_Bandwidth | 15.6% | 7.3% | **20.7%** | (suppressed) | — |
| FE.Fetch_Bandwidth.MITE (Slots_est) | — | — | **18.9%** | — | — |

PGO does what PGO advertises — branch prediction improves, Bad_Speculation
drops 6pp. But Frontend rises 13pp, and **the rise is in
Fetch_Bandwidth specifically, with the MITE sub-leaf at 18.9% of
slots-estimated**. MITE — the Macro Instruction Translation Engine, the
legacy decoder used when uop-cache (DSB) misses — fires when the hot
path no longer fits the uop cache. PGO grew the hot path through
profile-driven inlining decisions; the path overflowed the DSB; the
frontend now spends ~19% of slots fetching uops through the slower
decoder.

**The mispredict savings don't cover the MITE penalty.** Net: the
regression mechanism Finding 9 inferred from second-order counters
("PGO biases optimization toward `-L`") is now classified directly at
the cycle level as **uop-cache overflow on the word kernel's hot path**.

**Verdict: Finding 9 stays dropped, with sharper evidence.** The rescue
shape — "constrain PGO's inlining decisions to fit the DSB" — is the
same family of trade-offs Finding 9's config sweep already explored
(stage-1 LTO on/off, training data size, etc.), all of which failed.
There's no new lever this classification exposes that wasn't tried.

### Finding 10 redux — no L2-bound headroom for L1 tiling

Finding 10 dropped strip-mining `scanBuffer` to L1-resident strips
after a 60-cell `DEFAULT_FLAGS` × six-corpora wall-clock sweep showed
no consistent shift > 2% on either ISA. The redux asks the underlying
question directly: does the baseline `(default = -lwc)` cell show
BE/Memory headroom — the deficit L1 tiling would have to recover?

Top-level slot percentages across baseline `(default = -lwc)`:

| Cell | Run | Frontend.FL | BE/Memory | BE/Core | Bad_Speculation | Notes |
|---|---|---|---|---|---|---|
| mixed `-lwc` | 1 | **30.8%** | — | — | 15.9% | FE.Branch_Resteers 15.3% of clocks |
| mixed `-lwc` | 2 | 16.7% | — | — | **25.3%** | Branch_Mispredicts dominant |
| mixed `-lwc` | 3 | 17.0% | — | — | **26.1%** | Branch_Mispredicts dominant |
| big.txt `-lwc` | 1 | **31.0%** | — | (17.1%) | 17.9% | BE=25.8%, Ports_Utilization 26.2% of clocks |
| big.txt `-lwc` | 2 | 17.2% | — | — | **22.8%** | Branch_Mispredicts dominant |

Across all five runs, **BE/Memory never surfaces above toplev's display
threshold** (sub-5% slots). When Backend appears at all (big.txt run 1,
25.8%), it lands in **BE/Core** (Core_Bound 17.1%, Ports_Utilization 26.2%
of clocks) — execution-port saturation, not memory hierarchy stalls.

A note on the run-to-run swing on mixed and big.txt: toplev attributes
branch-prediction cost to either `BAD.Branch_Mispredicts` or
`FE.Fetch_Latency.Branch_Resteers` depending on the multiplex slice the
counter group fell into. Across the five runs, the attribution swings
but the total cost (mispredicts + resteers) is consistently the
dominant feature. **BE/Memory's absence is the load-bearing signal**, and
it's stable across every run.

**Verdict: Finding 10 stays closed, on cleaner evidence than the
original 60-cell wall-clock sweep.** The proposed L1-tiling rescue
needed a cycle-level L2/L3-bound deficit to recover; TMA shows no
deficit at any cache level. Strip-mining the `scanBuffer` could not
have moved the wall-clock matrix because the per-byte pipeline is
**branch-bound and core-bound**, not memory-bound, on the multi-counter
default-`-lwc` workload at Finding 6's L2-resident scan buffer.

### Reproducibility and noise floor

Top-level slot categories on the Finding 8 and Finding 9 cells reproduce
to within ±0.4pp (short `-l -w`), ±1.5pp (PGO long `-w` Frontend), and
±0.3pp (LTO-only long `-w`). Finding 10's mixed `-lwc` cell needed a
third run because runs 1 and 2 disagreed by 14pp on the Frontend
attribution — the third run sided with run 2, confirming the
Branch_Mispredicts-dominant shape. The dispersion came from toplev's
attribution of branch cost to FE-vs-BAD across multiplex slices, not
from the underlying workload changing; the *aggregate* branch cost is
stable across all three.

**No `--no-multiplex` escalation was needed.** Findings 8 and 9 were
answered at top-level slot share alone; Finding 10's verdict hinged on
BE/Memory being absent from display, which top-level reads carry as a
present-vs-absent flag (not a leaf delta needing finer counter
attribution).

### Cross-references

- **Finding 8** (`-l -w` fusion null) — amended by §"Finding 8 redux".
- **Finding 9** (PGO regression) — amended by §"Finding 9 redux".
- **Finding 10** (strip-mine null) — amended by §"Finding 10 redux".
- **Finding 14** (TMA classification of `-L` on long-line input) — the
  Rung 2 workflow Finding 16 reuses; the geometry-contrast prediction
  that motivated Finding 8 redux.
- **Finding 12** (per-instruction attribution on CJK) — the per-instruction
  layer Rung 2 sits above; both layers ship side-by-side, separate
  questions.

## Finding 17 — branchless `WordScan` state update: TMA rescue works, wall-clock regresses (null)

Measured **2026-06-22** on the i7-8700 (AVX2, 12 logical CPUs, GCC 16.1.1).
`./qwc-perf` RelWithDebInfo build for Rungs 1–2; the standard `sweep.sh`
candidate build (LTO, `-march=x86-64-v3`) for Rung 3. All TMA cells under
`LC_ALL=C`. Spec + plan:
`qwc-companion/superpowers/{specs,plans}/2026-06-22-branchless-wordscan-state-update*`.

### TL;DR

- **Rung 2 confirmed the rescue mechanism.** Bad_Speculation dropped
  **9.0–10.3pp** across all four cells (`short`/`long` × `-l -w`/`-w`
  under `LC_ALL=C`). The drop was the same on `-l -w` and `-w`-only,
  refining Finding 16's "geometry-invariant on `-l -w`" reading to
  **`-w`-specific** — the unpredictable branches are in the run state
  machine, fired by every word-byte transition, independent of `-l`.
- **Rung 3 falsified the wall-clock prediction.** The sweep matrix
  regressed: **+5–7%** on C-locale `-w` cells, **+18–23%** on C.UTF-8
  `-w` cells. The falsification floor (1.5%) was blown on every word-
  counting cell, AVX2-fast-path included.
- **Diagnostic:** retired instructions **+10% (C-locale)** to **+42%
  (C.UTF-8)** despite branch-misses dropping **~20%** as predicted.
  The branchless rewrite trades unpredictable branches for unconditional
  stores; the stores retire more cost than the mispredict savings recover.
- **Methodology lesson.** TMA's Bad_Speculation share is a *normalized
  slot fraction*, not an absolute cost. A drop in BadSpec slot share
  does not predict wall-clock if Retiring slot share rises in concert.
  Future probes that gate on a TMA BadSpec signal should pair the
  reading with an executed-instructions A/B from the start.
- **Verdict: dropped.** Branch parked on `branchless-word-scan` for
  archaeology; the change does not ship. Both `step()` and `stepMasks()`
  fast-path rewrites move the regression in the same direction
  (per-byte vs per-block stores, same mechanism), so no narrower
  carve-out salvages the win.

### Methodology

Single AVX2 branch (`branchless-word-scan`) touching only
`include/words_kernel.h` — both `step()` (the scalar / per-byte path)
and the fast-path arm of `stepMasks()` (the SIMD per-block path)
rewritten to straight-line bitop updates. The slow-path run-walker
inside `stepMasks` deliberately not touched: barren-run policy is open,
and rewriting the walker would harden semantics that may flip.
Correctness verified by full `qwc_tests` green on both AVX2
(`build-perf`) and scalar (`build-scalar`) builds plus the `--quick`
conformance run against GNU `wc 9.4`.

Three measurement rungs in order. Each gated the next.

| Rung | Substrate | Cells |
|---|---|---|
| 1 | `perf-annotate.sh` (top-symbol report) | `mixed -l -w` `LC_ALL=C` |
| 2 | `toplev.sh` (top-level TMA slot shares) | `{short, long}` × `{-l -w, -w}`, `LC_ALL=C`, ≥ 2 runs/cell |
| 3 | `sweep.sh` (the standard 7-corpus × 10-flag × 2-locale matrix) | seven 256 MiB corpora, all flags, both locales |

Per Finding 16's protocol: top-level slot reproducibility floor
±3pp; a third run if the first two disagreed by more on
Bad_Speculation. One such tie-breaker fired (long `-l -w` after-rewrite
run 1 fell below toplev's display threshold for BadSpec, a multiplex
attribution artifact; runs 2 and 3 agreed at 16.5–16.7%).

### Rung 1 — top-symbol shape stable

| | `words` (isra.0) | `rep_movs_alternative` | `count` |
|---|---|---|---|
| baseline | 79.97% | 10.20% | 3.94% |
| after | 81.89% | 10.56% | 3.59% |

`words` share crept up; with ~670 samples per single-run report the
shape is reproducibility-noise indistinguishable from the baseline.
Rung 1 alone could not have answered the wall-clock question.

### Rung 2 — Bad_Speculation drops 9–10pp on every cell

| Cell | Baseline avg | After avg | Δ |
|---|---|---|---|
| short `-l -w` | 25.2% | 16.1% | **−9.1pp** |
| long `-l -w`  | 25.6% | 16.6% | **−9.0pp** |
| short `-w`    | 26.7% | 16.4% | **−10.3pp** |
| long `-w`     | 26.8% | 17.0% | **−9.8pp** |

The displacement showed up as Frontend_Bound rising **~+10–13pp** in
concert (e.g. long `-l -w` Frontend 17.4% → 31.1% across runs) —
the saved BadSpec slots are still cycle cost, just reattributed once
the speculation window closes. Retiring share rose **~+1–3pp**
absolute (e.g. short `-l -w` Retiring 32.0% baseline → 33.4% after),
small in proportional terms but, as Rung 3 shows, large enough in
absolute slots to dominate the wall-clock outcome.

This is the symmetric inverse of Finding 14's `-L` short/long contrast:
there, Bad_Speculation cost was geometry-specific (only short, because
short-line input shortens the loop trip distance and starves the
predictor); here, Bad_Speculation cost is **flag-specific to `-w`** and
fires equally on short and long, because the unpredictable branches are
inside the run state machine that visits every byte regardless of line
length.

### Rung 3 — wall-clock regressed; floor blown

Selected rows from the 7-corpus sweep matrix
(`logs/branchless-wordscan/rung3-sweep.log`). Times in ms; "vs v0.3.0"
shows the candidate / baseline time ratio.

| Corpus | locale / flag | candidate | v0.3.0 | ratio |
|---|---|---|---|---|
| big.txt | C / `-w` | 32.3 | 30.7 | **1.05× slower** |
| big.txt | C / `-l -w` | 33.3 | 31.6 | 1.05× slower |
| big.txt | C.UTF-8 / `-w` | **48.7** | **39.9** | **1.22× slower** |
| big.txt | C.UTF-8 / `-l -w` | 49.5 | 40.9 | 1.21× slower |
| mixed | C / `-l -w` | 37.2 | 34.7 | 1.07× slower |
| mixed | C.UTF-8 / `-w` | **54.1** | **44.1** | **1.23× slower** |
| mixed | C.UTF-8 / `-l -w` | 55.2 | 45.4 | 1.22× slower |
| long  | C / `-l -w` | 33.3 | 31.6 | 1.05× slower |
| long  | C.UTF-8 / `-w` | 48.5 | 39.7 | 1.22× slower |
| short | C / `-l -w` | 37.4 | 34.7 | 1.08× slower |
| short | C.UTF-8 / `-w` | 54.2 | 44.7 | 1.21× slower |

Every C-locale `-w` cell regressed 5–8%. Every C.UTF-8 `-w` cell
regressed 18–23%. The falsification floor (1.5%) was the trigger;
multiple cells exceeded it by an order of magnitude.

### Diagnostic — retired-instruction blow-up explains the regression

`perf stat -r 5` on the two cleanest regressors:

| Cell | metric | candidate | v0.3.0 | Δ |
|---|---|---|---|---|
| `big -w` C.UTF-8 | instructions | 1.861G | 1.314G | **+42%** |
| | branch-misses | 8.49M | 10.68M | −20% |
| | cycles | 972M | 781M | +24% |
| | time (s) | 0.0496 | 0.0403 | +23% |
| `mixed -l -w` C | instructions | 969M | 878M | **+10%** |
| | branch-misses | 6.29M | 7.96M | −21% |
| | cycles | 665M | 633M | +5% |
| | time (s) | 0.0335 | 0.0315 | +6% |

Two clean signals:

1. **Branch-misses fell ~20% on every cell**, confirming the rescue
   mechanism — the per-byte branches in the run state machine WERE
   unpredictable, and the branchless rewrite removed them as
   predicted.
2. **Retired instructions blew up.** The C-locale path takes
   `stepMasks` fast-path almost exclusively (only the scalar epilogue
   hits `step`), so +10% reflects the fast-path rewrite alone. The
   C.UTF-8 path sends every block with a byte ≥ 0x80 through
   `scalarUtf8` → `step` per byte; +42% reflects step's rewrite cost
   amplified by the punt path. The extra 32pp is the per-byte `step`
   cost.

The static disassembly tells the same story: `words(...)` grew from
**1125 to 1364 instruction lines** (+21% function size). The fast-path
arm picked up the carry-correction's `xor` + `and` + `sub`, three
movzbl loads of state fields that the conditional original could keep
in flags, and the `if (sMask != 0)` collapse — net several extra
instructions per 32-byte block. On a 256 MiB workload at 8M blocks, even
~10 extra instructions per block is 80M extra retired instructions,
which is exactly the C-locale regression scale.

`step()` is the bigger offender: the branchless form writes **all**
state fields unconditionally — `s.inWord`, `s.runHasPrintable`,
`s.sawSeparator` — where the conditional original was effectively a
NO-OP on the common path (a non-Sep byte after a non-Sep byte hits
`s.inWord = true` already-true, skipped by the predictor as a
well-predicted-out branch with no store). Turning that NO-OP into a
real store retires cost on every byte; on the punt path that means
millions of bytes paying for what was almost free before.

### Why TMA's BadSpec rescue didn't predict wall-clock

TMA's slot-share categories — Bad_Speculation, Frontend_Bound,
Backend_Bound, Retiring — sum to 100%. The arithmetic of slot-share
shifts:

- BadSpec dropped ~9pp (the rescue).
- Frontend_Bound rose ~+10–13pp (mispredict cost reattributed to fetch
  resteers once speculation closed).
- Retiring rose ~+1–3pp absolute. **Small as a slot-share delta —
  large as absolute slot count when the workload retires hundreds of
  millions of additional unconditional stores.**

The slot-share view normalizes away the issue: a 26% → 16% BadSpec
drop with a 32% → 35% Retiring rise looks like a clean win at the
top-level reading. The absolute view (executed instructions × IPC)
inverts it.

Finding 16's redux protocol was correct for *naming* the bottleneck
(it correctly identified BadSpec as the dominant slot share in `-l -w`
and `-w`) but insufficient for *predicting the wall-clock impact of a
rescue* (because the rescue's instruction-count cost lives in
Retiring, which the redux didn't gate on). The methodology refinement
is: pair a TMA BadSpec rescue probe with a Rung-1.5 `perf stat -r N`
A/B that includes the executed-instructions delta, run *before* the
sweep matrix — that catches +10% / +42% instruction inflation in a
cheaper, cleaner measurement than the 60-cell matrix and would have
preempted Rung 3 here.

### Verdict

**Dropped.** The `branchless-word-scan` branch stays parked for
archaeology; the production tree is untouched (the rewrite was reverted
before the diff committed, only this Finding 17 lands on `main`). The
TODO entry "Branchless state-machine update for `WordScan` flags"
moves from `qwc-companion/TODO.md` to `qwc-companion/NOT-DOING.md`
with the falsification recorded.

No narrower carve-out is on the table: `step()`'s per-byte stores and
`stepMasks()` fast-path's per-block stores both regress wall-clock
through the same mechanism. The slow-path run-walker stays scoped
out — its rewrite would face the same store-cost wager AND depends on
unsettled barren-run semantics, two blockers stacked. The remaining
~16–17% BadSpec on `-w` is now classified as a real cycle cost the
current shape cannot recover without a different algorithm
(packed `WordScan` fields reducing the store width? merging fields so
fewer addresses are touched per byte?), which is a separate design
question and a different ticket.

### Cross-references

- **Finding 14** — TMA on `-L` long lines; the workflow Finding 17 reuses.
- **Finding 16** — TMA redux that motivated this rescue; the BadSpec
  attribution it produced is confirmed, the wall-clock rescue
  hypothesis it implied is falsified here.
- **Finding 8** — the original `-l -w` fusion null. Stays dropped;
  Finding 17 didn't open a fusion rescue.

## Finding 18 — `qwc --validate-csv` beats `zsv check --parser fast`

New `qwc --validate-csv` command proving CSV rectangularity (constant
field count per record, quoted content masked). Benchmarked against
`zsv check --parser fast` (zsv 1.4.3, the branchless-SIMD NEON parser),
which performs the same column-count anomaly check. Apple Silicon
(14 cores), four deterministic ~268 MiB corpora from
`benchmarks/gen-csv.py`, warm page cache, best-of-7 wall-clock.

| corpus (~268 MiB) | qwc | zsv | speedup |
| --- | --- | --- | --- |
| `unquoted` (valid, no quotes) | 19 ms | 238 ms | **12.4×** |
| `sprinkled` (1-of-6 quoted, embedded `,`/`\n`) | 136 ms | 235 ms | 1.7× |
| `heavy` (every field quoted) | 131 ms | 231 ms | 1.8× |
| `ragged` (`--fast`, bad last row) | 19 ms | 233 ms | **12.4×** |
| `ragged` (default, names the row) | 273 ms | 233 ms | 0.86× |

**Headline: 12.4× on the unquoted case** — the dominant real-world shape
and the Phase-1-only fast path (NEON `vshrn` nibble-mask scan + parallel
chunking; no two-assumption work because no chunk is "dirty"). zsv is
single-threaded; qwc's win is SIMD-per-chunk **and** 14-way parallelism,
and the validator does far less than zsv's full parse.

**Quoted corpora win 1.7–1.8× with the Phase-2 walk still scalar.** Only
quote-bearing chunks take the two-assumption quote-aware pass, and even
run scalar-per-chunk that pass is parallel across chunks, which already
beats single-threaded zsv. This measurement is why **the NEON Phase-2
kernel was dropped as YAGNI** (the design spec's `vmull_p64` prefix-XOR
Phase-2): there is no zsv deficit to close on the quoted path. A NEON
Phase-2 would push the quoted multiplier higher but is not needed to win,
and the error-prone backslash escaped-mask SIMD is not worth the risk
absent a measured target. Spun off as a `## Next` follow-up.

**The one loss is by design.** Default-mode `ragged` (0.86×) is slower
than zsv only because, after the parallel pass detects the violation, the
inspection step re-reads the file sequentially to pinpoint the exact bad
row — and the bad row here is the very last one, so it scans ~the whole
file. `--fast` skips inspection and `_Exit(1)`s on first detection,
restoring 12.4×. (A seeded re-scan from the offending chunk instead of
from byte 0 would bound this; deferred — failure-with-diagnosis is the
exceptional path and `--fast` already covers the latency-sensitive case.)

Output / row-number convention: a valid file prints nothing; an invalid one
prints `<name>: <ragged rows>` to stdout (`--all`, the default), selectable
down to `--first` / `--list` / `--fast`. Row numbers are 1-based over logical
records **including the header** (header = row 1), so on the `ragged` corpus
qwc reports row 4467653 where zsv reports "Row 4467652" (zsv excludes the
header from its count). Both name the same physical line.

**Validation.** `qwc_tests` green incl. a 1200-case dual-dialect fuzz vs
an independent oracle and byte-by-byte chunk-seam stress; ASan+UBSan and
TSan clean on the threaded path; `conformance/csv/diff_csv.py` 600 cases
× {NEON, scalar} binaries × {C, C.UTF-8} all matched (validation is
byte-oriented, so the locale is provably irrelevant).

**Port status:** NEON + scalar reference shipped. **AVX2 is the flagged
release-parity blocker** — the Phase-1 nibble scan maps to
`_mm256_movemask_epi8` and (if Phase-2 is ever built) the prefix-XOR to
`_mm_clmulepi64_si128`. An x86 release must not ship `--validate-csv`
until the AVX2 kernel lands and passes the same gate.

**Update:** the "Phase-2 dropped as YAGNI" verdict above was **reversed by
Finding 19** — the SIMD Phase-2 was built and is a 3–4× win on the quoted
corpora, so the shipping quoted numbers are now ~5–8× vs zsv, not the 1.7–1.8×
recorded here. This entry's quoted cells are the *scalar-Phase-2* baseline.

## Finding 19 — NEON SIMD Phase-2 (prefix-XOR in-quote mask): 3–4× on quoted CSV

Reverses Finding 18's "Phase-2 dropped as YAGNI". The deferred SIMD Phase-2
kernel was built and measured against the scalar Phase-2 it replaces (A/B via
`-DQWC_CSV_NEON_PHASE2=OFF`), Apple Silicon 14-core, 268 MiB corpora,
best-of-11 wall-clock, vs `zsv check --parser fast` 1.4.3.

| corpus (~268 MiB) | P2-ON | P2-OFF (scalar) | zsv | **ON/OFF** | ON vs zsv |
| --- | --- | --- | --- | --- | --- |
| `unquoted` (control, no dirty chunks) | 18.9 ms | 19.0 ms | 233 ms | 1.01× | 12.3× |
| `sprinkled` (1-of-6 quoted) | 45.6 ms | 136.7 ms | 237 ms | **3.00×** | 5.2× |
| `heavy` (every field quoted) | 30.6 ms | 131.1 ms | 237 ms | **4.28×** | 7.7× |

**Mechanism.** A dirty chunk's per-byte scalar walk (run twice, once per entry
hypothesis) is replaced by: three bit-per-byte movemasks (quote / delim /
newline via the `vaddv` powers-of-two reduction), one **prefix-XOR of the quote
mask** (`vmull_p64`/PMULL carryless-multiply-by-all-ones — the simdjson idiom;
a 4-step Kogge-Stone shift-XOR ladder where the crypto extension is absent), and
two cheap masked segmentations sharing that one construction (`Q_in = ~Q_out`,
so `real_out = bits & ~Q`, `real_in = bits & Q`). The mask build happens **once**
for both hypotheses; the running entry-outside quote parity carries across
16-byte blocks; the < 16-byte tail steps both hypotheses scalar. Doubled-quote
`""` falls out for free (two adjacent toggles cancel in the prefix-XOR).

**Why heavy beats sprinkled** despite more quotes: heavy masks nearly every
delimiter/newline (they sit inside quotes), so `segmentBits16` iterates almost
no real newlines — the segmentation, not the mask build, is the variable cost.

**Both dialects.** RFC-4180 (doubled-quote) and the backslash (`--esc`) dialect
are both SIMD. Backslash adds the simdjson odd-backslash-run mask
(`findEscaped16`, `prevEscaped` carried across blocks) to clear escaped
quotes/delims/newlines before the prefix-XOR; on `-esc` corpora it is
**5.3× (sprinkled) / 4.1× (heavy)** over the scalar walk (`-DQWC_CSV_NEON_PHASE2=OFF`;
zsv is not a baseline here — it does not implement backslash escaping).
`unquoted` is unchanged (no chunk is dirty, so Phase-2 never runs): the
Finding-18 fast path is untouched.

**Correctness bug found en route (now fixed).** The backslash SIMD work added a
hard fuzz (random-length backslash runs straddling block *and* chunk
boundaries), which exposed a **pre-existing** driver bug: a quote-free chunk was
classified "clean" and counted by the escape-blind Phase-1 kernel, so an escaped
delimiter/newline (`\,` / `\⏎`) in a quote-free chunk — or one escaped by a `\`
carried from the previous chunk — was miscounted, yielding wrong verdicts on
`--esc` input at chunk boundaries. Fix: in backslash mode every chunk takes the
quote-aware path with its resolved entry-escape (no quote-blind fast path). RFC
mode is unaffected. The fuzz is now a permanent regression guard.

**Lesson recorded.** Finding 18 dropped this as YAGNI from the *parallel scalar
Phase-2 already beating zsv 1.7×* — true, but "beats the baseline" set the bar
at the wrong tool. The right question was "is there headroom in our own kernel",
and there was a 3–4×. Default `QWC_CSV_NEON_PHASE2=ON`; `OFF` keeps the scalar
A/B baseline. Validation: `qwc_tests` green incl. the 1200-case RFC+backslash
fuzz, the 600-case backslash-run stress, and the chunk-seam stress; ASan+UBSan
and TSan clean. **Port status: AVX2 unchanged** — still the release-parity
blocker, now for both phases *and* both dialects; the prefix-XOR maps to
`_mm_clmulepi64_si128`, the bit-per-byte movemask to `_mm256_movemask_epi8`,
and `findEscaped16` ports directly.

## Finding 20 — AVX2 SIMD validate-csv kernel ships (release-parity port)

The AVX2 port of `validate-csv`'s Phase-1 and Phase-2 kernels
(`src/validatecsv_avx2.cpp`). Bit-identical to NEON / scalar on the
dual-dialect fuzz, backslash-run stress, chunk-seam stress, and the 600-case
conformance harness. `_mm256_movemask_epi8` returns the bit-per-byte mask in
one uop, so NEON's `vshrn` nibble trick is unnecessary on x86. PCLMUL
(`_mm_clmulepi64_si128`) is the x86 counterpart of NEON's PMULL `vmull_p64`
for the prefix-XOR; the 32-bit-wide `findEscaped32` is the direct widening of
NEON's `findEscaped16`. Build-flag note: PCLMUL is **not** in the psABI
`x86-64-v2` / `v3` levels even though every AVX2-capable CPU has it, so
`-mpclmul` is added explicitly to `QWC_SIMD_FLAGS` on top of `-march`.

**Box.** Intel Core i7-8700 (Coffee Lake, 6 cores / 12 threads, 3.20 GHz
base), 31.3 GiB RAM, Arch Linux kernel 7.0.12; GCC 16.1.1, `-O3 -DNDEBUG
-march=x86-64-v3 -mpclmul`, LTO on. Warm page cache,
page-cache-resident 268 MiB corpora; `bench-csv.py --runs 11` (median wall).

**Headline (`qwc --validate-csv` vs `zsv check --parser fast` 1.4.3).**

| Shape              | `qwc` ms | `zsv` ms | qwc speedup | Notes                              |
| ------------------ | -------- | -------- | ----------- | ---------------------------------- |
| `unquoted`         |     68.8 |    389.9 |       5.66× | Phase 1 only (no dirty chunks)     |
| `sprinkled`        |     97.0 |    378.9 |       3.90× | mostly Phase 1, occasional Phase 2 |
| `heavy`            |    119.2 |    329.5 |       2.76× | every chunk Phase 2                |
| `ragged --fast`    |     69.5 |    394.7 |       5.68× | fail-fast path                     |
| `ragged` (default) |    902.3 |        — |          —  | inspection re-scan tax (cf. F18)   |

Lower headline ratios than Finding 18 (Apple Silicon 14-core: 12.4× /
12.4× / 5.2× / 7.7×) — expected: this box has 6 physical cores against
M-series 14, and qwc's win on `unquoted` / `ragged --fast` is dominated by
cross-core parallelism over the same per-byte work. The ratios remain
substantial across all shapes.

**SIMD vs scalar Phase 2 (x86 A/B, `QWC_CSV_AVX2_PHASE2=ON` vs `OFF`).**
The point of this column is "did the SIMD Phase-2 construction pay off on
x86 the way it did on NEON?" — Finding 19 measured 3–4× there.

| Shape       | SIMD ms | Scalar ms | SIMD speedup | Notes        |
| ----------- | ------- | --------- | ------------ | ------------ |
| `unquoted`  |    68.8 |      69.8 |        1.01× | Phase 1 only, no Phase-2 invocations |
| `sprinkled` |    97.0 |     407.4 |        4.20× | RFC dialect  |
| `heavy`     |   119.2 |     444.2 |        3.73× | RFC dialect  |
| `ragged --fast` | 69.5 |     69.7 |        1.00× | Phase 1 only on the fail-fast path  |

3.7–4.2× on the quoted shapes — matches Finding 19's NEON ratio
(3–4× on `sprinkled` / `heavy`), confirming the prefix-XOR construction
carries its own weight on x86 just as on NEON. The unquoted / ragged-fast
rows show no movement because those workloads never invoke Phase 2 —
they're a control. **Backslash dialect not benched separately on x86: the
kernel differs from RFC only by `findEscaped32 + keep` (the same shape
NEON measured 5.3× / 4.1× on in Finding 19), so the SIMD-vs-scalar ratio
carries over by construction.**

**Reproduce.**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target qwc -j

cmake -S . -B build-csv-noavx2 -DCMAKE_BUILD_TYPE=Release -DQWC_CSV_AVX2_PHASE2=OFF
cmake --build build-csv-noavx2 --target qwc -j

uv run benchmarks/bench-csv.py --qwc ./build/qwc            --zsv zsv --runs 11
uv run benchmarks/bench-csv.py --qwc ./build-csv-noavx2/qwc --zsv zsv --runs 11
```

**Validation.** `qwc_tests` 43/43 green on both `QWC_CSV_AVX2_PHASE2=ON`
and `=OFF` (incl. the 1200-case RFC+backslash fuzz, backslash-run stress,
chunk-seam stress); `conformance/csv/diff_csv.py --iters 600 --qwc
./build/qwc --qwc-scalar ./build/qwc-scalar` matched across {C, C.UTF-8}
× {AVX2, scalar}; ASan+UBSan and TSan clean on the CSV test subset;
`scripts/check-format.sh` clean.

**Outcome.** AVX2 release-parity port shipped; `qwc --validate-csv` is now
safe to include in an x86 release tag. **Port status: AVX2 + NEON +
portable scalar all shipped; no open ports for validate-csv.**

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
