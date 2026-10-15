#!/usr/bin/env python3
"""drive_list_replica.py — Independent Python replica of the C++ Drive module
(Trimui-GameSync P3: src/Drive.h / src/Drive.cpp).

Implements the SAME logic so the parent agent can cross-check the C++ client
(which cannot run on WSL/aarch64):
  - parse_drive_folder_id(): same markers/rules as C++ parseDriveFolderId
  - list_children(): same Drive API v3 params + pageToken pagination
  - list_recursive(): same recursion, same "PortMaster" skip rule,
    same system_map.json mapping, same relPath sorting
  - binary cache: byte-identical format to C++ saveDriveCache
    (magic "GSDRV001", little-endian integers)

Security: the API key is NEVER committed. It is read from the
GS_DRIVE_API_KEY environment variable or the --key argument only.

Usage:
  GS_DRIVE_API_KEY=... python3 drive_list_replica.py --roots <url-or-id>... [--out json|bin]
  python3 drive_list_replica.py --test-parse
  python3 drive_list_replica.py --base-url http://127.0.0.1:PORT ...  # mock testing
"""

import argparse
import json
import os
import struct
import sys
import urllib.parse
import urllib.request

DRIVE_FOLDER_MIME = "application/vnd.google-apps.folder"
MAGIC = b"GSDRV001"


# ---------------------------------------------------------------- parsing

def parse_drive_folder_id(s):
    """Mirror of C++ parseDriveFolderId."""
    s = (s or "").strip(" \t\r\n")
    for mk in ("/folders/", "open?id=", "id="):
        p = s.find(mk)
        if p != -1:
            s = s[p + len(mk):]
            break
    # cut at first of / ? & #
    cut = len(s)
    for ch in "/?&#":
        p = s.find(ch)
        if p != -1:
            cut = min(cut, p)
    s = s[:cut]
    if len(s) < 10:
        return ""
    for ch in s:
        if not (ch.isalnum() or ch in "-_"):
            return ""
    return s


# ---------------------------------------------------------------- client

class DriveError(Exception):
    def __init__(self, http_code, message):
        super().__init__(message)
        self.http_code = http_code
        self.message = message


class DriveClient:
    def __init__(self, api_key, base_url="https://www.googleapis.com"):
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")

    def _get(self, path, params):
        params = dict(params)
        params["key"] = self.api_key
        url = self.base_url + path + "?" + urllib.parse.urlencode(params)
        req = urllib.request.Request(
            url, headers={"User-Agent": "Trimui-GameSync/1.0"})
        try:
            with urllib.request.urlopen(req, timeout=90) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", "replace")
            return e.code, body
        except Exception as e:  # transport failure
            raise DriveError(0, "Loi mang: %s" % e)

    def list_children(self, folder_id):
        """Mirror of C++ DriveClient::listChildren. Returns list of dicts."""
        if not self.api_key:
            raise DriveError(0, "Thieu API key Drive.")
        out = []
        page_token = ""
        while True:
            params = {
                "q": "'%s' in parents and trashed=false" % folder_id,
                "fields": "files(id,name,mimeType,size,md5Checksum,modifiedTime),nextPageToken",
                "pageSize": "1000",
            }
            if page_token:
                params["pageToken"] = page_token
            code, body = self._get("/drive/v3/files", params)
            try:
                j = json.loads(body)
            except Exception as e:
                raise DriveError(code, "JSON loi: %s" % e)
            if isinstance(j, dict) and "error" in j:
                msg = j["error"].get("message", "Drive API bao loi.")
                raise DriveError(code, "Drive API: %s" % msg)
            if code != 200:
                raise DriveError(code, "HTTP %d" % code)
            for f in j.get("files", []):
                if not isinstance(f, dict) or not f.get("id"):
                    continue
                size = 0
                if isinstance(f.get("size"), str) and f["size"].isdigit():
                    size = int(f["size"])
                out.append({
                    "id": f.get("id", ""),
                    "name": f.get("name", ""),
                    "mimeType": f.get("mimeType", ""),
                    "size": size,
                    "md5": f.get("md5Checksum", "") or "",
                    "mtime": f.get("modifiedTime", "") or "",
                    "isFolder": f.get("mimeType") == DRIVE_FOLDER_MIME,
                })
            page_token = j.get("nextPageToken") or ""
            if not page_token:
                break
        return out

    def list_recursive(self, root_folder_id, system_map):
        """Mirror of C++ DriveClient::listRecursive. Returns list of dicts
        sorted by relPath (byte order of UTF-8)."""
        out = []
        stack = [(root_folder_id, "", "")]  # (id, relPrefix, topFolder)
        while stack:
            fid, rel_prefix, top_folder = stack.pop()
            for k in self.list_children(fid):
                if k["isFolder"]:
                    if k["name"] == "PortMaster":
                        continue  # out of app scope (defense in depth)
                    top = top_folder or k["name"]
                    stack.append((k["id"], rel_prefix + k["name"] + "/", top))
                else:
                    out.append({
                        "relPath": rel_prefix + k["name"],
                        "driveFolder": top_folder,
                        "system": system_map.get(top_folder, ""),
                        "id": k["id"],
                        "size": k["size"],
                        "md5": k["md5"],
                        "mtime": k["mtime"],
                    })
        out.sort(key=lambda d: d["relPath"].encode("utf-8"))
        return out


# ---------------------------------------------------------------- cache

def _put_str(buf, s):
    b = s.encode("utf-8")
    buf += struct.pack("<I", len(b))
    buf += b


def write_cache_bin(path, files):
    """Byte-identical to C++ saveDriveCache."""
    buf = bytearray()
    buf += MAGIC
    buf += struct.pack("<Q", len(files))
    for f in files:
        _put_str(buf, f["relPath"])
        _put_str(buf, f["driveFolder"])
        _put_str(buf, f["system"])
        _put_str(buf, f["id"])
        buf += struct.pack("<Q", f["size"])
        _put_str(buf, f["md5"])
        _put_str(buf, f["mtime"])
    with open(path, "wb") as fh:
        fh.write(bytes(buf))


def load_system_map(path):
    with open(path, "r", encoding="utf-8") as fh:
        j = json.load(fh)
    mapping = j.get("mapping", {})
    return {k: v for k, v in mapping.items()
            if k and not k.startswith("_") and isinstance(v, str)}


# ---------------------------------------------------------------- cli

def test_parse():
    cases = [
        ("https://drive.google.com/drive/folders/1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM",
         "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"),
        ("https://drive.google.com/drive/u/0/folders/1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM",
         "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"),
        ("https://drive.google.com/open?id=1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM",
         "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"),
        ("https://drive.google.com/drive/folders/1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM?usp=sharing",
         "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"),
        ("https://drive.google.com/drive/folders/1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM#list",
         "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"),
        ("1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM",
         "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"),
        ("https://drive.google.com/drive/folders/ABC_def-123", "ABC_def-123"),
        ("not a link!!!", ""),
        ("", ""),
        ("https://example.com/", ""),
        ("https://drive.google.com/drive/folders/short", ""),
    ]
    failed = 0
    for url, want in cases:
        got = parse_drive_folder_id(url)
        ok = got == want
        failed += not ok
        print("%s  %-90s -> %r" % ("PASS" if ok else "FAIL", url, got))
    print("%d/%d passed" % (len(cases) - failed, len(cases)))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", nargs="*", default=[],
                    help="Drive folder URLs or raw IDs")
    ap.add_argument("--key", default=os.environ.get("GS_DRIVE_API_KEY", ""),
                    help="API key (or GS_DRIVE_API_KEY env)")
    ap.add_argument("--base-url", default="https://www.googleapis.com")
    ap.add_argument("--map", default="",
                    help="path to system_map.json")
    ap.add_argument("--out", choices=["json", "bin"], default="json")
    ap.add_argument("--out-dir", default=".")
    ap.add_argument("--test-parse", action="store_true")
    args = ap.parse_args()

    if args.test_parse:
        return test_parse()

    if not args.roots:
        print("no --roots given", file=sys.stderr)
        return 2

    system_map = load_system_map(args.map) if args.map else {}
    client = DriveClient(args.key, args.base_url)
    all_files = []
    for root in args.roots:
        fid = parse_drive_folder_id(root)
        if not fid:
            print("cannot parse folder id: %r" % root, file=sys.stderr)
            return 3
        try:
            files = client.list_recursive(fid, system_map)
        except DriveError as e:
            print("ERROR http=%d %s" % (e.http_code, e.message),
                  file=sys.stderr)
            return 4
        all_files.extend(files)
        cache_path = os.path.join(args.out_dir, "drive_cache_%s.bin" % fid)
        write_cache_bin(cache_path, files)
        print("root %s: %d files -> %s" % (fid, len(files), cache_path))

    if args.out == "json":
        print(json.dumps(all_files, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
