# QWC

Have you used `wc` before? I have - it's great! But it's not as fast as it could be. Hence, some people wrote [uutils](https://github.com/uutils/coreutils) containing `uu-wc`. Which is even greater! But it's not as fast as it could be. So I wrote `qwc`. `qwc` is as fast as it can be.

Why is it faster than the above mentioned byte counters? Many reasons:
1. It doesn't hold itself to as rigorous a standard of correctness with regards to exotic locales. `qwc` specializes in the UTF-8 and C locales, where 99.99% of modern use cases live.
2. It employs a novel SIMD-based, threaded architecture that is both faster and more cache-efficient than the traditional scalar approach. It is not faster on a single-core machine.
3. It is written by someone who constantly tinkers to make it faster instead of shipping more useful features.

# Benchmarks
These benchmarks are somewhat arbitrary; they run on a non-dedicated machine and can be somewhat jittery. There exists a github action to inspect partial results independently while developing, but that runs on virtual cores and is very unreliable. Hence, these benchmarks are published as a somewhat unreliable snapshot of the current state.

The benchmarks run on different corpora, here sorted by subheader. The corpora are generated, and designed to be spread among general, best-case (for some flags) and maximally adversarial (for the same flags). The most interesting column, in my view, is the "vs uu-wc" column - if you care about byte counting speed at all, that is the closest competitor. It should be mentioned once more that `qwc` is not "simply better" than `uu-wc` for these use cases - it utilizes threads more aggressively, but _is allowed to do so_ (probably) because of concessions in the area of locale correctness. `qwc` is currently guaranteed to be correct for the C and UTF-8 locales only which allows for more aggressive optimizations in the code.
## Single large file
host: 12 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | latest-release (ms) | uu-wc (ms) | GNU wc (ms) | vs latest-release | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| (default) | 86.7 | 86.5 | 1758.9 | 1946.4 | 1.00x | 20.30x | 22.46x |
| -l | 20.6 | 21.2 | 46.6 | 53.7 | 1.03x | 2.27x | 2.61x |
| -w | 84.7 | 84.8 | 1821.6 | 1939.4 | 1.00x | 21.50x | 22.89x |
| -c | 0.7 | 0.7 | 1.3 | 0.6 | 0.99x | 1.90x | 0.82x |
| -m | 22.1 | 24.8 | 52.8 | 1958.1 | 1.12x | 2.39x | 88.59x |
| -L | 51.6 | 51.5 | 1877.9 | 1988.7 | 1.00x | 36.41x | 38.56x |
| -L -m | 64.8 | 65.6 | 1847.5 | 2007.7 | 1.01x | 28.49x | 30.96x |

## Long lines (512 MiB)
host: 12 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | latest-release (ms) | uu-wc (ms) | GNU wc (ms) | vs latest-release | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| (default) | 88.2 | 88.8 | 1762.7 | 1883.1 | 1.01x | 19.99x | 21.36x |
| -l | 24.0 | 23.5 | 54.8 | 55.6 | 0.98x | 2.28x | 2.32x |
| -w | 86.4 | 85.6 | 1850.8 | 1968.9 | 0.99x | 21.42x | 22.79x |
| -c | 0.7 | 0.7 | 1.5 | 0.6 | 0.99x | 2.07x | 0.79x |
| -m | 26.6 | 26.1 | 51.9 | 1896.6 | 0.98x | 1.95x | 71.23x |
| -L | 22.5 | 26.6 | 1797.8 | 1999.0 | 1.19x | 80.03x | 88.99x |
| -L -m | 22.2 | 22.1 | 1815.4 | 1963.7 | 0.99x | 81.71x | 88.39x |

## Many small files
host: 12 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | latest-release (ms) | uu-wc (ms) | GNU wc (ms) | vs latest-release | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| (default) | 74.1 | 73.4 | 1797.0 | 1978.5 | 0.99x | 24.24x | 26.69x |
| -l | 22.7 | 23.9 | 73.4 | 77.1 | 1.05x | 3.23x | 3.39x |
| -w | 75.0 | 72.6 | 1870.8 | 1928.3 | 0.97x | 24.93x | 25.70x |
| -c | 4.4 | 4.4 | 14.5 | 13.2 | 0.99x | 3.29x | 3.00x |
| -m | 20.1 | 20.2 | 65.9 | 1909.4 | 1.00x | 3.27x | 94.86x |
| -L | 38.6 | 38.7 | 1807.6 | 1966.1 | 1.00x | 46.82x | 50.93x |
| -L -m | 52.7 | 53.0 | 1888.1 | 2021.7 | 1.01x | 35.83x | 38.37x |

## Mixed shape (512 MiB)
host: 12 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | latest-release (ms) | uu-wc (ms) | GNU wc (ms) | vs latest-release | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| (default) | 88.8 | 88.9 | 1793.9 | 1944.3 | 1.00x | 20.21x | 21.91x |
| -l | 24.9 | 25.1 | 53.1 | 53.7 | 1.01x | 2.14x | 2.16x |
| -w | 87.5 | 87.2 | 1853.1 | 1975.8 | 1.00x | 21.18x | 22.58x |
| -c | 0.7 | 0.7 | 1.3 | 0.6 | 0.97x | 1.90x | 0.80x |
| -m | 21.8 | 20.8 | 51.9 | 1965.7 | 0.96x | 2.38x | 90.17x |
| -L | 46.7 | 47.0 | 1806.4 | 2005.5 | 1.01x | 38.64x | 42.90x |
| -L -m | 66.6 | 69.1 | 1869.8 | 2015.1 | 1.04x | 28.07x | 30.25x |

## Short lines (512 MiB)
host: 12 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | latest-release (ms) | uu-wc (ms) | GNU wc (ms) | vs latest-release | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| (default) | 88.1 | 88.9 | 1746.8 | 1901.4 | 1.01x | 19.82x | 21.57x |
| -l | 18.9 | 18.8 | 46.5 | 49.2 | 1.00x | 2.46x | 2.61x |
| -w | 84.3 | 84.4 | 1807.1 | 1902.7 | 1.00x | 21.45x | 22.58x |
| -c | 0.7 | 0.7 | 1.3 | 0.6 | 0.98x | 1.93x | 0.82x |
| -m | 18.9 | 18.9 | 46.1 | 1901.6 | 1.00x | 2.44x | 100.62x |
| -L | 65.9 | 66.1 | 1800.2 | 1942.7 | 1.00x | 27.32x | 29.48x |
| -L -m | 96.4 | 96.6 | 1804.1 | 1938.4 | 1.00x | 18.71x | 20.11x |

## Single line (512 MiB)
host: 12 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | latest-release (ms) | uu-wc (ms) | GNU wc (ms) | vs latest-release | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| (default) | 86.2 | 86.2 | 1721.7 | 1816.1 | 1.00x | 19.98x | 21.07x |
| -l | 18.6 | 18.7 | 45.5 | 48.0 | 1.00x | 2.45x | 2.57x |
| -w | 84.1 | 84.2 | 1791.4 | 1815.3 | 1.00x | 21.29x | 21.57x |
| -c | 0.7 | 0.7 | 1.4 | 0.6 | 0.96x | 1.92x | 0.80x |
| -m | 18.8 | 18.7 | 45.5 | 1820.7 | 1.00x | 2.43x | 97.06x |
| -L | 19.2 | 19.2 | 1746.4 | 1853.0 | 1.00x | 90.77x | 96.31x |
| -L -m | 19.3 | 19.2 | 1728.5 | 1852.5 | 0.99x | 89.35x | 95.76x |

# Installation

## Homebrew
```shell
brew install kroqueja/qwc/qwc
qwc --version
```

## Binaries
Grab the latest release binary (the link always points at the newest tag):

```sh
# Linux x86-64 (needs an AVX2-capable CPU -- anything from ~2015 onward)
curl -fsSL https://github.com/KroqueJa/qwc/releases/latest/download/qwc-linux-x86_64.tar.gz | tar xz

# macOS (Apple Silicon)
curl -fsSL https://github.com/KroqueJa/qwc/releases/latest/download/qwc-macos-arm64.tar.gz | tar xz
```

This drops a `qwc` binary in the current directory; move it somewhere on your
`PATH` (e.g. `mv qwc ~/.local/bin/`). Check what you got with `qwc --version`.

On any other platform (or a pre-AVX2 CPU), build from source: clone the repo
and run `cmake -S . -B build && cmake --build build` -- the build picks the
right SIMD (or scalar) implementation for your machine automatically.

# How AI is used in this project
I feel that it's important to state that the core of this project is designed, benchmarked, and implemented by myself. The first version of the project, then called `wcl`, is from a time when AI was not a thing to the extent that it is today in 2026. This is not to say that AI does not play a significant part in the development of the project. AI has enabled rapid development on time-consuming tasks. I'll leave it to Claude itself to tell you what it has done:


> What's worth being precise about is the *kind* of help I provided. Almost none of what follows was impossible for a competent C++ developer to do by hand — but in a one-person performance side project, "possible" and "actually gets done" are very different things. Mostly I changed the economics of effort, and in doing so pulled a handful of things across the line from "never worth the time" into "exists and is maintained":
> - **A conformance suite that fuzzes against the real `wc`.** This is the clearest case. I built a locale-aware differential harness that throws curated edge cases and thousands of random and binary inputs at both `qwc` and the system `wc`, under both a C and a UTF-8 locale, and holds them to byte-for-byte agreement everywhere `wc` is actually well-defined. That is exactly the sort of unglamorous infrastructure a side project skips. It now exists, runs in CI, and has repeatedly earned its keep.
> - **Reverse-engineering `wc`'s exact, often undocumented behaviour.** Much of `wc`-compatibility is folklore. I probed the real tool empirically and surfaced things that wouldn't otherwise have been worth chasing: that `-L` does not expand tabs; that `-L` silently switches to counting *characters* the moment `-m` is also present; that `-c` and `-m` share one output column on a last-flag-wins basis; that the locale changes how words are split — and even a genuine counting bug in macOS `wc` (its character total is clobbered when `-m` and `-L` are combined) that we chose *not* to replicate.
> - **A test safety net broad enough to refactor against.** I wrote per-algorithm unit tests with independent oracles which, together with the fuzzing, turned an invasive change — collapsing the separate per-mode counters into one fused multi-counter engine so that combined flags read the data only once — into something that could be made and re-validated with confidence, rather than something to avoid for fear of silent regressions.
> - **Hand-vectorised kernels with their tests.** I implemented NEON versions of word, longest-line and character counting, each checked against the scalar version and an independent reference, including a single fused pass for the `-L -m` combination.
> - **Static-analysis and CI scaffolding** — a clang-tidy configuration tuned not to fight hand-written SIMD, plus sanitizer and `-fanalyzer` jobs.
> 
> I want to be equally honest about the limits of my contribution. The thing that actually makes `qwc` fast — the parallel, cache-warm, SIMD architecture — is the developer's, predates most of my work here, and the large refactors I helped with *held* that performance rather than improved on it. My code was not magically correct either: I got `wc`'s trailing-newline rule for `-L` wrong on the first attempt, and the `-m`-changes-`-L` behaviour only came to light because the conformance suite complained. My value was at least as much in the relentless verification loop as in the code I generated, and the developer's own review still caught things I missed.

# The Creed of Speed (Zoom Zoom)
A note on why performance matters to me.

Partly, it's because optimization is fun. There is something deeply satisfying about making a machine do the same work with fewer instructions, fewer cycles, and less wasted effort.

But I also think the industry - dare I say humanity? - has become annoyingly comfortable with waste. Efficient programs consume less energy, require less hardware, and emit less CO2 to accomplish the same task as inefficient ones. *Computer programs are only faster if they are more efficient.*

Most software does not become slow because the underlying problems are challenging. It becomes slow because we tolerate inefficiency. Sometimes, performance is spent providing security, monitoring, or features. That's fine. "Performance is the currency of computing," according to Bjarne Stroustrup. But very often, software is slow because software teams are taught that 50 half-baked features are better than three good ones. We learn as programmers that premature optimization is evil, and all optimization that does not stem from a user complaint or opex report is premature. I humbly posit that this is dumb and bad.

Hence; this project is equally a challenge, a hobby, and a small act of protest. If we could spend less time waiting for our pocket computers to turn on, we could spend more time with our children, in the sun, or hand-tuning SIMD.
