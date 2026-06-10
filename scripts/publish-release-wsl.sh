#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/publish-release-wsl.sh <version> [options]

Publishes artifacts created by scripts/release-windows.ps1 using WSL git/gh auth.

Options:
  --repo <owner/name>  GitHub repository to publish to (default: orienw/obs-dpdfnet)
  --draft             Create the GitHub release as a draft
  --prerelease        Force prerelease status
  --release           Force non-prerelease status
  -h, --help          Show this help
USAGE
}

die() {
  echo "error: $*" >&2
  exit 1
}

version=""
repo="orienw/obs-dpdfnet"
draft=0
prerelease_override=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo)
      [[ $# -ge 2 ]] || die "--repo requires a value"
      repo="$2"
      shift 2
      ;;
    --draft)
      draft=1
      shift
      ;;
    --prerelease)
      prerelease_override=1
      shift
      ;;
    --release)
      prerelease_override=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      die "unknown option: $1"
      ;;
    *)
      [[ -z "$version" ]] || die "unexpected extra argument: $1"
      version="$1"
      shift
      ;;
  esac
done

[[ -n "$version" ]] || {
  usage >&2
  exit 2
}

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.]+)?$ ]] ||
  die "version '$version' must look like 0.3.1 or 0.4.0-rc1"

if [[ -n "$prerelease_override" ]]; then
  is_prerelease="$prerelease_override"
elif [[ "$version" =~ ^0\. || "$version" == *-* ]]; then
  is_prerelease=1
else
  is_prerelease=0
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
tag="v$version"
zip_name="obs-dpdfnet-$version-windows-x64.zip"
zip_path="$root/build/$zip_name"
sha_path="$zip_path.sha256"
notes_path="$root/build/release-notes-v$version.md"
commit_path="$root/build/release-commit-v$version.txt"

command -v git >/dev/null || die "git is required"
command -v gh >/dev/null || die "GitHub CLI is required"

[[ -f "$zip_path" ]] || die "missing release zip: $zip_path"
[[ -f "$sha_path" ]] || die "missing checksum file: $sha_path"
[[ -f "$notes_path" ]] || die "missing release notes: $notes_path"
[[ -f "$commit_path" ]] || die "missing release commit file: $commit_path"

expected_sha="$(awk '{print $1; exit}' "$sha_path")"
actual_sha="$(sha256sum "$zip_path" | awk '{print $1}')"
[[ "$actual_sha" == "$expected_sha" ]] ||
  die "checksum mismatch for $zip_name: expected $expected_sha, got $actual_sha"

[[ -z "$(git -C "$root" status --porcelain)" ]] ||
  die "working tree is not clean; commit or stash before publishing"

branch="$(git -C "$root" rev-parse --abbrev-ref HEAD)"
[[ "$branch" != "HEAD" ]] || die "cannot publish from a detached HEAD"

staged_commit="$(tr -d '[:space:]' < "$commit_path")"
[[ -n "$staged_commit" ]] ||
  die "release commit file is empty: $commit_path"

git -C "$root" fetch origin "$branch" >/dev/null
head_sha="$(git -C "$root" rev-parse HEAD)"
remote_sha="$(git -C "$root" rev-parse "origin/$branch")"
[[ "$staged_commit" == "$head_sha" ]] ||
  die "staged zip was built from a different commit ($staged_commit); current HEAD is $head_sha. Re-run scripts/release-windows.ps1."
[[ "$head_sha" == "$remote_sha" ]] ||
  die "HEAD ($head_sha) is not pushed to origin/$branch ($remote_sha)"

if git -C "$root" rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
  die "local tag $tag already exists"
fi

if [[ -n "$(git -C "$root" ls-remote --tags origin "refs/tags/$tag")" ]]; then
  die "remote tag $tag already exists"
fi

gh auth status -h github.com >/dev/null

if gh release view "$tag" -R "$repo" >/dev/null 2>&1; then
  die "GitHub release $tag already exists in $repo"
fi

git -C "$root" tag -a "$tag" -m "obs-dpdfnet $version"
git -C "$root" push origin "$tag"

release_args=(
  release create "$tag"
  -R "$repo"
  --title "obs-dpdfnet $version"
  --notes-file "$notes_path"
)

if [[ "$is_prerelease" == "1" ]]; then
  release_args+=(--prerelease)
fi

if [[ "$draft" == "1" ]]; then
  release_args+=(--draft)
fi

release_args+=("$zip_path" "$sha_path")

gh "${release_args[@]}"
