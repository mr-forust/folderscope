#!/usr/bin/env bash
# Fail fast when the self-hosted runner lacks the build toolchain or disk
# space. Run right after checkout, before any configure/build step.
#
# Usage: check-runner.sh [--min-free-mib MIB] tool [tool ...]
# The minimum free space defaults to 2048 MiB and can be overridden with
# FOLDERSCOPE_MIN_FREE_MIB.
set -euo pipefail

min_free_mib="${FOLDERSCOPE_MIN_FREE_MIB:-2048}"
if [[ "${1:-}" == "--min-free-mib" ]]; then
    if [[ $# -lt 3 ]]; then
        echo "Usage: $0 [--min-free-mib MIB] tool [tool ...]" >&2
        exit 2
    fi
    min_free_mib="$2"
    shift 2
fi
if [[ $# -lt 1 ]]; then
    echo "Usage: $0 [--min-free-mib MIB] tool [tool ...]" >&2
    exit 2
fi
if [[ ! "$min_free_mib" =~ ^[0-9]+$ ]]; then
    echo "Minimum free space must be a non-negative integer (MiB)." >&2
    exit 2
fi

missing=()
for tool in "$@"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        missing+=("$tool")
    fi
done

avail_mib="$(df -m --output=avail "${FOLDERSCOPE_CHECK_PATH:-$PWD}" | tail -n 1 | tr -d '[:space:]')"
if [[ ! "$avail_mib" =~ ^[0-9]+$ ]]; then
    echo "Could not determine free disk space for ${PWD}." >&2
    exit 1
fi

failed=0
if (( ${#missing[@]} > 0 )); then
    echo "Runner is missing required tools: ${missing[*]}." >&2
    echo "Install the documented runner toolchain instead of running a partial build." >&2
    failed=1
fi
if (( avail_mib < min_free_mib )); then
    echo "Runner has only ${avail_mib} MiB free; ${min_free_mib} MiB required." >&2
    echo "Free disk space before retrying; the build must not fill the runner disk." >&2
    failed=1
fi
if (( failed )); then
    exit 1
fi
echo "Runner preflight passed: ${avail_mib} MiB free; tools present: $*."
