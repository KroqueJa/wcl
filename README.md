# QWC

Have you used `wc` before? I have - it's great! But it's not as fast as it could be. Hence, some people wrote [uutils](https://github.com/uutils/coreutils) containing `uu-wc`. Which is even greater! But it's not as fast as it could be. So I wrote `qwc`. `qwc` is as fast as it can be.

Why is it faster than the above mentioned byte counters? Many reasons, here are some:
1. It doesn't hold itself to as rigorous a standard of correctness with regards to exotic locales. `qwc` specializes in the UTF-8 and C locales, where 99.99% of modern use cases live.
2. It employs a novel SIMD-based, threaded architecture that is both faster and more cache-efficient than the traditional scalar approach. It is probably not faster on a single-core machine.

**Update 2026-06-24**: `qwc` now validates rectangularity of csv files! For more info, do:
```bash
qwc --validate-csv --help
```
It's about 5-12x faster than `zsv check`.

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

# Benchmarks

qwc is really, [really](benchmarks/latest.md) fast.

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
