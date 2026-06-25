# Benchmarks
These benchmarks are somewhat arbitrary; they run on a non-dedicated machine and can be jittery. There exists a github action to inspect partial results independently while developing, but that runs on virtual cores and is very unreliable. Hence, these benchmarks are published as an unreliable snapshot of the current state.

The benchmarks run on different corpora, here sorted by subheader. The corpora are generated, and designed to be spread among general, best-case (for some flags) and maximally adversarial (for the same flags). The most interesting column, in my view, is the "vs uu-wc" column - if you care about byte counting speed at all, that is the closest competitor. It should be mentioned once more that `qwc` is not "simply better" than `uu-wc` for these use cases - it utilizes threads more aggressively, but _is allowed to do so_ (probably) because of concessions in the area of locale correctness. `qwc` is currently guaranteed to be correct for the C and UTF-8 locales only which allows for more aggressive optimizations in the code.

Each table runs every flag combination twice: once under `LC_ALL=C`, once under `LC_ALL=C.UTF-8`. `qwc` adopts the locale once at startup so the two halves exercise different scan kernels. This snapshot is from the **Apple M4 Pro** (arm64 / NEON) build; the `v0.3.0` column is qwc's previous tagged release and the `qwc` column is the unreleased `main` (`v0.3.0-20-g41594ae`), so the `vs v0.3.0` column shows the net change since shipping — chiefly the NEON Phase 2 word kernel, visible on the C.UTF-8 word-counting rows. Unlike the Linux numbers, macOS has no bench-prep quiescing step (`scripts/bench/prep.sh` is Linux-only and is skipped here), so expect more run-to-run jitter; the host CPU count is all logical cores (Apple Silicon has no SMT).

## Single large file (256 MiB)
host: 14 logical CPUs (qwc threads to this) · Apple M4 Pro · 48.0 GiB RAM
| flag | qwc (ms) | v0.3.0 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.3.0 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 24.5 | 25.3 | 499.1 | 410.9 | 1.03x | 20.41x | 16.80x |
| C       / -l | 6.2 | 6.1 | 18.4 | 59.8 | 0.99x | 2.97x | 9.62x |
| C       / -w | 23.9 | 24.8 | 434.2 | 417.8 | 1.04x | 18.14x | 17.46x |
| C       / -c | 1.3 | 1.3 | 2.7 | 1.2 | 0.98x | 2.07x | 0.94x |
| C       / -m | 1.2 | 1.3 | 18.5 | 1.3 | 1.03x | 14.90x | 1.05x |
| C       / -L | 16.8 | 16.8 | 379.0 | 421.8 | 1.00x | 22.57x | 25.12x |
| C       / -L -m | 16.9 | 16.8 | 320.6 | 414.7 | 1.00x | 18.98x | 24.55x |
| C       / -l -w | 24.6 | 25.4 | 508.6 | 423.7 | 1.03x | 20.70x | 17.25x |
| C       / -l -L | 17.5 | 17.4 | 337.2 | 424.8 | 1.00x | 19.32x | 24.34x |
| C       / -l -w -m | 24.4 | 25.4 | 516.7 | 399.6 | 1.04x | 21.16x | 16.36x |
| C.UTF-8 / (default) | 27.7 | 33.2 | 533.4 | 604.4 | 1.20x | 19.23x | 21.79x |
| C.UTF-8 / -l | 6.1 | 6.1 | 18.7 | 61.2 | 1.00x | 3.04x | 9.98x |
| C.UTF-8 / -w | 27.3 | 32.7 | 433.1 | 584.7 | 1.20x | 15.88x | 21.44x |
| C.UTF-8 / -c | 1.3 | 1.2 | 2.5 | 1.2 | 0.95x | 2.00x | 0.97x |
| C.UTF-8 / -m | 6.1 | 6.1 | 18.0 | 577.6 | 1.00x | 2.96x | 95.23x |
| C.UTF-8 / -L | 16.9 | 16.9 | 360.2 | 649.4 | 1.00x | 21.31x | 38.43x |
| C.UTF-8 / -L -m | 20.8 | 20.9 | 311.5 | 643.1 | 1.00x | 14.94x | 30.85x |
| C.UTF-8 / -l -w | 27.7 | 33.2 | 497.5 | 594.9 | 1.20x | 17.95x | 21.46x |
| C.UTF-8 / -l -L | 17.6 | 17.6 | 333.3 | 672.0 | 1.00x | 18.93x | 38.16x |
| C.UTF-8 / -l -w -m | 28.4 | 33.9 | 519.3 | 602.2 | 1.19x | 18.29x | 21.20x |

## Long lines (256 MiB)
host: 14 logical CPUs (qwc threads to this) · Apple M4 Pro · 48.0 GiB RAM
| flag | qwc (ms) | v0.3.0 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.3.0 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 24.5 | 25.3 | 532.0 | 425.1 | 1.03x | 21.75x | 17.38x |
| C       / -l | 6.1 | 6.0 | 18.8 | 25.5 | 0.99x | 3.08x | 4.18x |
| C       / -w | 23.9 | 24.8 | 442.0 | 426.0 | 1.04x | 18.53x | 17.86x |
| C       / -c | 1.3 | 1.3 | 2.7 | 1.2 | 0.96x | 2.05x | 0.95x |
| C       / -m | 1.2 | 1.2 | 18.7 | 1.3 | 0.99x | 14.94x | 1.03x |
| C       / -L | 7.4 | 7.4 | 371.4 | 425.7 | 1.00x | 49.91x | 57.21x |
| C       / -L -m | 7.4 | 7.6 | 311.6 | 426.0 | 1.02x | 42.13x | 57.59x |
| C       / -l -w | 24.5 | 25.3 | 533.1 | 427.0 | 1.03x | 21.80x | 17.46x |
| C       / -l -L | 8.0 | 8.0 | 323.2 | 411.4 | 1.00x | 40.36x | 51.37x |
| C       / -l -w -m | 24.4 | 25.3 | 512.3 | 398.0 | 1.04x | 21.04x | 16.34x |
| C.UTF-8 / (default) | 27.6 | 33.1 | 494.0 | 577.4 | 1.20x | 17.90x | 20.92x |
| C.UTF-8 / -l | 6.1 | 6.0 | 18.0 | 24.2 | 0.98x | 2.94x | 3.95x |
| C.UTF-8 / -w | 27.2 | 32.6 | 409.9 | 583.3 | 1.20x | 15.06x | 21.43x |
| C.UTF-8 / -c | 1.3 | 1.3 | 2.8 | 1.3 | 0.99x | 2.14x | 1.02x |
| C.UTF-8 / -m | 6.1 | 6.1 | 18.6 | 599.8 | 1.00x | 3.04x | 97.78x |
| C.UTF-8 / -L | 7.4 | 7.4 | 360.2 | 664.6 | 1.01x | 48.68x | 89.82x |
| C.UTF-8 / -L -m | 7.7 | 7.7 | 310.6 | 655.4 | 1.00x | 40.39x | 85.23x |
| C.UTF-8 / -l -w | 27.7 | 33.2 | 494.2 | 593.1 | 1.20x | 17.87x | 21.45x |
| C.UTF-8 / -l -L | 8.0 | 8.0 | 309.4 | 646.1 | 1.00x | 38.65x | 80.71x |
| C.UTF-8 / -l -w -m | 28.2 | 33.7 | 511.7 | 575.7 | 1.20x | 18.14x | 20.41x |

## Many small files (256 MiB total)
host: 14 logical CPUs (qwc threads to this) · Apple M4 Pro · 48.0 GiB RAM
| flag | qwc (ms) | v0.3.0 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.3.0 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 83.5 | 84.0 | 1141.7 | 935.0 | 1.01x | 13.67x | 11.20x |
| C       / -l | 79.0 | 79.4 | 126.9 | 270.6 | 1.01x | 1.61x | 3.43x |
| C       / -w | 82.7 | 82.7 | 992.7 | 941.5 | 1.00x | 12.01x | 11.39x |
| C       / -c | 81.2 | 81.6 | 85.0 | 81.3 | 1.01x | 1.05x | 1.00x |
| C       / -m | 81.7 | 81.7 | 125.2 | 81.3 | 1.00x | 1.53x | 1.00x |
| C       / -L | 80.3 | 80.9 | 880.9 | 972.3 | 1.01x | 10.97x | 12.11x |
| C       / -L -m | 81.7 | 81.1 | 766.8 | 951.6 | 0.99x | 9.39x | 11.65x |
| C       / -l -w | 85.4 | 85.5 | 1141.4 | 940.9 | 1.00x | 13.36x | 11.01x |
| C       / -l -L | 79.0 | 80.1 | 764.1 | 933.2 | 1.01x | 9.67x | 11.81x |
| C       / -l -w -m | 86.4 | 88.2 | 1155.8 | 935.5 | 1.02x | 13.38x | 10.83x |
| C.UTF-8 / (default) | 85.7 | 89.4 | 1140.4 | 1309.2 | 1.04x | 13.30x | 15.27x |
| C.UTF-8 / -l | 79.0 | 86.7 | 127.3 | 271.0 | 1.10x | 1.61x | 3.43x |
| C.UTF-8 / -w | 85.4 | 88.8 | 965.3 | 1312.8 | 1.04x | 11.30x | 15.36x |
| C.UTF-8 / -c | 80.4 | 81.6 | 83.7 | 81.4 | 1.01x | 1.04x | 1.01x |
| C.UTF-8 / -m | 80.1 | 79.9 | 127.0 | 1321.5 | 1.00x | 1.59x | 16.51x |
| C.UTF-8 / -L | 79.1 | 79.5 | 866.9 | 1452.0 | 1.00x | 10.96x | 18.36x |
| C.UTF-8 / -L -m | 80.4 | 83.8 | 762.6 | 1450.4 | 1.04x | 9.49x | 18.05x |
| C.UTF-8 / -l -w | 85.5 | 89.5 | 1139.7 | 1306.4 | 1.05x | 13.33x | 15.28x |
| C.UTF-8 / -l -L | 78.8 | 79.5 | 764.0 | 1445.5 | 1.01x | 9.69x | 18.33x |
| C.UTF-8 / -l -w -m | 86.8 | 90.2 | 1191.2 | 1325.8 | 1.04x | 13.72x | 15.27x |

## Mixed shape (256 MiB)
host: 14 logical CPUs (qwc threads to this) · Apple M4 Pro · 48.0 GiB RAM
| flag | qwc (ms) | v0.3.0 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.3.0 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 24.5 | 25.3 | 513.4 | 399.1 | 1.04x | 20.99x | 16.32x |
| C       / -l | 6.1 | 6.0 | 18.2 | 57.2 | 0.99x | 2.99x | 9.38x |
| C       / -w | 23.9 | 24.8 | 417.9 | 406.5 | 1.04x | 17.48x | 17.00x |
| C       / -c | 1.2 | 1.2 | 2.7 | 1.2 | 0.96x | 2.17x | 0.97x |
| C       / -m | 1.2 | 1.2 | 18.0 | 1.2 | 0.98x | 15.06x | 1.01x |
| C       / -L | 16.8 | 16.8 | 360.5 | 397.6 | 1.00x | 21.43x | 23.64x |
| C       / -L -m | 16.9 | 16.8 | 319.9 | 422.7 | 0.99x | 18.91x | 24.98x |
| C       / -l -w | 24.4 | 25.3 | 523.5 | 417.1 | 1.04x | 21.42x | 17.07x |
| C       / -l -L | 17.4 | 17.5 | 338.6 | 424.7 | 1.00x | 19.41x | 24.35x |
| C       / -l -w -m | 24.5 | 25.4 | 555.6 | 423.7 | 1.04x | 22.67x | 17.29x |
| C.UTF-8 / (default) | 27.8 | 33.3 | 495.5 | 582.8 | 1.20x | 17.84x | 20.98x |
| C.UTF-8 / -l | 6.1 | 6.1 | 18.2 | 57.3 | 0.99x | 2.95x | 9.31x |
| C.UTF-8 / -w | 27.2 | 32.7 | 425.7 | 606.0 | 1.20x | 15.64x | 22.26x |
| C.UTF-8 / -c | 1.4 | 1.3 | 2.8 | 1.3 | 0.98x | 2.07x | 0.95x |
| C.UTF-8 / -m | 6.2 | 6.3 | 18.8 | 590.8 | 1.01x | 3.04x | 95.35x |
| C.UTF-8 / -L | 16.8 | 16.8 | 362.3 | 656.1 | 1.00x | 21.51x | 38.95x |
| C.UTF-8 / -L -m | 20.9 | 21.0 | 314.8 | 648.9 | 1.01x | 15.09x | 31.11x |
| C.UTF-8 / -l -w | 27.8 | 33.1 | 498.7 | 582.3 | 1.19x | 17.91x | 20.91x |
| C.UTF-8 / -l -L | 17.4 | 17.4 | 322.7 | 648.1 | 1.00x | 18.53x | 37.22x |
| C.UTF-8 / -l -w -m | 28.3 | 33.9 | 516.2 | 578.0 | 1.20x | 18.22x | 20.40x |

## Short lines (256 MiB)
host: 14 logical CPUs (qwc threads to this) · Apple M4 Pro · 48.0 GiB RAM
| flag | qwc (ms) | v0.3.0 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.3.0 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 24.3 | 25.4 | 497.6 | 396.3 | 1.04x | 20.43x | 16.27x |
| C       / -l | 6.1 | 6.1 | 18.1 | 78.7 | 0.99x | 2.96x | 12.87x |
| C       / -w | 23.8 | 24.7 | 412.8 | 415.0 | 1.04x | 17.36x | 17.45x |
| C       / -c | 1.3 | 1.3 | 2.8 | 1.3 | 0.97x | 2.11x | 0.96x |
| C       / -m | 1.3 | 1.3 | 18.6 | 1.3 | 1.00x | 14.61x | 1.01x |
| C       / -L | 22.0 | 22.1 | 377.4 | 425.2 | 1.00x | 17.13x | 19.30x |
| C       / -L -m | 22.2 | 22.2 | 326.8 | 400.7 | 1.00x | 14.72x | 18.05x |
| C       / -l -w | 24.4 | 25.6 | 498.7 | 399.2 | 1.05x | 20.41x | 16.34x |
| C       / -l -L | 22.7 | 22.8 | 331.1 | 396.0 | 1.00x | 14.58x | 17.44x |
| C       / -l -w -m | 24.4 | 25.3 | 515.8 | 397.5 | 1.04x | 21.12x | 16.28x |
| C.UTF-8 / (default) | 27.8 | 33.2 | 497.5 | 580.5 | 1.19x | 17.89x | 20.87x |
| C.UTF-8 / -l | 6.1 | 6.1 | 18.1 | 78.7 | 1.00x | 2.98x | 13.00x |
| C.UTF-8 / -w | 27.2 | 32.7 | 412.8 | 601.2 | 1.20x | 15.17x | 22.09x |
| C.UTF-8 / -c | 1.3 | 1.3 | 2.9 | 1.4 | 0.99x | 2.19x | 1.02x |
| C.UTF-8 / -m | 6.2 | 6.3 | 19.0 | 605.8 | 1.01x | 3.08x | 98.05x |
| C.UTF-8 / -L | 22.1 | 22.1 | 381.9 | 653.3 | 1.00x | 17.25x | 29.51x |
| C.UTF-8 / -L -m | 28.9 | 29.1 | 321.6 | 654.7 | 1.01x | 11.12x | 22.63x |
| C.UTF-8 / -l -w | 27.9 | 33.3 | 498.3 | 581.3 | 1.19x | 17.83x | 20.80x |
| C.UTF-8 / -l -L | 22.7 | 22.7 | 326.0 | 650.6 | 1.00x | 14.38x | 28.69x |
| C.UTF-8 / -l -w -m | 28.4 | 33.8 | 517.5 | 586.4 | 1.19x | 18.23x | 20.66x |

## Single line (256 MiB)
host: 14 logical CPUs (qwc threads to this) · Apple M4 Pro · 48.0 GiB RAM
| flag | qwc (ms) | v0.3.0 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.3.0 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 24.5 | 25.3 | 492.9 | 417.3 | 1.03x | 20.15x | 17.06x |
| C       / -l | 6.2 | 6.1 | 18.8 | 23.8 | 0.99x | 3.05x | 3.86x |
| C       / -w | 23.9 | 24.8 | 409.9 | 401.3 | 1.04x | 17.14x | 16.78x |
| C       / -c | 1.2 | 1.2 | 2.6 | 1.2 | 0.96x | 2.06x | 0.94x |
| C       / -m | 1.2 | 1.2 | 18.1 | 1.2 | 1.00x | 15.18x | 1.04x |
| C       / -L | 7.2 | 7.1 | 351.6 | 397.2 | 0.99x | 48.90x | 55.24x |
| C       / -L -m | 7.2 | 7.2 | 299.6 | 414.1 | 1.01x | 41.66x | 57.58x |
| C       / -l -w | 24.5 | 25.4 | 531.7 | 426.5 | 1.04x | 21.73x | 17.43x |
| C       / -l -L | 7.9 | 7.9 | 323.1 | 426.9 | 1.00x | 40.97x | 54.13x |
| C       / -l -w -m | 24.5 | 25.3 | 547.1 | 429.8 | 1.03x | 22.34x | 17.55x |
| C.UTF-8 / (default) | 27.8 | 33.3 | 534.2 | 603.8 | 1.20x | 19.20x | 21.70x |
| C.UTF-8 / -l | 6.3 | 6.2 | 18.9 | 24.2 | 0.99x | 3.02x | 3.86x |
| C.UTF-8 / -w | 27.2 | 32.7 | 426.7 | 583.9 | 1.20x | 15.68x | 21.46x |
| C.UTF-8 / -c | 1.3 | 1.2 | 2.6 | 1.2 | 0.94x | 1.97x | 0.93x |
| C.UTF-8 / -m | 6.1 | 6.1 | 18.1 | 577.4 | 1.00x | 2.96x | 94.46x |
| C.UTF-8 / -L | 7.2 | 7.2 | 358.8 | 668.7 | 1.00x | 49.88x | 92.95x |
| C.UTF-8 / -L -m | 7.5 | 7.4 | 305.0 | 650.7 | 0.98x | 40.74x | 86.92x |
| C.UTF-8 / -l -w | 27.8 | 33.3 | 495.5 | 589.7 | 1.20x | 17.81x | 21.20x |
| C.UTF-8 / -l -L | 7.9 | 7.9 | 321.3 | 670.4 | 1.01x | 40.91x | 85.35x |
| C.UTF-8 / -l -w -m | 28.5 | 34.0 | 522.3 | 582.2 | 1.19x | 18.34x | 20.44x |

## CJK 3-byte UTF-8 short lines (256 MiB)
host: 14 logical CPUs (qwc threads to this) · Apple M4 Pro · 48.0 GiB RAM
| flag | qwc (ms) | v0.3.0 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.3.0 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 26.1 | 27.9 | 507.2 | 412.9 | 1.07x | 19.42x | 15.81x |
| C       / -l | 6.1 | 6.1 | 18.4 | 80.6 | 1.00x | 3.00x | 13.19x |
| C       / -w | 25.5 | 27.2 | 422.3 | 427.9 | 1.07x | 16.56x | 16.78x |
| C       / -c | 1.4 | 1.3 | 2.8 | 1.3 | 0.95x | 2.07x | 0.96x |
| C       / -m | 1.3 | 1.3 | 19.2 | 1.3 | 1.00x | 14.89x | 1.02x |
| C       / -L | 22.7 | 22.7 | 397.4 | 439.7 | 1.00x | 17.52x | 19.39x |
| C       / -L -m | 22.6 | 22.7 | 335.0 | 413.7 | 1.00x | 14.83x | 18.31x |
| C       / -l -w | 26.2 | 27.8 | 520.6 | 414.7 | 1.06x | 19.87x | 15.83x |
| C       / -l -L | 23.1 | 23.2 | 342.8 | 416.8 | 1.00x | 14.82x | 18.02x |
| C       / -l -w -m | 26.1 | 27.8 | 571.0 | 439.7 | 1.07x | 21.87x | 16.84x |
| C.UTF-8 / (default) | 27.8 | 28.1 | 551.9 | 663.2 | 1.01x | 19.84x | 23.84x |
| C.UTF-8 / -l | 6.2 | 6.2 | 18.5 | 81.4 | 1.02x | 3.01x | 13.24x |
| C.UTF-8 / -w | 27.5 | 27.5 | 440.2 | 664.1 | 1.00x | 16.01x | 24.16x |
| C.UTF-8 / -c | 1.3 | 1.3 | 2.8 | 1.3 | 0.96x | 2.08x | 1.00x |
| C.UTF-8 / -m | 6.2 | 6.2 | 19.2 | 642.0 | 1.00x | 3.10x | 103.47x |
| C.UTF-8 / -L | 22.6 | 22.6 | 397.2 | 787.2 | 1.00x | 17.59x | 34.85x |
| C.UTF-8 / -L -m | 29.6 | 29.7 | 331.7 | 772.9 | 1.00x | 11.20x | 26.09x |
| C.UTF-8 / -l -w | 27.8 | 28.1 | 516.7 | 660.4 | 1.01x | 18.60x | 23.77x |
| C.UTF-8 / -l -L | 23.2 | 23.1 | 358.0 | 799.9 | 1.00x | 15.42x | 34.46x |
| C.UTF-8 / -l -w -m | 28.4 | 28.6 | 528.8 | 655.3 | 1.01x | 18.63x | 23.09x |

## Cyrillic 2-byte UTF-8 short lines (256 MiB)
host: 14 logical CPUs (qwc threads to this) · Apple M4 Pro · 48.0 GiB RAM
| flag | qwc (ms) | v0.3.0 (ms) | uu-wc (ms) | GNU wc (ms) | vs v0.3.0 | vs uu-wc | vs GNU wc |
|---|---|---|---|---|---|---|---|
| C       / (default) | 36.0 | 28.5 | 539.7 | 378.7 | 0.79x | 14.97x | 10.51x |
| C       / -l | 5.8 | 5.8 | 18.0 | 65.4 | 0.99x | 3.10x | 11.24x |
| C       / -w | 35.4 | 27.9 | 515.4 | 358.8 | 0.79x | 14.57x | 10.14x |
| C       / -c | 1.3 | 1.2 | 2.6 | 1.2 | 0.96x | 2.05x | 0.94x |
| C       / -m | 1.2 | 1.2 | 17.1 | 1.2 | 1.01x | 14.40x | 1.05x |
| C       / -L | 17.4 | 17.5 | 580.1 | 353.6 | 1.00x | 33.32x | 20.31x |
| C       / -L -m | 17.5 | 17.5 | 547.4 | 352.3 | 1.00x | 31.29x | 20.14x |
| C       / -l -w | 35.9 | 28.4 | 503.4 | 355.3 | 0.79x | 14.02x | 9.89x |
| C       / -l -L | 18.0 | 18.0 | 566.2 | 350.4 | 1.00x | 31.38x | 19.42x |
| C       / -l -w -m | 35.9 | 28.6 | 532.6 | 376.4 | 0.80x | 14.83x | 10.48x |
| C.UTF-8 / (default) | 39.3 | 35.5 | 541.9 | 2916.6 | 0.90x | 13.78x | 74.19x |
| C.UTF-8 / -l | 5.8 | 5.7 | 17.2 | 60.3 | 0.98x | 2.98x | 10.43x |
| C.UTF-8 / -w | 38.8 | 35.4 | 491.0 | 2917.3 | 0.91x | 12.64x | 75.10x |
| C.UTF-8 / -c | 1.4 | 1.4 | 3.0 | 1.4 | 0.99x | 2.14x | 1.01x |
| C.UTF-8 / -m | 5.8 | 5.9 | 17.4 | 2900.7 | 1.01x | 2.99x | 497.57x |
| C.UTF-8 / -L | 17.6 | 17.5 | 599.0 | 5028.5 | 1.00x | 34.10x | 286.30x |
| C.UTF-8 / -L -m | 24.5 | 24.1 | 575.1 | 5056.8 | 0.98x | 23.47x | 206.38x |
| C.UTF-8 / -l -w | 39.3 | 35.4 | 536.3 | 2887.0 | 0.90x | 13.63x | 73.39x |
| C.UTF-8 / -l -L | 18.1 | 18.1 | 566.6 | 5008.5 | 1.00x | 31.38x | 277.37x |
| C.UTF-8 / -l -w -m | 40.0 | 36.1 | 535.5 | 2900.3 | 0.90x | 13.37x | 72.42x |
