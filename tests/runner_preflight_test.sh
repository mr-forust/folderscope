#!/usr/bin/env bash
set -euo pipefail

check_script="$(realpath "$1")"

if "$check_script" --min-free-mib 0 definitely-missing-tool-xyz >/dev/null 2>&1; then
    echo "check-runner.sh accepted a missing tool" >&2
    exit 1
fi
output="$("$check_script" --min-free-mib 0 definitely-missing-tool-xyz 2>&1 || true)"
if [[ "$output" != *"definitely-missing-tool-xyz"* ]]; then
    echo "check-runner.sh did not name the missing tool" >&2
    exit 1
fi

if "$check_script" --min-free-mib 999999999 sh >/dev/null 2>&1; then
    echo "check-runner.sh accepted an impossible disk requirement" >&2
    exit 1
fi

if FOLDERSCOPE_MIN_FREE_MIB=999999999 "$check_script" sh >/dev/null 2>&1; then
    echo "check-runner.sh ignored FOLDERSCOPE_MIN_FREE_MIB" >&2
    exit 1
fi

if [[ "$("$check_script" --min-free-mib 2>&1 || true)" != *"Usage"* ]]; then
    echo "check-runner.sh did not reject a flag without arguments" >&2
    exit 1
fi

"$check_script" --min-free-mib 0 sh >/dev/null
