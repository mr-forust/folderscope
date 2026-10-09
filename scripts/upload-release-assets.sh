#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: $0 RELEASES_API_URL RELEASE_ID ASSET_DIRECTORY" >&2
    exit 2
fi

releases_api_url="${1%/}"
release_id="$2"
asset_directory="$3"
if [[ ! "$release_id" =~ ^[0-9]+$ || ! -d "$asset_directory" ]]; then
    echo "Invalid release ID or asset directory." >&2
    exit 2
fi
if [[ -z "${GITEA_TOKEN:-}" ]]; then
    echo "GITEA_TOKEN is required." >&2
    exit 2
fi

shopt -s nullglob
assets=("$asset_directory"/*)
checksums=("$asset_directory"/*.sha256)
if (( ${#assets[@]} == 0 )); then
    echo "No release assets found in ${asset_directory}." >&2
    exit 1
fi

for checksum in "${checksums[@]}"; do
    (cd "$asset_directory" && sha256sum --check --strict "$(basename "$checksum")")
done

for asset in "${assets[@]}"; do
    if [[ ! -f "$asset" || -L "$asset" ]]; then
        echo "Release asset is not a regular file: ${asset}" >&2
        exit 1
    fi
    name="$(basename "$asset")"
    encoded_name="$(python3 -c \
        'import sys; from urllib.parse import quote; print(quote(sys.argv[1], safe=""))' \
        "$name")"
    response="$(curl -fsS \
        -X POST "${releases_api_url}/${release_id}/assets?name=${encoded_name}" \
        -H "Authorization: token ${GITEA_TOKEN}" \
        -F "attachment=@${asset}")"
    python3 -c '
import json
import os
import sys

response = json.load(sys.stdin)
name, asset_path = sys.argv[1:]
received_name = response.get("name")
if received_name != name:
    raise SystemExit(f"Gitea returned unexpected asset name: {received_name!r}")
if response.get("size") != os.stat(asset_path).st_size:
    raise SystemExit(f"Gitea returned unexpected asset size for {name}.")
' "$name" "$asset" <<< "$response"
done
