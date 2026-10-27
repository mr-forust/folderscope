#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: $0 RELEASES_API_URL TAG COMMIT" >&2
    exit 2
fi

if [[ -z "${GITEA_TOKEN:-}" ]]; then
    echo "GITEA_TOKEN is required." >&2
    exit 2
fi

python3 - "$1" "$2" "$3" <<'PY'
import json
import os
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


releases_url, tag, commit = sys.argv[1:]
releases_url = releases_url.rstrip("/")
token = os.environ["GITEA_TOKEN"]


def request_json(method, url, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    headers = {"Authorization": f"token {token}", "User-Agent": "folderscope-ci/1.0"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    request = Request(url, data=data, headers=headers, method=method)
    with urlopen(request, timeout=20) as response:
        return json.load(response)


def release_id(release):
    if not isinstance(release, dict):
        raise SystemExit("Gitea returned an invalid release response.")
    value = release.get("id")
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise SystemExit("Gitea release response has no valid ID.")
    return value


tag_url = f"{releases_url}/tags/{quote(tag, safe='')}"
try:
    existing = request_json("GET", tag_url)
except HTTPError as error:
    if error.code != 404:
        print(f"Gitea release lookup failed with HTTP {error.code}.", file=sys.stderr)
        raise SystemExit(1) from None
    existing = None
except (URLError, TimeoutError) as error:
    print(f"Gitea release lookup failed: {error}", file=sys.stderr)
    raise SystemExit(1) from None

if existing is not None:
    print(release_id(existing))
    raise SystemExit(0)

payload = {
    "tag_name": tag,
    "target_commitish": commit,
    "name": f"FolderScope {tag}",
    "body": f"Release {tag}. See CHANGELOG.md for notable changes.",
    "draft": False,
    "prerelease": False,
}
try:
    created = request_json("POST", releases_url, payload)
except HTTPError as error:
    if error.code == 409:
        try:
            existing = request_json("GET", tag_url)
        except (HTTPError, URLError, TimeoutError) as retry_error:
            print(f"Gitea release creation conflicted and lookup failed: {retry_error}", file=sys.stderr)
            raise SystemExit(1) from None
        print(release_id(existing))
        raise SystemExit(0)
    print(f"Gitea release creation failed with HTTP {error.code}.", file=sys.stderr)
    raise SystemExit(1) from None
except (URLError, TimeoutError) as error:
    print(f"Gitea release creation failed: {error}", file=sys.stderr)
    raise SystemExit(1) from None

print(release_id(created))
PY
