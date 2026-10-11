#!/usr/bin/env python3
import base64
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading


EXPECTED_PATH = "/api/packages/testowner/arch/testrepo"


class ArchRegistry(BaseHTTPRequestHandler):
    def log_message(self, _format, *_args):
        pass

    def _respond(self, status, payload):
        body = payload.encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_PUT(self):
        self.server.request_count += 1
        if self.path != EXPECTED_PATH:
            self._respond(404, "unexpected endpoint")
            return
        expected = "Basic " + base64.b64encode(
            f"testowner:{self.server.expected_token}".encode()
        ).decode()
        if self.headers.get("Authorization") != expected:
            self._respond(401, "reqPackageAccess")
            return
        if self.server.mode == "unauthorized":
            self._respond(401, "reqPackageAccess")
            return
        if self.server.mode == "conflict":
            self._respond(409, "package file already exists")
            return
        length = int(self.headers.get("Content-Length", "0"))
        self.server.uploaded = self.rfile.read(length)
        self._respond(201, "created")

    def do_DELETE(self):
        self.server.delete_count += 1
        self._respond(204, "")


def run(script, server_url, package, extra_env=None):
    environment = os.environ.copy()
    environment.pop("FOLDERSCOPE_PACKAGE_TOKEN", None)
    environment.pop("GITEA_TOKEN", None)
    if extra_env:
        environment.update(extra_env)
    return subprocess.run(
        [str(script), server_url, "testowner", "testrepo", str(package)],
        env=environment,
        capture_output=True,
    )


def main():
    publish_script = Path(sys.argv[1]).resolve()
    server = ThreadingHTTPServer(("127.0.0.1", 0), ArchRegistry)
    server.request_count = 0
    server.delete_count = 0
    server.uploaded = b""
    server.expected_token = "package-token"
    server.mode = "ok"
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    try:
        server_url = f"http://127.0.0.1:{server.server_port}"
        with tempfile.TemporaryDirectory(prefix="folderscope-arch-publish-") as temp:
            package = Path(temp) / "folderscope-1.2.3-1-x86_64.pkg.tar.zst"
            package_data = b"fake arch package contents"
            package.write_bytes(package_data)

            published = run(
                publish_script, server_url, package,
                {"FOLDERSCOPE_PACKAGE_TOKEN": "package-token"},
            )
            assert published.returncode == 0, published.stderr.decode(errors="replace")
            assert server.uploaded == package_data, "uploaded bytes differ"
            assert server.delete_count == 0, "publish attempted a delete"

            # FOLDERSCOPE_PACKAGE_TOKEN wins over GITEA_TOKEN when both are set.
            server.expected_token = "package-token"
            preferred = run(
                publish_script, server_url, package,
                {"FOLDERSCOPE_PACKAGE_TOKEN": "package-token", "GITEA_TOKEN": "job-token"},
            )
            assert preferred.returncode == 0, preferred.stderr.decode(errors="replace")

            # The built-in job token cannot replace the registry token.
            before = server.request_count
            fallback = run(publish_script, server_url, package,
                           {"GITEA_TOKEN": "job-token"})
            assert fallback.returncode != 0
            assert server.request_count == before

            # Without any token the script refuses before any network traffic.
            before = server.request_count
            missing = run(publish_script, server_url, package)
            assert missing.returncode != 0, "missing token was accepted"
            assert server.request_count == before, "request sent without credentials"

            # A non-package file is rejected before any network traffic.
            server.expected_token = "package-token"
            before = server.request_count
            not_package = Path(temp) / "notes.txt"
            not_package.write_text("not a package\n")
            rejected = run(
                publish_script, server_url, not_package,
                {"FOLDERSCOPE_PACKAGE_TOKEN": "package-token"},
            )
            assert rejected.returncode != 0, "non-package file was accepted"
            assert server.request_count == before, "request sent for invalid file"

            # HTTP 401 reports the FOLDERSCOPE_PACKAGE_TOKEN remedy and deletes nothing.
            server.mode = "unauthorized"
            denied = run(
                publish_script, server_url, package,
                {"FOLDERSCOPE_PACKAGE_TOKEN": "package-token"},
            )
            assert denied.returncode != 0, "HTTP 401 was reported as success"
            assert "FOLDERSCOPE_PACKAGE_TOKEN" in denied.stderr.decode(errors="replace")
            assert server.delete_count == 0, "failed publish attempted a delete"

            # HTTP 409 fails without deleting the published copy.
            server.mode = "conflict"
            conflicted = run(
                publish_script, server_url, package,
                {"FOLDERSCOPE_PACKAGE_TOKEN": "package-token"},
            )
            assert conflicted.returncode != 0, "HTTP 409 was reported as success"
            assert server.delete_count == 0, "conflict deleted the published copy"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
