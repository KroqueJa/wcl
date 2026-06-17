#!/usr/bin/env bash
# Three-stage PGO+LTO build for qwc. Run from the project root.
#
#   ./scripts/build-pgo.sh                  # default build dir: build-pgo/
#   ./scripts/build-pgo.sh path/to/build    # custom build dir
#
# Stage 1: instrumented build (LTO off; some toolchain combos corrupt the
#          instrumented binary with LTO + -fprofile-generate).
# Stage 2: training run -- every DEFAULT_FLAGS bundle on every training-
#          corpus shape, plus one multi-file invocation for orchestration.
# Stage 3: merge raw profile (Clang/AppleClang only; GCC reads .gcda in
#          place).
# Stage 4: PGO + LTO final build. Resulting binary at ./qwc per the
#          project's RUNTIME_OUTPUT_DIRECTORY.
set -euo pipefail

BUILD_DIR="${1:-build-pgo}"
PROFILE_DIR="$(pwd)/${BUILD_DIR}/pgo-profile"
TRAIN_DIR="${BUILD_DIR}/pgo-train"

echo "=== Stage 1: instrumented build (LTO off) ==="
rm -rf "$BUILD_DIR" "$PROFILE_DIR"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
      -DQWC_PGO=generate -DQWC_PGO_DATA_DIR="$PROFILE_DIR" -DQWC_LTO=OFF
cmake --build "$BUILD_DIR" --target qwc -j

echo "=== Stage 2: training run ==="
python3 benchmarks/gen-data.py --pgo-training --out-dir "$TRAIN_DIR"
# RUNTIME_OUTPUT_DIRECTORY in CMakeLists.txt emits to the project root, not
# the build dir -- so the instrumented binary is ./qwc, not $BUILD_DIR/qwc.
QWC="./qwc"
for f in "$TRAIN_DIR"/*.txt; do
  for flags in "" "-l" "-w" "-c" "-m" "-L" "-L -m" \
               "-l -w" "-l -L" "-l -w -m"; do
    # shellcheck disable=SC2086
    "$QWC" $flags "$f" > /dev/null
  done
done
# Multi-file invocation: exercises per-file orchestration and mapFiles.
"$QWC" -l -w -c "$TRAIN_DIR"/*.txt > /dev/null

echo "=== Stage 3: merge profile (Clang only) ==="
if "${CXX:-c++}" --version 2>&1 | grep -qi clang; then
  if command -v llvm-profdata >/dev/null; then
    llvm-profdata merge -output="$PROFILE_DIR/qwc.profdata" \
                         "$PROFILE_DIR"/default_*.profraw
  elif command -v xcrun >/dev/null; then
    xcrun llvm-profdata merge -output="$PROFILE_DIR/qwc.profdata" \
                               "$PROFILE_DIR"/default_*.profraw
  else
    echo "ERROR: Clang detected but neither llvm-profdata nor xcrun on PATH" >&2
    exit 1
  fi
fi

# Wipe stage-1 objects before reconfiguring with use+LTO. Stale objects
# can still link successfully but with mixed PGO state.
cmake --build "$BUILD_DIR" --target clean >/dev/null 2>&1 || true

echo "=== Stage 4: PGO + LTO final build ==="
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
      -DQWC_PGO=use -DQWC_PGO_DATA_DIR="$PROFILE_DIR" -DQWC_LTO=ON
cmake --build "$BUILD_DIR" --target qwc -j

echo
echo "PGO+LTO build complete: ./qwc"
ls -l qwc
