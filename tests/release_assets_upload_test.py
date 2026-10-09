#!/usr/bin/env python3
import email.policy
import hashlib
import json
import os
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from urllib.parse import parse_qs, urlsplit


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

    def do_POST(self):
        self.server.post_count += 1
        parsed = urlsplit(self.path)
        if parsed.path != "/api/v1/repos/forust/folderscope/releases/42/assets":
            self._respond(404, {"message": "unexpected endpoint"})
            return
        if self.headers.get("Authorization") != "token test-token":
            self._respond(401, {"message": "unexpected token"})
            return

        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        content_type = self.headers.get("Content-Type", "")
        message = BytesParser(policy=email.policy.default).parsebytes(
            f"Content-Type: {content_type}\r\nMIME-Version: 1.0\r\n\r\n".encode()
            + body
        )
        attachments = [part for part in message.iter_parts() if part.get_filename()]
        if len(attachments) != 1:
            self._respond(400, {"message": "expected one attachment"})
            return

        attachment = attachments[0]
        name = attachment.get_filename()
        query_name = parse_qs(parsed.query).get("name", [""])[0]
        if name != query_name:
            self._respond(400, {"message": "query name does not match upload"})
            return

        if self.server.fail_next_upload:
            self.server.fail_next_upload = False
            self._respond(500, {"message": "simulated upload failure"})
            return
        if name in self.server.assets:
            self._respond(409, {"message": "asset already exists"})
            return

        uploaded_data = attachment.get_payload(decode=True)
        self.server.assets[name] = uploaded_data
        self._respond(201, {
            "id": len(self.server.assets),
            "name": "unexpected.tar.gz" if self.server.wrong_name else name,
            "size": len(uploaded_data) + (1 if self.server.wrong_size else 0),
        })

    def do_DELETE(self):
        self.server.delete_count += 1
        self._respond(204, {})


def main():
    upload_script = Path(sys.argv[1]).resolve()
    server = ThreadingHTTPServer(("127.0.0.1", 0), ReleaseApi)
    server.assets = {"previous-good.tar.gz": b"previous published asset"}
    server.fail_next_upload = True
    server.wrong_name = False
    server.wrong_size = False
    server.post_count = 0
    server.delete_count = 0
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    try:
        with tempfile.TemporaryDirectory(prefix="folderscope-release-assets-") as temp:
            assets = Path(temp) / "assets"
            assets.mkdir()
            archive = assets / "folderscope-1.2.3-linux-amd64-test.tar.gz"
            archive_data = b"new archive contents"
            archive.write_bytes(archive_data)
            checksum = assets / f"{archive.name}.sha256"
            checksum.write_text(
                f"{hashlib.sha256(archive_data).hexdigest()}  {archive.name}\n"
            )

            api_url = f"http://127.0.0.1:{server.server_port}/api/v1/repos/forust/folderscope/releases"
            environment = os.environ.copy()
            environment["GITEA_TOKEN"] = "test-token"
            command = [str(upload_script), api_url, "42", str(assets)]

            failed = subprocess.run(command, env=environment, capture_output=True)
            assert failed.returncode != 0, "upload failure was reported as success"
            assert server.assets == {
                "previous-good.tar.gz": b"previous published asset"
            }, "failed upload changed the previously published assets"
            assert server.delete_count == 0, "upload attempted to delete an old asset"

            uploaded = subprocess.run(command, env=environment, capture_output=True)
            assert uploaded.returncode == 0, uploaded.stderr.decode(errors="replace")
            assert server.assets == {
                "previous-good.tar.gz": b"previous published asset",
                archive.name: archive_data,
                checksum.name: checksum.read_bytes(),
            }
            uploaded_digest = server.assets[checksum.name].decode().split()[0]
            assert uploaded_digest == hashlib.sha256(server.assets[archive.name]).hexdigest()
            assert server.delete_count == 0, "successful upload deleted an old asset"

            for attribute, expected_error in [
                ("wrong_name", b"unexpected asset name"),
                ("wrong_size", b"unexpected asset size"),
            ]:
                server.assets.pop(archive.name, None)
                server.assets.pop(checksum.name, None)
                setattr(server, attribute, True)
                bad_response = subprocess.run(command, env=environment, capture_output=True)
                assert bad_response.returncode != 0
                assert expected_error in bad_response.stderr, bad_response.stderr
                assert b"Traceback" not in bad_response.stderr, bad_response.stderr
                assert server.delete_count == 0
                setattr(server, attribute, False)

            requests_before_bad_checksum = server.post_count
            archive.write_bytes(b"changed after checksum generation")
            rejected = subprocess.run(command, env=environment, capture_output=True)
            assert rejected.returncode != 0, "mismatched checksum was accepted"
            assert server.post_count == requests_before_bad_checksum, (
                "assets were uploaded before local checksum validation"
            )
            assert server.assets["previous-good.tar.gz"] == b"previous published asset"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
