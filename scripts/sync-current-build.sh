#!/usr/bin/env bash
# Build the current working tree's qwc binary as ./qwc, for the bench scripts
# to use as the candidate (the binary being measured) against the latest-release
# baseline produced by sync-latest-release.sh.
#
# Builds from the WORKING TREE, not HEAD: uncommitted changes are included, so
# in-flight perf work measures honestly. The version header still derives from
# `git describe --tags --always --dirty`, so a dirty tree shows up as such in
# the binary's --version.
#
# Uses a stable /tmp build dir so ninja can rebuild incrementally; near no-op
# when nothing has changed (the cmake configure only re-runs if CMakeLists.txt
# or the cache is stale).
#
# Set QWC_BENCH_PGO=1 to build the candidate the way a release ships it --
# the full three-stage PGO+LTO pipeline (build-pgo.sh) instead of the plain
# Release build below. Use this to benchmark PGO/LTO against a release that
# predates it. The PGO path is a full multi-stage rebuild, hence opt-in.
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

build_dir="/tmp/qwc-bench-build"
target="$repo_root/qwc"

# PGO/LTO mode: delegate to the three-stage orchestrator. Both paths emit to
# ./qwc (CMake's RUNTIME_OUTPUT_DIRECTORY), so they are mutually exclusive per
# run -- whichever ran last owns ./qwc. build-pgo.sh expects a *relative*
# build dir (it derives the profile dir as $(pwd)/<dir>/...), and uses its own
# build-pgo/ tree, leaving this script's incremental /tmp dir untouched.
if [ "${QWC_BENCH_PGO:-0}" = "1" ]; then
  echo "sync-current-build: QWC_BENCH_PGO=1 -> PGO+LTO build via build-pgo.sh"
  "$repo_root/scripts/build-pgo.sh" build-pgo
  echo "sync-current-build: $target now $("$target" --version 2>/dev/null | awk '{print $2}')"
  exit 0
fi

if [ ! -f "$build_dir/build.ninja" ]; then
  echo "sync-current-build: configuring $build_dir..."
  cmake -G Ninja -S "$repo_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
fi
ninja -C "$build_dir" qwc
# CMake's RUNTIME_OUTPUT_DIRECTORY is the source tree, so ninja writes the
# binary straight to ./qwc -- no cp needed (see CMakeLists.txt around the
# `set_target_properties( ... RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR} )`
# line).

echo "sync-current-build: $target now $("$target" --version 2>/dev/null | awk '{print $2}')"
