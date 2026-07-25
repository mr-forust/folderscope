#!/usr/bin/env bash
set -euo pipefail

release_script="$(realpath "$1")"
test_directory="$(mktemp -d)"
trap 'rm -rf "$test_directory"' EXIT

git -C "$test_directory" init -q
git -C "$test_directory" config user.name "FolderScope Tests"
git -C "$test_directory" config user.email "tests@folderscope.invalid"
printf '9.9.9\n' > "$test_directory/VERSION"
printf '# Changelog\n\n## [9.9.9]\n' > "$test_directory/CHANGELOG.md"
git -C "$test_directory" add VERSION CHANGELOG.md
git -C "$test_directory" commit -q -m "test fixture"
touch "$test_directory/untracked"

if (cd "$test_directory" && "$release_script" 9.9.9 >/dev/null 2>&1); then
    echo "release.sh accepted a repository with untracked files" >&2
    exit 1
fi
