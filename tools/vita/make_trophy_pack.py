#!/usr/bin/env python3
"""Makes the Vita's trophy pack from the PS3 disc's (the user's own copy).

usage: make_trophy_pack.py <PS3 TROPHY.TRP>
   e.g. make_trophy_pack.py E:/PS3_GAME/TROPDIR/NPWR03554_00/TROPHY.TRP

Writes
  neo/sys/vita/sce_sys/trophy/DBFG00001_00/TROPHY.TRP  (packed into the VPK)
  neo/sys/vita/vita_trophy_list.inc                     (id, grade, hidden, English name)

The Vita takes a homebrew trophy pack the way Ghostship (Rinnegatamante's
Super Mario 64 port) ships one: with the NoTrpDrm plugin installed
(ux0:tai/NoTrpDrm.suprx under *main), which turns off the check of the
application's NP communication signature and of the pack's signature. The
pack itself is the PS3's container, unencrypted, with:
  - container version 2, and a SHA-1 of the whole file (that field zeroed)
    at 0x1C;
  - every entry's flags 0, its data 16-byte aligned;
  - in each .SFM: platform="psp2" on <trophyconf>, the port's own NP
    communication ID (DBFG00001_00 - never the PS3 set's NPWR03554_00, so it
    cannot be mistaken for the real set), and Sony's signature comment
    blanked with x's (the plugin skips the check).
The trophies, names, descriptions (6 languages), grades and icons are the
PS3 set's - 66 trophies, the same ids as the game's achievement_t.
"""
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VITA = os.path.join(HERE, "..", "..", "neo", "sys", "vita")
# four letters and five digits, as Sony's (NPWR03554) and every homebrew set (GHST00001,
# FFIV00001): the first round's D3BF00001 was refused by sceNpTrophyCreateContext (0x8055160C)
COMM_ID = "DBFG00001_00"


def read_trp(path):
    with open(path, "rb") as fp:
        data = fp.read()
    magic, version, size, count, esize = struct.unpack_from(">IIQII", data, 0)
    assert magic == 0xDCA24D00, "not a trophy pack"
    files = []
    for i in range(count):
        e = 0x40 + i * esize
        name = data[e:e + 32].split(b"\0")[0].decode("ascii")
        off, length, flags = struct.unpack_from(">QQI", data, e + 32)
        files.append((name, data[off:off + length]))
    return version, files


def vita_sfm(text):
    text = re.sub(r"Sce-Np-Trophy-Signature: ([0-9a-fA-F]+)", lambda m: "Sce-Np-Trophy-Signature: " + "x" * len(m.group(1)), text, count=1)
    text = re.sub(r"<npcommid>[^<]*</npcommid>", "<npcommid>%s</npcommid>" % COMM_ID, text, count=1)
    if 'platform="' not in text:
        text = text.replace('<trophyconf version="1.1"', '<trophyconf version="1.1" platform="psp2"', 1)
    assert 'platform="psp2"' in text and COMM_ID in text
    return text


def write_trp(path, files):
    count = len(files)
    pos = 0x40 + count * 0x40
    table = b""
    blobs = b""
    base = pos
    for name, blob in files:
        table += name.encode("ascii").ljust(32, b"\0") + struct.pack(">QQI", pos, len(blob), 0) + b"\0" * 12
        pad = (-len(blob)) % 16
        blobs += blob + b"\0" * pad
        pos += len(blob) + pad
    total = base + len(blobs)
    header = struct.pack(">IIQII", 0xDCA24D00, 2, total, count, 0x40) + b"\0" * 4
    header += b"\0" * (0x40 - len(header))
    data = bytearray(header + table + blobs)
    assert len(data) == total
    data[0x1C:0x30] = hashlib.sha1(bytes(data)).digest()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fp:
        fp.write(data)
    return total


def main():
    version, files = read_trp(sys.argv[1])
    out = []
    names = {}
    conf = None
    for name, blob in files:
        if name.upper().endswith(".SFM"):
            text = blob.decode("utf-8")
            text = vita_sfm(text)
            blob = text.encode("utf-8")
            if name.upper() == "TROPCONF.SFM":
                conf = text
            if name.upper() == "TROP.SFM":
                for m in re.finditer(r'<trophy id="(\d+)" hidden="(\w+)" ttype="(\w)"[^>]*>\s*<name>(.*?)</name>', text, re.S):
                    names[int(m.group(1))] = (m.group(3), m.group(2) == "yes", m.group(4))
        out.append((name, blob))
    assert conf is not None and len(names) >= 66, "expected the 66 trophies, found %d" % len(names)
    trp = os.path.join(VITA, "sce_sys", "trophy", COMM_ID, "TROPHY.TRP")
    size = write_trp(trp, out)
    print("%s: %d files, %d bytes (PS3 pack version %d -> 2)" % (trp, len(out), size, version))
    grades = {"P": 0, "G": 0, "S": 0, "B": 0}
    with open(os.path.join(VITA, "vita_trophy_list.inc"), "w", encoding="utf-8", newline="\n") as fp:
        fp.write("// tools/vita/make_trophy_pack.py: the trophies of the pack (PS3 set NPWR03554_00 as %s)\n" % COMM_ID)
        fp.write("// { id, grade (P G S B), hidden, English name } - the ids are achievement_t's\n")
        for i in sorted(names):
            g, hidden, n = names[i]
            grades[g] += 1
            n = n.replace("&amp;", "&").replace("&apos;", "'").replace("&quot;", '"')
            fp.write('{ %d, \'%s\', %s, "%s" },\n' % (i, g, "true" if hidden else "false", n.replace("\\", "\\\\").replace('"', '\\"')))
    print("vita_trophy_list.inc: %d trophies - %d platinum, %d gold, %d silver, %d bronze" % (len(names), grades["P"], grades["G"], grades["S"], grades["B"]))


if __name__ == "__main__":
    main()
