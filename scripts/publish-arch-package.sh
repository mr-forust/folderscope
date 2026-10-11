#!/usr/bin/env bash
# Publish an Arch package file to the Gitea Arch package registry.
#
# Usage: publish-arch-package.sh SERVER_URL OWNER REPOSITORY PACKAGE_FILE
#
# Authentication requires FOLDERSCOPE_PACKAGE_TOKEN with the write:package scope.
# The built-in job token cannot publish to the registry (HTTP 401).
# Store FOLDERSCOPE_PACKAGE_TOKEN as an Actions secret.
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "Usage: $0 SERVER_URL OWNER REPOSITORY PACKAGE_FILE" >&2
    exit 2
fi

server_url="${1%/}"
owner="$2"
repository="$3"
package_file="$4"

token="${FOLDERSCOPE_PACKAGE_TOKEN:-}"
if [[ -z "$token" ]]; then
    echo "FOLDERSCOPE_PACKAGE_TOKEN is not set; cannot authenticate." >&2
    exit 1
fi
if [[ ! -f "$package_file" || -L "$package_file" ]]; then
    echo "Package file is not a regular file: ${package_file}" >&2
    exit 1
fi
case "$package_file" in
    *.pkg.tar.zst) ;;
    *)
        echo "Not an Arch package file (*.pkg.tar.zst): ${package_file}" >&2
        exit 1
        ;;
esac

url="${server_url}/api/packages/${owner}/arch/${repository}"
response_file="$(mktemp)"
trap 'rm -f "$response_file"' EXIT
http_code="$(curl --silent --show-error --output "$response_file" \
    --write-out '%{http_code}' --user "${owner}:${token}" \
    --upload-file "$package_file" "$url")" || {
    echo "Upload request failed before receiving an HTTP response." >&2
    exit 1
}

case "$http_code" in
    201)
        echo "Published $(basename "$package_file") to ${owner}/arch/${repository}."
        ;;
    401)
        echo "Registry refused the credentials (HTTP 401)." >&2
        echo "Create a personal access token with the 'write:package' scope and" >&2
        echo "store it as the FOLDERSCOPE_PACKAGE_TOKEN Actions secret." >&2
        exit 1
        ;;
    409)
        echo "Package file already published (HTTP 409); refusing to overwrite." >&2
        echo "Bump pkgrel or delete the existing file before republishing." >&2
        exit 1
        ;;
    *)
        echo "Upload failed with HTTP ${http_code}." >&2
        exit 1
        ;;
esac
