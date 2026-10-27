#!/usr/bin/env python3
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import threading


class ReleaseApi(BaseHTTPRequestHandler):
    def log_message(self, _format, *_args):
        pass

    def _respond(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.server.get_count += 1
        if self.headers.get("Authorization") != "token test-token" or self.headers.get("User-Agent") != "folderscope-ci/1.0":
            self._respond(401, {"message": "unexpected token"})
            return
        if self.path != "/api/v1/repos/forust/folderscope/releases/tags/v1.2.3":
            self._respond(404, {"message": "unexpected endpoint"})
            return
        if self.server.get_status == 404 and self.server.post_count == 0:
            self._respond(404, {"message": "release does not exist"})
            return
        if self.server.get_status >= 500:
            self._respond(self.server.get_status, {"message": "simulated server failure"})
            return
        self._respond(200, {"id": self.server.existing_id, "tag_name": "v1.2.3"})

    def do_POST(self):
        self.server.post_count += 1
        if self.headers.get("Authorization") != "token test-token" or self.headers.get("User-Agent") != "folderscope-ci/1.0":
            self._respond(401, {"message": "unexpected token"})
            return
        if self.path != "/api/v1/repos/forust/folderscope/releases":
            self._respond(404, {"message": "unexpected endpoint"})
            return

        payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.received_payload = payload
        if self.server.post_status >= 400:
            self._respond(self.server.post_status, {"message": "simulated create failure"})
            return
        self._respond(201, {"id": self.server.created_id, "tag_name": payload["tag_name"]})


def run_helper(helper, server):
    environment = os.environ.copy()
    environment["GITEA_TOKEN"] = "test-token"
    url = f"http://127.0.0.1:{server.server_port}/api/v1/repos/forust/folderscope/releases"
    return subprocess.run(
        [str(helper), url, "v1.2.3", "deadbeef"],
        env=environment,
        capture_output=True,
        text=True,
        timeout=10,
    )


def main():
    helper = Path(sys.argv[1]).resolve()
    server = ThreadingHTTPServer(("127.0.0.1", 0), ReleaseApi)
    server.get_status = 404
    server.get_count = 0
    server.post_count = 0
    server.existing_id = 27
    server.created_id = 42
    server.post_status = 201
    server.received_payload = None
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    try:
        created = run_helper(helper, server)
        assert created.returncode == 0, created.stderr
        assert created.stdout.strip() == "42"
        assert server.get_count == 1
        assert server.post_count == 1
        assert server.received_payload == {
            "tag_name": "v1.2.3",
            "target_commitish": "deadbeef",
            "name": "FolderScope v1.2.3",
            "body": "Release v1.2.3. See CHANGELOG.md for notable changes.",
            "draft": False,
            "prerelease": False,
        }

        server.get_status = 200
        server.get_count = 0
        server.post_count = 0
        reused = run_helper(helper, server)
        assert reused.returncode == 0, reused.stderr
        assert reused.stdout.strip() == "27"
        assert server.get_count == 1
        assert server.post_count == 0

        server.get_status = 500
        server.get_count = 0
        failed = run_helper(helper, server)
        assert failed.returncode != 0, "server error was treated as a missing release"
        assert server.get_count == 1
        assert server.post_count == 0, "server error unexpectedly created a release"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
