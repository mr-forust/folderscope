#!/usr/bin/env bash
set -euo pipefail

release_script="$(realpath "$1")"
test_directory="$(mktemp -d)"
trap 'rm -rf "$test_directory"' EXIT

remote="$test_directory/remote.git"
work="$test_directory/work"
git init -q --bare "$remote"
git init -q -b main "$work"
git -C "$work" config user.name "FolderScope Tests"
git -C "$work" config user.email "tests@folderscope.invalid"
printf '9.9.9\n' > "$work/VERSION"
printf '# Changelog\n\n## [9.9.9]\n' > "$work/CHANGELOG.md"
git -C "$work" add VERSION CHANGELOG.md
git -C "$work" commit -q -m "test fixture"
git -C "$work" remote add origin "$remote"
git -C "$work" push -qu origin main
git -C "$work" branch --set-upstream-to=origin/main main >/dev/null

touch "$work/untracked"
if (cd "$work" && "$release_script" 9.9.9 >/dev/null 2>&1); then
    echo "release.sh accepted a repository with untracked files" >&2
    exit 1
fi
rm "$work/untracked"

git -C "$work" checkout -qb feature
if (cd "$work" && "$release_script" 9.9.9 >/dev/null 2>&1); then
    echo "release.sh accepted a branch other than main" >&2
    exit 1
fi
git -C "$work" checkout -q main

printf 'local\n' > "$work/local.txt"
git -C "$work" add local.txt
git -C "$work" commit -q -m "local commit"
if (cd "$work" && "$release_script" 9.9.9 >/dev/null 2>&1); then
    echo "release.sh accepted main ahead of origin/main" >&2
    exit 1
fi
git -C "$work" reset -q --hard origin/main

(cd "$work" && "$release_script" 9.9.9 >/dev/null)
test "$(git -C "$work" rev-parse 'v9.9.9^{commit}')" = \
    "$(git -C "$work" rev-parse HEAD)"
