# Changelog

Notable, user-visible changes to `qwc`. Format follows
[Keep a Changelog](https://keepachangelog.com); the changelog is hand-curated,
not generated from commit messages.

## [0.3.0] - 2026-06-21

### Performance

- The AVX2 and NEON word-counting kernels now vectorize clean 3-byte
  UTF-8 sub-rows in-block instead of punting them to a per-byte scalar
  fallback. The largest win lands on CJK text (Hiragana, Katakana, Han
  ideographs, Hangul, Devanagari, ...): on a 256 MiB short-line CJK
  corpus under `LC_ALL=C.UTF-8`, `qwc -w` drops from 219.8 ms (v0.2.1)
  to 73.6 ms — **2.99× wall time**, 69% fewer cycles per byte. The
  same mechanism delivers a smaller but visible gain on mixed-script
  realistic-text workloads where a fraction of the input is multibyte:
  every non-CJK C.UTF-8 `-w` cell in the bench matrix lands at
  1.32–1.35× vs v0.2.1 (big, long, mixed, short, single-line all
  consistent). 4-byte sequences (`F0..F4` lead bytes — emoji,
  supplementary plane) still take the scalar path. C-locale cells,
  ASCII-only input, and every flag bundle other than `-w` /
  default-`-lwc` / `-l -w` are unchanged within ±2%. See
  `benchmarks/README.md` Finding 13.

  Mechanism, for the curious: the kernel's existing "is this block
  cleanly classifiable in-vector?" check — previously a per-lead-byte
  cleanness table that only admitted 2-byte sub-rows — now also
  consults a per-(lead, cont1) table that admits 3-byte sub-rows whose
  Unicode classification is locally unambiguous (the 1024-byte
  `kCandLead3` table excludes overlong encodings, surrogate-lead
  ranges, and code points around the ideographic space whose
  printability flips inside a single E-prefixed range). The dirty path
  is bit-identical to v0.2.1's scalar walk, so counts agree byte-for-
  byte with GNU `wc` on every conformance cell as before.

## [0.2.1] - 2026-06-20

### Performance

- Release binaries are now built with LTO (default-on; opt out with
  `-DQWC_LTO=OFF`). It is perf-neutral across the benchmark matrix — every
  flag bundle is within ±1% of the previous release (`v0.2.0`) — and ships
  because it is essentially free. PGO was evaluated alongside it and dropped:
  GCC profile-guided optimization regressed the headline word-counting paths
  (default, `-w`, `-l -w`) by 7–10% while only helping the rarer `-L` family,
  consistently optimizing the wrong kernel. See `benchmarks/README.md`
  Finding 9.

### Changed

- The Linux Release binary is back down to ~93 KB (`text` ~65 KB), from
  ~350 KB at v0.2.0 and below the v0.1.0 dynamic-link starting point
  (~96 KB) — within ~30% of GNU `wc`'s 70 KB on the same host. The C++
  runtime is still statically linked, but ~110 KB of libstdc++ / libgcc
  machinery (the C++ name demangler that formats "terminate called after
  throwing X" messages, the exception personality routine, and the dwarf2
  stack unwinder) is no longer pulled into the link. The mechanism is
  link-time symbol overrides: a static archive contributes an object only
  when it satisfies an unresolved reference, so defining the relevant leaf
  symbols in qwc as stubs (each calls `_Exit` with a distinct `ExitPoint`
  enum value so the exit status names the leaf if any ever fires) leaves
  the upstream archive members untouched and `--gc-sections` frees
  everything they transitively kept alive. The three overridden trees are
  `libsupc++/vterminate.o`
  (`__gnu_cxx::__verbose_terminate_handler` → demangler), libstdc++'s
  `eh_personality.o` (`__gxx_personality_v0`), and the 18 public symbols
  exported by `libgcc_eh.a`'s `unwind-dw2.o`. To make sure no qwc code path
  can ever land in those stubs, the qwc target compiles (Release only)
  with `-fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables
  -fno-unwind-tables`; the implicit `std::bad_alloc` edges that would have
  thrown become `std::terminate()`, which matches the existing
  fatal-error contract. Debug builds keep the full machinery so debuggers,
  sanitizers and the verbose terminate message work normally — the cut is
  Release-only (gated `#if !defined(__APPLE__) && defined(NDEBUG)` in
  main.cpp; `if( NOT APPLE AND CMAKE_BUILD_TYPE MATCHES
  "Release|RelWithDebInfo" )` in CMakeLists.txt). Apple targets are
  unaffected either way — they get libc++ / libunwind from the dyld
  shared cache, so the same symbols are resolved at runtime against
  system libraries. A separate, smaller cut on the same branch swapped
  `std::sort` (the only introsort instantiation in the binary, on the cold
  `--sort-by-*` path) for `std::qsort` with a free-function comparator,
  dropping another ~12 KB of `__introsort_loop` + `__adjust_heap` +
  `__insertion_sort` text. Counts and output are unchanged; the conformance
  suite agrees on every required cell.

## [0.2.0] - 2026-06-16

### Performance

- Big-file `-l` and `-m` are markedly faster on multi-core hosts. The
  per-thread scan buffer was retuned from 1 MiB to 256 KiB after the sweep
  documented in `benchmarks/README.md` Finding 6: at the old size the
  buffer spilled out of L2 on every host we measured, so `pread`'s
  `copy_to_user` paid two extra DRAM trips per byte; at 256 KiB it stays
  L2-resident. On a native i7-8700 with a warm 512 MiB file, 4-vCPU pinned
  `-l` drops from 0.85× vs uu-wc to **1.77×** (29.9 ms vs 53.0 ms), and
  `-m` matches (25.3 ms vs 44.8 ms). At all 12 threads the big-file `-l`
  win is **2.36×** (20.5 ms vs 48.4 ms). LLC-load-misses on the headline
  workload fall from ~91k at 1 MiB to 668 at 256 KiB. Counts are
  unchanged.
- `-L` (longest-line scan) benefits from the same L2-residency mechanism
  Finding 6 documented, even though Finding 6's sweep didn't cover it.
  Measured after the fact on the same native i7-8700 (warm 512 MiB
  corpora, all 12 threads), main is faster than v0.1.0 on every
  `-L`/`-L -m` cell. The bandwidth-bound long-lines corpus is the
  cleanest mechanism win: `-L` on `long-512MiB` drops from 31.2 ms →
  20.7 ms (**1.50×**), `-L -m` from 32.1 ms → 20.7 ms (**1.56×**). The
  1 MiB buffer was spilling L2 and adding a second DRAM pass per byte
  for the kernel; 256 KiB stays in L2 and removes it. Compute-bound
  corpora keep a smaller but consistent gain (`big.txt` / `mixed-512MiB`
  `-L` 1.16–1.18×, `-L -m` ~1.07×). Counts are unchanged.
- `qwc` starts ~1.6× faster (2.6 → 1.6 ms on the measurement box, vs GNU wc's
  1.4 ms), which dominates short invocations like `qwc -c <file>` that never
  scan the file at all. Two changes: libstdc++/libgcc are now linked
  statically, removing the dynamic-loader relocation and page-fault cost that
  made up most of the gap to `wc`, and all output goes through stdio instead
  of iostreams. Output is byte-identical to before.
- Bare `qwc -c <file>` is now faster than GNU `wc -c` on the measurement box
  — the first time qwc has measured ahead on the bytes-only invocation. Four
  small cuts closed the residual ~0.2 ms left over after the static-link
  work: `std::thread::hardware_concurrency()` no longer runs as a static
  initializer (the sysconf reading `/sys/devices/system/cpu/online` is now
  paid lazily, and the lone-file `-c` and `-l` paths skip it entirely);
  `setlocale(LC_CTYPE, "")`, `nl_langinfo(CODESET)`, and
  `getenv("POSIXLY_CORRECT")` are gated on `-w`/`-m`, so bare `-c`/`-l`
  never fault in libc's cold locale pages; `collectFiles` short-circuits
  when `--recursive` is off; and the binary is built with
  `-ffunction-sections -fdata-sections -Wl,--gc-sections` (GNU ld / lld
  only), shrinking the ~276 minor page faults the kernel paid to map the
  354 KB static image on startup. Counts and output are unchanged.

### Changed

- The binary is larger: ~350 KB, up from ~96 KB, the price of the statically
  linked C++ runtime (it would have been 1.8 MB; dropping `<filesystem>` and
  `<string>` from the implementation clawed back the rest). It no longer
  depends on `libstdc++.so`/`libgcc_s.so` at runtime.
- The `--recursive` directory walk is hand-rolled on `opendir`/`readdir`
  instead of `std::filesystem`. Behavior is unchanged (unreadable
  subdirectories are skipped, directory symlinks are not followed, a symlink
  to a regular file is counted) and verified against `find -type f`.

### Added

- `CONTRIBUTING.md`: build, test, benchmark, and PR guidance.
- A clang-format CI gate (`scripts/check-format.sh`, also usable locally with
  `--fix`); the whole tree was reformatted once to make it pass.

## [0.1.0] - 2026-06-12

Initial tagged release: a drop-in `wc` (`-l`, `-w`, `-c`, `-m`, `-L`, stdin,
multi-file totals) with GNU-conformant counting verified by a fuzzing
conformance suite, SIMD kernels (AVX2 on x86-64, NEON on arm64) with a scalar
fallback that is the correctness oracle, threaded scanning of large files and
file lists, and qwc extensions: `--char`, `--recursive`, `--sort-by-*`,
`--top`, `--reverse`.
