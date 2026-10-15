#!/usr/bin/env python3
"""mock_drive_dl.py — Mock Google Drive alt=media download endpoint for P4
download tests (tools/test_sync_native.cpp --dl-test).

Serves:
  GET /drive/v3/files/<id>?alt=media[&key=...]
    small1 -> 2300 bytes, fast
    big1   -> 8 MiB patterned data, throttled (~3 MB/s) so cancel can trigger
  Honors "Range: bytes=N-" with 206 / Content-Range; 416 when N >= size.
  Unknown id -> 404.

Prints on startup:
  PORT=<port>
  MD5_small1=<hex>
  MD5_big1=<hex>
"""
import hashlib
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

FILES = {
    "small1": b"Hello Trimui-GameSync! " * 100,
    "big1": bytes(i % 251 for i in range(8 * 1024 * 1024)),
}
MD5 = {k: hashlib.md5(v).hexdigest() for k, v in FILES.items()}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        u = urlparse(self.path)
        qs = parse_qs(u.query)
        parts = u.path.strip("/").split("/")
        if (len(parts) == 4 and parts[:3] == ["drive", "v3", "files"]
                and qs.get("alt") == ["media"]):
            fid = parts[3]
            if fid not in FILES:
                self.send_error(404)
                return
            data = FILES[fid]
            start = 0
            rng = self.headers.get("Range")
            if rng and rng.startswith("bytes="):
                try:
                    start = int(rng[len("bytes="):].split("-")[0])
                except ValueError:
                    start = 0
            if start >= len(data):
                self.send_response(416)
                self.end_headers()
                return
            body = data[start:]
            code = 206 if start else 200
            self.send_response(code)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            if code == 206:
                self.send_header(
                    "Content-Range",
                    "bytes %d-%d/%d" % (start, len(data) - 1, len(data)))
            self.end_headers()
            chunk = 65536
            for i in range(0, len(body), chunk):
                if fid == "big1":
                    time.sleep(0.02)
                try:
                    self.wfile.write(body[i:i + chunk])
                except (BrokenPipeError, ConnectionResetError):
                    break
            return
        self.send_error(404)


if __name__ == "__main__":
    srv = HTTPServer(("127.0.0.1", 0), H)
    print("PORT=%d" % srv.server_address[1], flush=True)
    for k in ("small1", "big1"):
        print("MD5_%s=%s" % (k, MD5[k]), flush=True)
    srv.serve_forever()
