#!/usr/bin/env bash
# Maintainer-only: bump the personal Homebrew tap (KroqueJa/homebrew-qwc) to
# a qwc release tag. Needs write rights to that tap; contributors don't need
# this script and shouldn't run it. Lives under scripts/maintainer/ to keep
# scripts/ contributor-facing.
#
# Resolves the tag (default: latest tag in this repo), verifies the GitHub
# release has both expected tarballs attached, computes their SHA256s, and
# rewrites <tap>/Formula/qwc.rb in place. Idempotent.
#
# The release.yml workflow (push of vX.Y.Z) must have finished and uploaded
# both qwc-linux-x86_64.tar.gz and qwc-macos-arm64.tar.gz before this can
# run; otherwise we'd compute SHAs of GitHub's 404 page and write those.
# That trap is caught explicitly.
#
# Usage:
#   scripts/maintainer/release-homebrew.sh             # latest tag, edit only
#   scripts/maintainer/release-homebrew.sh v0.2.0      # explicit tag, edit only
#   scripts/maintainer/release-homebrew.sh --push      # also commit + push tap
#
# Env:
#   HOMEBREW_QWC_DIR  path to the homebrew-qwc checkout
#                     (default: <repo>/../homebrew-qwc)
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

push=0
tag=""
for arg in "$@"; do
  case "$arg" in
    --push) push=1 ;;
    -*)     echo "release-homebrew: unknown flag: $arg" >&2; exit 2 ;;
    *)
      if [ -n "$tag" ]; then
        echo "release-homebrew: positional tag given twice" >&2; exit 2
      fi
      tag="$arg"
      ;;
  esac
done

if [ -z "$tag" ]; then
  tag="$(git tag --sort=-creatordate | head -n1)"
fi
if [ -z "$tag" ]; then
  echo "release-homebrew: no tag found in this repository" >&2
  exit 1
fi

tap_root="${HOMEBREW_QWC_DIR:-${repo_root}/../homebrew-qwc}"
formula="${tap_root}/Formula/qwc.rb"
if [ ! -f "$formula" ]; then
  echo "release-homebrew: formula not found at ${formula}" >&2
  echo "  (set HOMEBREW_QWC_DIR if the tap lives elsewhere)" >&2
  exit 1
fi

version="${tag#v}"

# Confirm the release exists with both assets before fetching; otherwise we'd
# happily compute the SHA of GitHub's 404 HTML and write that to the formula.
assets="$(gh release view "$tag" --json assets --jq '.assets[].name')"
for need in qwc-linux-x86_64.tar.gz qwc-macos-arm64.tar.gz; do
  if ! grep -qFx "$need" <<< "$assets"; then
    echo "release-homebrew: release ${tag} is missing asset ${need}" >&2
    echo "  (has the release.yml workflow finished?)" >&2
    exit 1
  fi
done

url_base="https://github.com/KroqueJa/qwc/releases/download/${tag}"
linux_sha="$(curl -fsSL "${url_base}/qwc-linux-x86_64.tar.gz" | sha256sum | awk '{print $1}')"
macos_sha="$(curl -fsSL "${url_base}/qwc-macos-arm64.tar.gz" | sha256sum | awk '{print $1}')"

# Defense in depth: the GitHub-404 HTML page sha. The gh check above should
# prevent it, but if anything ever lets it through, fail loudly.
sha_404="0019dfc4b32d63c1392aa264aed2253c1e0c2fb09216f8e2cc269bbfb8bb49b5"
for s in "$linux_sha" "$macos_sha"; do
  if [ "$s" = "$sha_404" ]; then
    echo "release-homebrew: got the GitHub-404-page SHA, aborting" >&2
    exit 1
  fi
done

# Rewrite the formula. The macOS sha follows the arm64 URL line, the Linux
# sha follows the x86_64 URL line; the version line is the single "version "
# line near the top. Order matches the file as currently structured.
tmp="$(mktemp)"
awk -v ver="$version" -v lin="$linux_sha" -v mac="$macos_sha" '
  /^[[:space:]]*url[[:space:]]+".*arm64\.tar\.gz"/  { next_sha = "mac"; print; next }
  /^[[:space:]]*url[[:space:]]+".*x86_64\.tar\.gz"/ { next_sha = "lin"; print; next }
  /^[[:space:]]*sha256[[:space:]]+"/ {
    if      (next_sha == "mac") { sub(/"[^"]*"/, "\"" mac "\""); next_sha = "" }
    else if (next_sha == "lin") { sub(/"[^"]*"/, "\"" lin "\""); next_sha = "" }
  }
  /^[[:space:]]*version[[:space:]]+"/ { sub(/"[^"]*"/, "\"" ver "\"") }
  { print }
' "$formula" > "$tmp"
mv "$tmp" "$formula"

echo "release-homebrew: bumped ${formula}"
echo "  version:      ${version}"
echo "  linux sha256: ${linux_sha}"
echo "  macos sha256: ${macos_sha}"

if [ "$push" -eq 1 ]; then
  (
    cd "$tap_root"
    git add Formula/qwc.rb
    git commit -m "Bump qwc to ${version}"
    git push
  )
  echo "release-homebrew: tap pushed"
else
  echo "release-homebrew: file edits made, not committed."
  echo "  review:  (cd ${tap_root} && git diff)"
  echo "  publish: re-run with --push, or commit + push manually."
fi
