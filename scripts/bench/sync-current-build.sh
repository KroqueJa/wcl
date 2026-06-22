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
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

build_dir="/tmp/qwc-bench-build"
target="$repo_root/qwc"

if [ ! -f "$build_dir/build.ninja" ]; then
  echo "sync-current-build: configuring $build_dir..."
  cmake -G Ninja -S "$repo_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
fi
ninja -C "$build_dir" qwc
cp "$build_dir/qwc" "$target"

echo "sync-current-build: $target now $("$target" --version 2>/dev/null | awk '{print $2}')"
