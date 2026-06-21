# QWC

Have you used `wc` before? I have - it's great! But it's not as fast as it could be. Hence, some people wrote [uutils](https://github.com/uutils/coreutils) containing `uu-wc`. Which is even greater! But it's not as fast as it could be. So I wrote `qwc`. `qwc` is as fast as it can be.

Why is it faster than the above mentioned byte counters? Many reasons:
1. It doesn't hold itself to as rigorous a standard of correctness with regards to exotic locales. `qwc` specializes in the UTF-8 and C locales, where 99.99% of modern use cases live.
2. It employs a novel SIMD-based, threaded architecture that is both faster and more cache-efficient than the traditional scalar approach. It is not faster on a single-core machine.
3. It is written by someone who constantly tinkers to make it faster instead of shipping more useful features.

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
These benchmarks are somewhat arbitrary; they run on a non-dedicated machine and can be jittery. There exists a github action to inspect partial results independently while developing, but that runs on virtual cores and is very unreliable. Hence, these benchmarks are published as an unreliable snapshot of the current state.

The benchmarks run on different corpora, here sorted by subheader. The corpora are generated, and designed to be spread among general, best-case (for some flags) and maximally adversarial (for the same flags). The most interesting column, in my view, is the "vs uu-wc" column - if you care about byte counting speed at all, that is the closest competitor. It should be mentioned once more that `qwc` is not "simply better" than `uu-wc` for these use cases - it utilizes threads more aggressively, but _is allowed to do so_ (probably) because of concessions in the area of locale correctness. `qwc` is currently guaranteed to be correct for the C and UTF-8 locales only which allows for more aggressive optimizations in the code.

Each table runs every flag combination twice: once under `LC_ALL=C`, once under `LC_ALL=C.UTF-8`. `qwc` adopts the locale once at startup so the two halves exercise different scan kernels. The `v0.2.1` column is qwc's previous tagged release; comparing against it shows the change since shipping the 3-byte UTF-8 vectorization (visible mainly on `C.UTF-8 / -w` and on the CJK corpus). Host CPU count reflects SMT-off bench prep.

## Single large file (256 MiB)
host: 6 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | v0.2.1 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.2.1 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 42.6 | 43.8 | 1171.4 | 679.5 | 1.03x | 27.51x | 15.96x |
| C       / -l | 11.4 | 11.4 | 28.8 | 30.6 | 1.00x | 2.53x | 2.68x |
| C       / -w | 40.5 | 42.1 | 1222.6 | 680.4 | 1.04x | 30.19x | 16.81x |
| C       / -c | 0.9 | 0.9 | 1.7 | 0.7 | 0.99x | 1.95x | 0.80x |
| C       / -m | 0.9 | 0.9 | 28.5 | 0.7 | 1.00x | 32.33x | 0.79x |
| C       / -L | 37.7 | 37.6 | 1202.0 | 679.4 | 1.00x | 31.90x | 18.03x |
| C       / -L -m | 37.7 | 37.5 | 1202.7 | 680.9 | 1.00x | 31.93x | 18.08x |
| C       / -l -w | 41.7 | 43.5 | 1172.7 | 680.0 | 1.04x | 28.11x | 16.30x |
| C       / -l -L | 38.8 | 38.8 | 894.0 | 680.8 | 1.00x | 23.02x | 17.53x |
| C       / -l -w -m | 41.7 | 43.4 | 1294.4 | 679.7 | 1.04x | 31.02x | 16.29x |
| C.UTF-8 / (default) | 54.0 | 72.0 | 1172.0 | 1273.3 | 1.33x | 21.71x | 23.59x |
| C.UTF-8 / -l | 11.4 | 11.4 | 28.5 | 32.2 | 1.00x | 2.49x | 2.81x |
| C.UTF-8 / -w | 52.6 | 70.6 | 1222.0 | 1269.9 | 1.34x | 23.22x | 24.13x |
| C.UTF-8 / -c | 0.9 | 0.9 | 1.7 | 2.0 | 1.01x | 1.97x | 2.33x |
| C.UTF-8 / -m | 12.7 | 12.7 | 28.3 | 1269.3 | 1.00x | 2.23x | 99.81x |
| C.UTF-8 / -L | 37.5 | 37.5 | 1201.5 | 1303.4 | 1.00x | 32.01x | 34.72x |
| C.UTF-8 / -L -m | 52.3 | 52.3 | 1203.3 | 1298.7 | 1.00x | 23.01x | 24.83x |
| C.UTF-8 / -l -w | 53.9 | 71.9 | 1172.0 | 1270.9 | 1.34x | 21.75x | 23.59x |
| C.UTF-8 / -l -L | 38.9 | 38.8 | 895.3 | 1308.2 | 1.00x | 23.02x | 33.64x |
| C.UTF-8 / -l -w -m | 55.1 | 73.0 | 1294.2 | 1269.3 | 1.33x | 23.51x | 23.06x |

## Long lines (256 MiB)
host: 6 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | v0.2.1 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.2.1 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 41.7 | 43.2 | 1168.1 | 654.8 | 1.04x | 28.01x | 15.70x |
| C       / -l | 11.4 | 11.3 | 28.4 | 30.5 | 1.00x | 2.49x | 2.68x |
| C       / -w | 40.4 | 42.1 | 1217.6 | 653.3 | 1.04x | 30.13x | 16.17x |
| C       / -c | 0.9 | 0.9 | 1.7 | 0.7 | 0.99x | 1.96x | 0.80x |
| C       / -m | 0.9 | 0.9 | 28.2 | 0.7 | 0.98x | 31.82x | 0.78x |
| C       / -L | 12.6 | 12.7 | 1174.6 | 653.7 | 1.00x | 92.88x | 51.69x |
| C       / -L -m | 12.7 | 12.7 | 1179.0 | 654.6 | 1.00x | 92.93x | 51.59x |
| C       / -l -w | 41.6 | 43.4 | 1168.3 | 654.2 | 1.04x | 28.11x | 15.74x |
| C       / -l -L | 13.7 | 13.8 | 869.2 | 652.9 | 1.00x | 63.31x | 47.55x |
| C       / -l -w -m | 41.7 | 43.3 | 1289.6 | 655.1 | 1.04x | 30.95x | 15.72x |
| C.UTF-8 / (default) | 53.7 | 71.9 | 1166.6 | 1241.3 | 1.34x | 21.70x | 23.09x |
| C.UTF-8 / -l | 11.4 | 11.4 | 28.1 | 31.6 | 1.00x | 2.47x | 2.78x |
| C.UTF-8 / -w | 52.5 | 70.6 | 1218.2 | 1241.5 | 1.34x | 23.21x | 23.65x |
| C.UTF-8 / -c | 0.9 | 0.9 | 1.7 | 2.0 | 0.99x | 1.93x | 2.28x |
| C.UTF-8 / -m | 12.7 | 12.7 | 28.1 | 1238.0 | 1.00x | 2.22x | 97.55x |
| C.UTF-8 / -L | 12.6 | 12.7 | 1176.5 | 1269.1 | 1.00x | 93.31x | 100.66x |
| C.UTF-8 / -L -m | 13.8 | 13.9 | 1178.9 | 1268.6 | 1.00x | 85.29x | 91.78x |
| C.UTF-8 / -l -w | 55.2 | 73.2 | 1167.2 | 1238.7 | 1.33x | 21.15x | 22.44x |
| C.UTF-8 / -l -L | 13.8 | 13.8 | 871.6 | 1266.6 | 1.00x | 63.16x | 91.78x |
| C.UTF-8 / -l -w -m | 54.9 | 73.0 | 1288.7 | 1239.6 | 1.33x | 23.46x | 22.57x |

## Many small files (256 MiB total)
host: 6 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | v0.2.1 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.2.1 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 37.4 | 38.9 | 1185.2 | 694.6 | 1.04x | 31.72x | 18.59x |
| C       / -l | 12.6 | 12.6 | 43.9 | 44.4 | 1.00x | 3.49x | 3.53x |
| C       / -w | 36.0 | 37.6 | 1235.0 | 693.8 | 1.04x | 34.26x | 19.25x |
| C       / -c | 3.2 | 3.2 | 9.9 | 8.7 | 1.00x | 3.08x | 2.71x |
| C       / -m | 3.2 | 3.2 | 43.8 | 8.7 | 0.99x | 13.48x | 2.69x |
| C       / -L | 34.6 | 34.6 | 1219.5 | 697.1 | 1.00x | 35.28x | 20.17x |
| C       / -L -m | 34.7 | 34.7 | 1215.6 | 694.3 | 1.00x | 35.02x | 20.00x |
| C       / -l -w | 37.2 | 38.8 | 1184.1 | 693.9 | 1.04x | 31.82x | 18.65x |
| C       / -l -L | 35.7 | 35.9 | 908.2 | 694.2 | 1.00x | 25.42x | 19.43x |
| C       / -l -w -m | 37.4 | 38.9 | 1312.8 | 697.4 | 1.04x | 35.14x | 18.67x |
| C.UTF-8 / (default) | 48.6 | 63.8 | 1188.7 | 1285.4 | 1.31x | 24.47x | 26.46x |
| C.UTF-8 / -l | 12.6 | 12.6 | 44.1 | 46.2 | 1.00x | 3.51x | 3.68x |
| C.UTF-8 / -w | 47.3 | 62.6 | 1234.2 | 1288.3 | 1.32x | 26.08x | 27.23x |
| C.UTF-8 / -c | 3.2 | 3.2 | 9.9 | 10.0 | 0.98x | 3.05x | 3.09x |
| C.UTF-8 / -m | 13.8 | 13.8 | 43.9 | 1286.0 | 1.00x | 3.18x | 93.13x |
| C.UTF-8 / -L | 34.6 | 34.5 | 1215.5 | 1313.7 | 1.00x | 35.17x | 38.02x |
| C.UTF-8 / -L -m | 47.3 | 47.2 | 1215.8 | 1316.6 | 1.00x | 25.69x | 27.82x |
| C.UTF-8 / -l -w | 48.5 | 63.7 | 1184.1 | 1287.1 | 1.31x | 24.43x | 26.55x |
| C.UTF-8 / -l -L | 35.9 | 35.7 | 909.1 | 1319.9 | 1.00x | 25.35x | 36.80x |
| C.UTF-8 / -l -w -m | 49.5 | 65.0 | 1307.7 | 1288.4 | 1.31x | 26.42x | 26.03x |

## Mixed shape (256 MiB)
host: 6 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | v0.2.1 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.2.1 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 41.7 | 43.3 | 1173.1 | 680.8 | 1.04x | 28.14x | 16.33x |
| C       / -l | 11.3 | 11.3 | 28.1 | 29.6 | 1.00x | 2.48x | 2.61x |
| C       / -w | 40.5 | 42.0 | 1221.0 | 679.5 | 1.04x | 30.17x | 16.79x |
| C       / -c | 0.9 | 0.9 | 1.7 | 0.7 | 1.00x | 1.96x | 0.81x |
| C       / -m | 0.9 | 0.9 | 28.6 | 0.7 | 0.99x | 32.30x | 0.80x |
| C       / -L | 37.5 | 37.5 | 1204.0 | 680.3 | 1.00x | 32.10x | 18.14x |
| C       / -L -m | 37.5 | 37.5 | 1202.0 | 679.1 | 1.00x | 32.06x | 18.11x |
| C       / -l -w | 41.7 | 43.4 | 1171.2 | 680.1 | 1.04x | 28.09x | 16.31x |
| C       / -l -L | 38.8 | 38.9 | 893.9 | 679.3 | 1.00x | 23.01x | 17.49x |
| C       / -l -w -m | 41.7 | 43.3 | 1294.7 | 680.3 | 1.04x | 31.03x | 16.31x |
| C.UTF-8 / (default) | 53.7 | 71.9 | 1170.8 | 1269.1 | 1.34x | 21.80x | 23.63x |
| C.UTF-8 / -l | 11.3 | 11.4 | 28.0 | 31.5 | 1.00x | 2.47x | 2.78x |
| C.UTF-8 / -w | 52.5 | 70.7 | 1220.5 | 1270.0 | 1.35x | 23.23x | 24.17x |
| C.UTF-8 / -c | 0.9 | 0.9 | 1.7 | 2.0 | 0.99x | 1.95x | 2.28x |
| C.UTF-8 / -m | 12.7 | 12.7 | 28.0 | 1269.4 | 1.00x | 2.20x | 99.68x |
| C.UTF-8 / -L | 37.5 | 37.5 | 1201.9 | 1301.2 | 1.00x | 32.05x | 34.70x |
| C.UTF-8 / -L -m | 52.3 | 52.4 | 1201.8 | 1298.3 | 1.00x | 22.98x | 24.82x |
| C.UTF-8 / -l -w | 53.8 | 71.8 | 1171.0 | 1269.7 | 1.33x | 21.76x | 23.59x |
| C.UTF-8 / -l -L | 38.8 | 38.8 | 893.8 | 1298.1 | 1.00x | 23.01x | 33.42x |
| C.UTF-8 / -l -w -m | 54.9 | 73.1 | 1293.8 | 1269.0 | 1.33x | 23.55x | 23.10x |

## Short lines (256 MiB)
host: 6 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | v0.2.1 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.2.1 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 41.6 | 43.3 | 1179.4 | 703.9 | 1.04x | 28.34x | 16.91x |
| C       / -l | 11.3 | 11.3 | 27.4 | 29.5 | 1.00x | 2.43x | 2.62x |
| C       / -w | 40.5 | 41.9 | 1225.8 | 704.6 | 1.03x | 30.24x | 17.39x |
| C       / -c | 0.9 | 0.9 | 1.7 | 0.7 | 0.99x | 1.93x | 0.79x |
| C       / -m | 0.9 | 0.9 | 27.3 | 0.7 | 1.00x | 31.04x | 0.81x |
| C       / -L | 54.3 | 54.3 | 1220.0 | 702.9 | 1.00x | 22.48x | 12.95x |
| C       / -L -m | 54.5 | 58.6 | 1218.9 | 702.8 | 1.07x | 22.35x | 12.89x |
| C       / -l -w | 41.6 | 43.3 | 1178.9 | 703.0 | 1.04x | 28.32x | 16.89x |
| C       / -l -L | 55.6 | 55.6 | 913.1 | 702.9 | 1.00x | 16.41x | 12.63x |
| C       / -l -w -m | 41.6 | 43.2 | 1298.7 | 702.4 | 1.04x | 31.22x | 16.88x |
| C.UTF-8 / (default) | 53.9 | 71.8 | 1177.2 | 1290.5 | 1.33x | 21.84x | 23.94x |
| C.UTF-8 / -l | 12.2 | 11.4 | 27.5 | 31.0 | 0.93x | 2.26x | 2.54x |
| C.UTF-8 / -w | 52.5 | 70.6 | 1224.6 | 1292.3 | 1.34x | 23.31x | 24.60x |
| C.UTF-8 / -c | 0.9 | 0.9 | 1.8 | 2.0 | 1.00x | 1.99x | 2.30x |
| C.UTF-8 / -m | 12.7 | 12.6 | 27.7 | 1291.8 | 1.00x | 2.19x | 102.10x |
| C.UTF-8 / -L | 56.8 | 56.3 | 1220.2 | 1319.4 | 0.99x | 21.46x | 23.21x |
| C.UTF-8 / -L -m | 78.6 | 77.6 | 1219.3 | 1319.4 | 0.99x | 15.51x | 16.78x |
| C.UTF-8 / -l -w | 54.0 | 72.3 | 1178.2 | 1292.4 | 1.34x | 21.83x | 23.94x |
| C.UTF-8 / -l -L | 56.7 | 56.6 | 911.1 | 1319.9 | 1.00x | 16.07x | 23.28x |
| C.UTF-8 / -l -w -m | 55.1 | 73.9 | 1297.9 | 1289.9 | 1.34x | 23.54x | 23.39x |

## Single line (256 MiB)
host: 6 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | v0.2.1 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.2.1 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 41.6 | 43.2 | 1164.9 | 652.5 | 1.04x | 28.03x | 15.70x |
| C       / -l | 11.2 | 11.2 | 27.7 | 29.7 | 1.00x | 2.47x | 2.64x |
| C       / -w | 40.3 | 41.8 | 1215.6 | 652.8 | 1.04x | 30.16x | 16.19x |
| C       / -c | 0.9 | 0.9 | 1.7 | 0.7 | 1.00x | 1.97x | 0.82x |
| C       / -m | 0.9 | 0.9 | 27.4 | 0.7 | 1.00x | 31.02x | 0.79x |
| C       / -L | 12.2 | 12.3 | 1172.5 | 652.8 | 1.01x | 96.19x | 53.55x |
| C       / -L -m | 12.1 | 12.2 | 1178.0 | 653.8 | 1.00x | 97.23x | 53.96x |
| C       / -l -w | 41.5 | 43.2 | 1165.5 | 653.1 | 1.04x | 28.12x | 15.76x |
| C       / -l -L | 13.3 | 13.3 | 869.7 | 652.7 | 1.00x | 65.48x | 49.15x |
| C       / -l -w -m | 41.6 | 43.2 | 1285.9 | 651.9 | 1.04x | 30.91x | 15.67x |
| C.UTF-8 / (default) | 53.5 | 71.7 | 1166.8 | 1239.6 | 1.34x | 21.80x | 23.16x |
| C.UTF-8 / -l | 11.2 | 11.2 | 27.3 | 30.4 | 1.00x | 2.43x | 2.70x |
| C.UTF-8 / -w | 52.3 | 70.4 | 1217.7 | 1237.7 | 1.35x | 23.27x | 23.65x |
| C.UTF-8 / -c | 0.9 | 0.9 | 1.7 | 2.0 | 0.99x | 1.94x | 2.28x |
| C.UTF-8 / -m | 12.6 | 12.6 | 27.3 | 1236.3 | 1.00x | 2.17x | 98.49x |
| C.UTF-8 / -L | 12.2 | 12.2 | 1174.9 | 1267.7 | 1.00x | 96.40x | 104.01x |
| C.UTF-8 / -L -m | 13.3 | 13.3 | 1178.0 | 1267.7 | 1.00x | 88.69x | 95.44x |
| C.UTF-8 / -l -w | 53.6 | 71.7 | 1167.2 | 1238.8 | 1.34x | 21.78x | 23.12x |
| C.UTF-8 / -l -L | 13.3 | 13.3 | 871.1 | 1265.3 | 1.00x | 65.30x | 94.85x |
| C.UTF-8 / -l -w -m | 54.8 | 72.9 | 1288.3 | 1238.6 | 1.33x | 23.52x | 22.61x |

## CJK 3-byte UTF-8 short lines (256 MiB)
host: 6 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | v0.2.1 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.2.1 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 52.2 | 54.1 | 1050.1 | 558.2 | 1.04x | 20.12x | 10.69x |
| C       / -l | 11.1 | 11.1 | 27.6 | 29.5 | 1.00x | 2.50x | 2.67x |
| C       / -w | 51.0 | 52.6 | 1068.8 | 561.5 | 1.03x | 20.98x | 11.02x |
| C       / -c | 0.9 | 0.9 | 1.7 | 0.7 | 1.00x | 1.97x | 0.81x |
| C       / -m | 0.9 | 0.9 | 27.5 | 0.7 | 1.01x | 31.36x | 0.81x |
| C       / -L | 42.7 | 42.7 | 1238.9 | 556.6 | 1.00x | 29.04x | 13.04x |
| C       / -L -m | 42.7 | 42.7 | 1272.1 | 558.8 | 1.00x | 29.81x | 13.10x |
| C       / -l -w | 52.4 | 54.0 | 1049.7 | 561.4 | 1.03x | 20.01x | 10.70x |
| C       / -l -L | 44.1 | 44.0 | 1101.4 | 557.4 | 1.00x | 25.00x | 12.65x |
| C       / -l -w -m | 52.2 | 54.1 | 1079.0 | 559.0 | 1.04x | 20.67x | 10.71x |
| C.UTF-8 / (default) | 75.1 | 221.2 | 1048.0 | 1942.9 | 2.95x | 13.96x | 25.89x |
| C.UTF-8 / -l | 11.1 | 11.1 | 27.5 | 31.2 | 1.00x | 2.48x | 2.82x |
| C.UTF-8 / -w | 73.6 | 219.8 | 1068.8 | 1938.6 | 2.99x | 14.52x | 26.33x |
| C.UTF-8 / -c | 0.9 | 0.9 | 1.7 | 2.0 | 1.00x | 1.98x | 2.30x |
| C.UTF-8 / -m | 12.4 | 12.4 | 27.3 | 1938.3 | 1.00x | 2.21x | 156.43x |
| C.UTF-8 / -L | 42.7 | 42.7 | 1239.2 | 2261.0 | 1.00x | 29.01x | 52.93x |
| C.UTF-8 / -L -m | 68.4 | 68.4 | 1271.7 | 2260.8 | 1.00x | 18.58x | 33.04x |
| C.UTF-8 / -l -w | 74.9 | 220.9 | 1048.2 | 1939.0 | 2.95x | 13.99x | 25.88x |
| C.UTF-8 / -l -L | 44.1 | 44.0 | 1101.1 | 2264.0 | 1.00x | 25.00x | 51.40x |
| C.UTF-8 / -l -w -m | 76.2 | 222.4 | 1080.9 | 1940.7 | 2.92x | 14.19x | 25.48x |

## Cyrillic 2-byte UTF-8 short lines (256 MiB)
host: 6 logical CPUs (qwc threads to this) · Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz · 31.3 GiB RAM

| flag | qwc (ms) | v0.2.1 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.2.1 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 49.8 | 51.1 | 1177.4 | 609.4 | 1.03x | 23.65x | 12.24x |
| C       / -l | 10.9 | 10.8 | 26.3 | 28.7 | 1.00x | 2.42x | 2.64x |
| C       / -w | 48.8 | 49.8 | 1201.0 | 609.5 | 1.02x | 24.63x | 12.50x |
| C       / -c | 0.9 | 0.9 | 1.7 | 0.7 | 1.01x | 1.98x | 0.80x |
| C       / -m | 0.9 | 0.9 | 26.4 | 0.7 | 1.00x | 30.14x | 0.81x |
| C       / -L | 41.3 | 41.4 | 1387.9 | 610.3 | 1.00x | 33.56x | 14.76x |
| C       / -L -m | 41.4 | 41.5 | 1394.0 | 610.2 | 1.00x | 33.70x | 14.75x |
| C       / -l -w | 49.9 | 51.1 | 1177.2 | 608.8 | 1.03x | 23.61x | 12.21x |
| C       / -l -L | 42.7 | 42.7 | 1215.5 | 608.6 | 1.00x | 28.47x | 14.25x |
| C       / -l -w -m | 49.9 | 51.1 | 1259.0 | 610.1 | 1.02x | 25.22x | 12.22x |
| C.UTF-8 / (default) | 65.3 | 67.5 | 1177.1 | 2584.2 | 1.03x | 18.03x | 39.59x |
| C.UTF-8 / -l | 10.9 | 10.9 | 26.5 | 30.1 | 1.00x | 2.44x | 2.77x |
| C.UTF-8 / -w | 64.0 | 66.2 | 1202.1 | 2584.4 | 1.03x | 18.78x | 40.38x |
| C.UTF-8 / -c | 0.9 | 0.9 | 1.7 | 2.0 | 0.97x | 1.97x | 2.30x |
| C.UTF-8 / -m | 12.1 | 12.1 | 26.7 | 2589.1 | 1.00x | 2.22x | 214.62x |
| C.UTF-8 / -L | 41.3 | 41.3 | 1385.5 | 3097.6 | 1.00x | 33.56x | 75.02x |
| C.UTF-8 / -L -m | 66.5 | 66.7 | 1392.5 | 3095.2 | 1.00x | 20.95x | 46.57x |
| C.UTF-8 / -l -w | 65.3 | 67.5 | 1176.7 | 2586.2 | 1.03x | 18.03x | 39.64x |
| C.UTF-8 / -l -L | 42.7 | 42.7 | 1217.9 | 3097.9 | 1.00x | 28.55x | 72.62x |
| C.UTF-8 / -l -w -m | 66.8 | 68.6 | 1257.3 | 2584.5 | 1.03x | 18.83x | 38.72x |

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
