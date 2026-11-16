#!/usr/bin/env python3
"""Receive an MP4 posted by an STB and publish it into the recording tree.

The STB runs ffmpeg with `-f mp4 -method POST http://<nas>:40778/ingest` and
never writes to its own storage; this service spools the body in tmpfs and
moves the finished file onto JuiceFS.

Layout written (see the recording layout notes):
  <recordings>/<YYYY>/<MM>/<DD>/<channelDir>/<programDir>/<prefix>.mp4
  <recordings>/<YYYY>/<MM>/<DD>/<channelDir>/<programDir>/<prefix>.json

Query parameters: year, month, day, channel, program, name
"""
import json
import os
import shutil
import sys
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = "/mnt/xblk/video/zenroku/recordings"
SPOOL = "/dev/shm/pix-ingest"
PORT = 40778


def target_dir(q):
    year = q.get("year", [""])[0]
    month = q.get("month", [""])[0]
    day = q.get("day", [""])[0]
    channel = q.get("channel", [""])[0]
    program = q.get("program", [""])[0]
    for part in (year, month, day, channel, program):
        if not part or "/" in part or part in (".", ".."):
            raise ValueError("invalid path component: %r" % part)
    return os.path.join(ROOT, year, month, day, channel, program)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (time.strftime("%Y-%m-%dT%H:%M:%S"), fmt % args))

    def do_GET(self):
        if self.path == "/health":
            body = b"ok\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path != "/ingest":
            self.send_error(404)
            return
        q = parse_qs(parsed.query)
        name = q.get("name", ["upload"])[0]
        if "/" in name or name in ("", ".", ".."):
            self.send_error(400, "invalid name")
            return
        try:
            dest_dir = target_dir(q)
        except ValueError as exc:
            self.send_error(400, str(exc))
            return

        os.makedirs(SPOOL, exist_ok=True)
        fd, spool_path = tempfile.mkstemp(dir=SPOOL, suffix=".mp4")
        total = 0
        try:
            with os.fdopen(fd, "wb") as out:
                if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
                    while True:
                        line = self.rfile.readline().strip()
                        if not line:
                            break
                        size = int(line.split(b";")[0], 16)
                        if size == 0:
                            self.rfile.readline()
                            break
                        remaining = size
                        while remaining:
                            chunk = self.rfile.read(min(remaining, 1 << 20))
                            if not chunk:
                                raise IOError("short chunk")
                            out.write(chunk)
                            remaining -= len(chunk)
                            total += len(chunk)
                        self.rfile.read(2)
                else:
                    length = int(self.headers.get("Content-Length", "0"))
                    remaining = length
                    while remaining:
                        chunk = self.rfile.read(min(remaining, 1 << 20))
                        if not chunk:
                            raise IOError("short body")
                        out.write(chunk)
                        remaining -= len(chunk)
                        total += len(chunk)

            os.makedirs(dest_dir, exist_ok=True)
            dest = os.path.join(dest_dir, name)
            tmp_dest = dest + ".part"
            shutil.move(spool_path, tmp_dest)
            os.chmod(tmp_dest, 0o644)
            os.replace(tmp_dest, dest)
            spool_path = None
            self.log_message("stored %s (%d bytes)", dest, total)
            body = json.dumps({"stored": dest, "bytes": total}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception as exc:  # noqa: BLE001 - report any ingest failure to the client
            self.log_message("failed: %s", exc)
            try:
                self.send_error(500, str(exc))
            except Exception:
                pass
        finally:
            if spool_path and os.path.exists(spool_path):
                os.unlink(spool_path)


def main():
    os.makedirs(SPOOL, exist_ok=True)
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    server.daemon_threads = True
    sys.stderr.write("pix-ingest listening on %d, spool %s\n" % (PORT, SPOOL))
    server.serve_forever()


if __name__ == "__main__":
    main()
