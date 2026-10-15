#!/usr/bin/env python3
"""sync_compare_replica.py — Independent Python replica of the C++ Sync module
(Trimui-GameSync P4: src/Sync.h / src/Sync.cpp compare logic).

Implements the SAME compare/grouping rules so the parent agent can cross-check
the C++ implementation against real cache files:
  - join key: (system, path-within-system); remote strips the first component
    of the Drive relPath (the Drive folder name); local strips the system
    prefix when system is non-empty
  - statuses: SYNCED (same name+size), REMOTE_ONLY, LOCAL_ONLY, DIFFER
  - multi-disc grouping: extensions {.cue,.bin,.m3u,.pbp} (case-insensitive),
    same (system, dir, stem), >= 2 files -> one group row
  - rows sorted by (system, relPath); group members sorted by name

Reads the same binary caches as C++:
  - scan cache: magic "GSSCAN01", u64 count LE, per file:
    str system, str relPath, str name, u64 size, u64 mtime
  - drive cache: magic "GSDRV001", u64 count LE, per file:
    str relPath, str driveFolder, str system, str id, u64 size, str md5, str mtime
  (str = u32 LE length + bytes)

Output format (must match tools/test_sync_native exactly):
  COUNTS SYNCED=<n> REMOTE_ONLY=<n> LOCAL_ONLY=<n> DIFFER=<n> ROWS=<n>
  F|STATUS|system|relPath|localSize|remoteSize
  G|STATUS|system|relPath|nMembers|localSize|remoteSize
  M|STATUS|system|relPath|localSize|remoteSize      (group members)

Usage:
  python3 sync_compare_replica.py scan_cache.bin drive_cache_1.bin [drive_cache_2.bin ...]
  python3 sync_compare_replica.py --self-test
"""

import struct
import sys

GROUP_EXTS = {"cue", "bin", "m3u", "pbp"}


# ---------------------------------------------------------------- cache IO

def _read_str(buf, off):
    (ln,) = struct.unpack_from("<I", buf, off)
    off += 4
    s = buf[off:off + ln].decode("utf-8")
    return s, off + ln


def load_scan_cache(path):
    buf = open(path, "rb").read()
    assert buf[:8] == b"GSSCAN01", "bad scan magic"
    (count,) = struct.unpack_from("<Q", buf, 8)
    off = 16
    files = []
    for _ in range(count):
        system, off = _read_str(buf, off)
        rel, off = _read_str(buf, off)
        name, off = _read_str(buf, off)
        size, mtime = struct.unpack_from("<QQ", buf, off)
        off += 16
        files.append((system, rel, name, size))
    return files


def load_drive_cache(path):
    buf = open(path, "rb").read()
    assert buf[:8] == b"GSDRV001", "bad drive magic"
    (count,) = struct.unpack_from("<Q", buf, 8)
    off = 16
    files = []
    for _ in range(count):
        rel, off = _read_str(buf, off)
        _folder, off = _read_str(buf, off)
        system, off = _read_str(buf, off)
        fid, off = _read_str(buf, off)
        (size,) = struct.unpack_from("<Q", buf, off)
        off += 8
        md5, off = _read_str(buf, off)
        _mtime, off = _read_str(buf, off)
        files.append((system, rel, fid, size, md5))
    return files


# ---------------------------------------------------------------- compare

def _basename(p):
    return p.rsplit("/", 1)[-1]


def _dirname(p):
    i = p.rfind("/")
    return "" if i == -1 else p[:i]


def _stem(name):
    d = name.rfind(".")
    if d == -1 or d == 0:
        return name
    return name[:d]


def _ext(name):
    d = name.rfind(".")
    if d == -1 or d + 1 >= len(name):
        return ""
    return name[d + 1:].lower()


def _pis_local(system, rel):
    if system:
        p = rel.find("/")
        if p != -1:
            return rel[p + 1:]
    return rel


def _pis_remote(rel):
    p = rel.find("/")
    if p != -1:
        return rel[p + 1:]
    return rel


def compare(local_files, remote_files):
    sides = {}
    for system, rel, _name, size in local_files:
        k = (system, _pis_local(system, rel))
        s = sides.setdefault(k, {})
        s["hasLocal"] = True
        s["localSize"] = size
    for system, rel, fid, size, md5 in remote_files:
        k = (system, _pis_remote(rel))
        s = sides.setdefault(k, {})
        s["hasRemote"] = True
        s["remoteSize"] = size
        s["remoteId"] = fid
        s["remoteMd5"] = md5

    singles = []
    for (system, pis), s in sides.items():
        e = {
            "system": system,
            "relPath": pis,
            "name": _basename(pis),
            "localSize": s.get("localSize", 0),
            "remoteSize": 0,
            "remoteId": s.get("remoteId", ""),
            "remoteMd5": s.get("remoteMd5", ""),
            "isGroup": False,
            "members": [],
        }
        if s.get("hasLocal") and s.get("hasRemote"):
            if s["localSize"] == s["remoteSize"]:
                e["status"] = "SYNCED"
            else:
                e["status"] = "DIFFER"
                e["remoteSize"] = s["remoteSize"]
        elif s.get("hasRemote"):
            e["status"] = "REMOTE_ONLY"
            e["remoteSize"] = s["remoteSize"]
        else:
            e["status"] = "LOCAL_ONLY"
        singles.append(e)

    groups = {}
    for i, e in enumerate(singles):
        if _ext(e["name"]) not in GROUP_EXTS:
            continue
        gk = (e["system"], _dirname(e["relPath"]), _stem(e["name"]))
        groups.setdefault(gk, []).append(i)

    grouped = set()
    rows = []
    for (system, d, stem), idxs in groups.items():
        if len(idxs) < 2:
            continue
        members = sorted((singles[i] for i in idxs),
                         key=lambda e: e["name"])
        for i in idxs:
            grouped.add(i)
        statuses = {m["status"] for m in members}
        if statuses == {"SYNCED"}:
            gst = "SYNCED"
        elif statuses == {"REMOTE_ONLY"}:
            gst = "REMOTE_ONLY"
        elif statuses == {"LOCAL_ONLY"}:
            gst = "LOCAL_ONLY"
        else:
            gst = "DIFFER"
        rows.append({
            "system": system,
            "relPath": (d + "/" + stem) if d else stem,
            "name": stem,
            "localSize": sum(m["localSize"] for m in members),
            "remoteSize": sum(m["remoteSize"] for m in members
                             if m["status"] in ("REMOTE_ONLY", "DIFFER")),
            "remoteId": "",
            "remoteMd5": "",
            "status": gst,
            "isGroup": True,
            "members": members,
        })
    for i, e in enumerate(singles):
        if i not in grouped:
            rows.append(e)
    rows.sort(key=lambda e: (e["system"], e["relPath"]))
    return rows


# ---------------------------------------------------------------- output

def print_summary(rows):
    counts = {"SYNCED": 0, "REMOTE_ONLY": 0, "LOCAL_ONLY": 0, "DIFFER": 0}
    for e in rows:
        counts[e["status"]] += 1
    print("COUNTS SYNCED=%d REMOTE_ONLY=%d LOCAL_ONLY=%d DIFFER=%d ROWS=%d" % (
        counts["SYNCED"], counts["REMOTE_ONLY"], counts["LOCAL_ONLY"],
        counts["DIFFER"], len(rows)))
    for e in rows:
        if e["isGroup"]:
            print("G|%s|%s|%s|%d|%d|%d" % (
                e["status"], e["system"], e["relPath"], len(e["members"]),
                e["localSize"], e["remoteSize"]))
            for m in e["members"]:
                print("M|%s|%s|%s|%d|%d" % (
                    m["status"], m["system"], m["relPath"],
                    m["localSize"], m["remoteSize"]))
        else:
            print("F|%s|%s|%s|%d|%d" % (
                e["status"], e["system"], e["relPath"],
                e["localSize"], e["remoteSize"]))


def self_test():
    # statuses
    loc = [("GBA", "GBA/a.zip", "a.zip", 100),
           ("GBA", "GBA/b.zip", "b.zip", 100),
           ("GBA", "GBA/c.zip", "c.zip", 100),
           ("GBA", "GBA/d.zip", "d.zip", 100)]
    rem = [("GBA", "Nintendo - Game Boy Advance/a.zip", "id1", 100, "m1"),
           ("GBA", "Nintendo - Game Boy Advance/b.zip", "id2", 200, "m2"),
           ("GBA", "Nintendo - Game Boy Advance/e.zip", "id3", 300, "m3")]
    rows = compare(loc, rem)
    got = {e["relPath"]: e["status"] for e in rows}
    assert got == {"a.zip": "SYNCED", "b.zip": "DIFFER", "c.zip": "LOCAL_ONLY",
                   "d.zip": "LOCAL_ONLY", "e.zip": "REMOTE_ONLY"}, got
    # multi-disc grouping: mixed statuses -> DIFFER group
    loc = [("PS", "PS/g.cue", "g.cue", 10)]
    rem = [("PS", "Sony - PS1/g.cue", "i1", 10, ""),
           ("PS", "Sony - PS1/g.bin", "i2", 500, "")]
    rows = compare(loc, rem)
    assert len(rows) == 1 and rows[0]["isGroup"], rows
    assert rows[0]["status"] == "DIFFER", rows[0]["status"]
    assert rows[0]["remoteSize"] == 500, rows[0]["remoteSize"]
    assert [m["status"] for m in rows[0]["members"]] == ["REMOTE_ONLY", "SYNCED"]
    # all synced group
    loc = [("PS", "PS/h.cue", "h.cue", 10), ("PS", "PS/h.bin", "h.bin", 500)]
    rem = [("PS", "Sony - PS1/h.cue", "i1", 10, ""),
           ("PS", "Sony - PS1/h.bin", "i2", 500, "")]
    rows = compare(loc, rem)
    assert len(rows) == 1 and rows[0]["status"] == "SYNCED"
    assert rows[0]["remoteSize"] == 0
    # singleton groupable ext stays individual
    loc = []
    rem = [("PS", "Sony - PS1/solo.cue", "i9", 10, "")]
    rows = compare(loc, rem)
    assert len(rows) == 1 and not rows[0]["isGroup"]
    # unmapped ("", root) join
    loc = [("", "loose.zip", "loose.zip", 42)]
    rem = [("", "loose.zip", "ix", 42, "")]
    rows = compare(loc, rem)
    assert len(rows) == 1 and rows[0]["status"] == "SYNCED"
    print("SELF-TEST OK")


def main(argv):
    if argv[:1] == ["--self-test"]:
        self_test()
        return 0
    if len(argv) < 2:
        print(__doc__)
        return 2
    local_files = load_scan_cache(argv[0])
    remote_files = []
    for p in argv[1:]:
        remote_files.extend(load_drive_cache(p))
    print_summary(compare(local_files, remote_files))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
