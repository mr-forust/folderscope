#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Usage: $0 MAJOR.MINOR.PATCH" >&2
  exit 2
fi

version="$1"
tag="v${version}"

if [[ -n "$(git status --porcelain)" ]]; then
  echo "Refusing to release with uncommitted changes." >&2
  exit 1
fi
if [[ "$(git branch --show-current)" != "main" ]]; then
  echo "Refusing to release from a branch other than main." >&2
  exit 1
fi
if ! git remote get-url origin >/dev/null 2>&1; then
  echo "Refusing to release without an origin remote." >&2
  exit 1
fi
if ! git fetch --quiet origin main; then
  echo "Could not update origin/main before release." >&2
  exit 1
fi
if [[ "$(git rev-parse HEAD)" != "$(git rev-parse refs/remotes/origin/main)" ]]; then
  echo "Refusing to release from a main branch that is not up to date with origin/main." >&2
  exit 1
fi
if git rev-parse -q --verify "refs/tags/${tag}" >/dev/null; then
  echo "Tag ${tag} already exists." >&2
  exit 1
fi
if ! grep -q "^## \[${version}\]" CHANGELOG.md; then
  echo "Add a ## [${version}] section to CHANGELOG.md first." >&2
  exit 1
fi

current_version="$(tr -d '[:space:]' < VERSION)"
if [[ "$current_version" != "$version" ]]; then
  printf '%s\n' "$version" > VERSION
  git add VERSION
  git commit -m "chore: release ${tag}"
fi
git tag -a "$tag" -m "Release ${tag}"

echo "Created ${tag}. Publish it with:"
echo "  git push origin main --follow-tags"
