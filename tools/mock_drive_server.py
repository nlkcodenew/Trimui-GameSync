#!/usr/bin/env python3
"""mock_drive_server.py — Tiny canned Drive API v3 mock for testing
drive_list_replica.py (and, transitively, the C++ Drive module logic).

Serves GET /drive/v3/files honoring the `q` param ("'<id>' in parents")
and pageToken pagination. Prints "PORT=<n>" on stdout, then serves forever.

Canned tree (root id "mockrootfolderid123"):
  root/
    Nintendo - Game Boy Advance/   (folder, id f_gba)
      game1.zip                   (size 1234567, md5 set)
      Sub/                        (folder, id f_sub)
        nested.bin                (no md5Checksum field)
    PortMaster/                   (folder, id f_pm) -> MUST be skipped
      should_not_appear.bin
    readme.txt                    (file at root, size 42)
    Sega - Genesis/               (folder, id f_md, on page 2)
      Weird "quoted" (name).md    (tests JSON string escaping)
    Pokémon — Red (USA).zip       (file at root, page 2, unicode name)
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

FOLDER_MIME = "application/vnd.google-apps.folder"


def F(fid, name):
    return {"id": fid, "name": name, "mimeType": FOLDER_MIME}


def D(fid, name, size=None, md5=None):
    d = {"id": fid, "name": name, "mimeType": "application/zip"}
    if size is not None:
        d["size"] = str(size)  # Drive API returns size as STRING
    if md5 is not None:
        d["md5Checksum"] = md5
    d["modifiedTime"] = "2026-01-02T03:04:05.000Z"
    return d


# (children_page1, children_page2) per folder id
TREE = {
        "mockrootfolderid123": (
        [F("f_gba", "Nintendo - Game Boy Advance"),
         F("f_pm", "PortMaster"),
         D("doc1", "readme.txt", 42, "aaa")],
        [F("f_md", "Sega - Genesis"),
         D("doc2", "Pok\xe9mon \u2014 Red (USA).zip", 100, "bbb")],
    ),
    "f_gba": (
        [D("g1", "game1.zip", 1234567, "d41d8cd98f00b204e9800998ecf8427e"),
         F("f_sub", "Sub")],
        [],
    ),
    "f_sub": ([D("n1", "nested.bin")], []),
    "f_md": ([D("w1", 'Weird "quoted" (name).md', 7, "ccc")], []),
    "f_pm": ([D("x1", "should_not_appear.bin", 9, "ddd")], []),
}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        u = urlparse(self.path)
        qs = parse_qs(u.query)
        if u.path != "/drive/v3/files":
            self.send_error(404)
            return
        q = qs.get("q", [""])[0]
        fid = q.split("'")[1] if q.count("'") >= 2 else ""
        token = qs.get("pageToken", [""])[0]
        if fid not in TREE:
            body = {"error": {"message": "File not found: %s" % fid}}
            code = 404
        else:
            p1, p2 = TREE[fid]
            page = p2 if token == "p2" else p1
            body = {"files": page}
            if token != "p2" and p2:
                body["nextPageToken"] = "p2"
            code = 200
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    srv = HTTPServer(("127.0.0.1", 0), H)
    print("PORT=%d" % srv.server_address[1], flush=True)
    srv.serve_forever()
