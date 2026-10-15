#!/usr/bin/env python3
"""fixer_replica.py — Independent Python replica of the C++ Fixer module
(Trimui-GameSync P5: src/Fixer.h / src/Fixer.cpp).

Implements the SAME detection/grouping rules so the parent agent can
cross-check the C++ implementation against a real scan cache:
  - ext -> system table (lowercased ext, same order as C++)
  - ambiguous exts (.iso/.cso/.chd/.zip/.7z/.rar) -> WarnOnly
  - unknown ext -> skipped entirely; no ext -> WarnOnly
  - GB-in-GBC compatible -> WarnOnly (no move suggested)
  - multi-disc grouping: ext in {.cue,.bin,.m3u,.pbp}, same (system, dir,
    lowercased stem), >= 2 files -> one group, moves together
  - entries sorted by (system, relPath)

Reads the same binary scan cache as C++:
  magic "GSSCAN01", u64 count LE, per file:
    str system, str relPath, str name, u64 size, u64 mtime
  (str = u32 LE length + bytes)

Output format (must match tools/test_fixer_native exactly):
  COUNTS MOVE=<n> WARN=<n> GROUPS=<n> FILES=<n>
  M|<system>|<relPath>|<expected>|<size>
  G|<system>|<relDir>|<stem>|<expected>|<members>|<size>
  W|<system>|<relPath>|<ext>|<note>

Usage:
  python3 fixer_replica.py scan_cache.bin
  python3 fixer_replica.py --self-test
"""

import struct
import sys

EXT_SYSTEM = [
    (".nes", "FC"), (".fds", "FC"),
    (".sfc", "SFC"), (".smc", "SFC"),
    (".gb", "GB"), (".gbc", "GBC"), (".gba", "GBA"),
    (".md", "MD"), (".gen", "MD"), (".smd", "MD"),
    (".sms", "MS"), (".gg", "GG"),
    (".pce", "PCE"),
    (".a26", "ATARI2600"),
    (".nds", "NDS"),
    (".cue", "PS"), (".bin", "PS"), (".m3u", "PS"), (".pbp", "PS"),
]
EXT_MAP = dict(EXT_SYSTEM)
AMBIGUOUS = {".iso", ".cso", ".chd", ".zip", ".7z", ".rar"}
DISC_EXTS = {".cue", ".bin", ".m3u", ".pbp"}


# ---------------------------------------------------------------- cache IO

def _read_str(buf, off):
    (ln,) = struct.unpack_from("<I", buf, off)
    off += 4
    s = buf[off:off + ln].decode("utf-8")
    return s, off + ln


def load_scan_cache(path):
    buf = open(path, "rb").read()
    assert buf[:8] == b"GSSCAN01", "bad scan magic"
    (n,) = struct.unpack_from("<Q", buf, 8)
    off = 16
    files = []
    for _ in range(n):
        system, off = _read_str(buf, off)
        rel, off = _read_str(buf, off)
        name, off = _read_str(buf, off)
        (size,) = struct.unpack_from("<Q", buf, off)
        off += 8
        (mtime,) = struct.unpack_from("<Q", buf, off)
        off += 8
        files.append({"system": system, "relPath": rel, "name": name,
                      "size": size, "mtime": mtime})
    return files


# ---------------------------------------------------------------- logic

def ext_lower(name):
    p = name.rfind(".")
    if p < 0 or p + 1 >= len(name):
        return ""
    return name[p:].lower()


def parent_dir(rel):
    p = rel.rfind("/")
    return "" if p < 0 else rel[:p]


def stem_of(name, ext):
    if ext and len(name) > len(ext) and name[len(name) - len(ext):].lower() == ext:
        return name[:len(name) - len(ext)]
    return name


def is_port_data_folder(system):
    l = system.lower()
    return l == "ports" or l == "a30ports"


def find_misplaced(files):
    out = []       # dicts: system, relPath, name, ext, expected, note, kind,
                   #         isGroup, members, size
    cands = []
    for f in files:
        if is_port_data_folder(f["system"]):
            continue
        ext = ext_lower(f["name"])
        expected = EXT_MAP.get(ext, "")
        if not expected:
            if not ext or ext in AMBIGUOUS:
                out.append({"system": f["system"], "relPath": f["relPath"],
                            "name": f["name"], "ext": ext, "expected": "",
                            "note": ("Không có đuôi file — xem tay"
                                     if not ext else
                                     "Không xác định được hệ máy — xem tay"),
                            "kind": "W", "isGroup": False, "members": [],
                            "size": f["size"]})
            continue
        if f["system"] == expected:
            continue
        if expected == "GB" and f["system"] == "GBC":
            out.append({"system": f["system"], "relPath": f["relPath"],
                        "name": f["name"], "ext": ext, "expected": "",
                        "note": ("Chạy được ở thư mục này (%s→%s) — không cần move"
                                 % (expected, f["system"])),
                        "kind": "W", "isGroup": False, "members": [],
                        "size": f["size"]})
            continue
        cands.append({"system": f["system"], "relPath": f["relPath"],
                      "name": f["name"], "ext": ext, "expected": expected,
                      "note": "", "kind": "M", "isGroup": False, "members": [],
                      "size": f["size"],
                      "_dir": parent_dir(f["relPath"]),
                      "_stem": stem_of(f["name"], ext).lower()})

    cands.sort(key=lambda c: (c["system"], c["_dir"], c["_stem"], c["relPath"]))
    i = 0
    while i < len(cands):
        j = i + 1
        groupable = cands[i]["ext"] in DISC_EXTS
        while (j < len(cands) and groupable
               and cands[j]["ext"] in DISC_EXTS
               and cands[j]["system"] == cands[i]["system"]
               and cands[j]["_dir"] == cands[i]["_dir"]
               and cands[j]["_stem"] == cands[i]["_stem"]
               and cands[j]["expected"] == cands[i]["expected"]):
            j += 1
        if j - i >= 2:
            members = cands[i:j]
            d = members[0]["_dir"]
            stem = members[0]["_stem"]
            rel = stem if not d else d + "/" + stem
            out.append({"system": members[0]["system"], "relPath": rel,
                        "name": "%s.* (%d files)"
                                % (stem_of(members[0]["name"], members[0]["ext"]),
                                   len(members)),
                        "ext": "", "expected": members[0]["expected"], "note": "",
                        "kind": "M", "isGroup": True,
                        "members": members,
                        "size": sum(m["size"] for m in members)})
        else:
            c = dict(cands[i])
            del c["_dir"], c["_stem"]
            out.append(c)
        i = j

    out.sort(key=lambda e: (e["system"], e["relPath"]))
    return out


def summary(entries):
    n_move = sum(1 for e in entries if e["kind"] == "M")
    n_warn = sum(1 for e in entries if e["kind"] == "W")
    n_grp = sum(1 for e in entries if e["kind"] == "M" and e["isGroup"])
    lines = ["COUNTS MOVE=%d WARN=%d GROUPS=%d FILES=%d"
             % (n_move, n_warn, n_grp, len(entries))]
    for e in entries:
        if e["kind"] == "M" and e["isGroup"]:
            d = parent_dir(e["relPath"])
            stem = e["relPath"] if not d else e["relPath"][len(d) + 1:]
            lines.append("G|%s|%s|%s|%s|%d|%d"
                         % (e["system"], d, stem, e["expected"],
                            len(e["members"]), e["size"]))
        elif e["kind"] == "M":
            lines.append("M|%s|%s|%s|%d"
                         % (e["system"], e["relPath"], e["expected"], e["size"]))
        else:
            lines.append("W|%s|%s|%s|%s"
                         % (e["system"], e["relPath"], e["ext"], e["note"]))
    return "\n".join(lines) + "\n"


def self_test():
    files = [
        {"system": "SFC", "relPath": "SFC/zelda.nes", "name": "zelda.nes",
         "size": 100, "mtime": 0},
        {"system": "SFC", "relPath": "SFC/mario.sfc", "name": "mario.sfc",
         "size": 100, "mtime": 0},
        {"system": "GBC", "relPath": "GBC/tetris.gb", "name": "tetris.gb",
         "size": 100, "mtime": 0},
        {"system": "SFC", "relPath": "SFC/disc.cue", "name": "disc.cue",
         "size": 100, "mtime": 0},
        {"system": "SFC", "relPath": "SFC/disc.bin", "name": "disc.bin",
         "size": 200, "mtime": 0},
        {"system": "PS", "relPath": "PS/game.iso", "name": "game.iso",
         "size": 100, "mtime": 0},
    ]
    v = find_misplaced(files)
    assert sum(1 for e in v if e["kind"] == "M") == 2, v  # zelda + group
    assert sum(1 for e in v if e["kind"] == "W") == 2, v  # tetris + iso
    assert sum(1 for e in v if e["isGroup"]) == 1, v
    print("replica self-test OK")


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        self_test()
        return
    if len(sys.argv) != 2:
        print("usage: fixer_replica.py scan_cache.bin | --self-test")
        sys.exit(2)
    files = load_scan_cache(sys.argv[1])
    sys.stdout.write(summary(find_misplaced(files)))


if __name__ == "__main__":
    main()
