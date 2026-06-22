#!/usr/bin/env bash
# Build the latest release tag's qwc binary as ./qwc-latest-release for use
# as the bench-script baseline. Idempotent: a no-op when ./qwc-latest-release
# already reports the latest tag's version. The bench-*.sh wrappers invoke
# this on every run, so a fresh checkout's first bench builds the baseline
# and subsequent runs reuse it.
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

latest_tag="$(git tag --sort=-creatordate | head -n1)"
if [ -z "$latest_tag" ]; then
  echo "sync-latest-release: no release tag found in this repository" >&2
  exit 1
fi

target="$repo_root/qwc-latest-release"
if [ -x "$target" ]; then
  current="$("$target" --version 2>/dev/null | awk '{print $2}' || true)"
  if [ "$current" = "$latest_tag" ]; then
    exit 0
  fi
fi

echo "sync-latest-release: building $latest_tag in a worktree..."
worktree="/tmp/qwc-build-${latest_tag}"
if [ -e "$worktree" ]; then
  git worktree remove --force "$worktree" 2>/dev/null || rm -rf "$worktree"
fi

git worktree add --detach "$worktree" "$latest_tag"
(
  cd "$worktree"
  git submodule update --init --recursive
  mkdir -p build
  cd build
  cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
  ninja qwc
)
cp "$worktree/build/qwc" "$target"
git worktree remove --force "$worktree"

echo "sync-latest-release: $target now $latest_tag"
