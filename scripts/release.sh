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
