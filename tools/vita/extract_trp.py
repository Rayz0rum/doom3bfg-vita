#!/usr/bin/env python3
"""Unpacks a PS3 trophy pack (TROPHY.TRP: unencrypted on the PS3 disc).

usage: extract_trp.py <TROPHY.TRP> <output folder>

The container: a 64-byte header (magic DC A2 4D 00, version, file size,
entry count, entry size) then one 64-byte entry per file (name[32], offset
u64, size u64, flags u32, 12 bytes padding), all big-endian. The files:
TROPCONF.SFM (the trophy list, XML), TROP.SFM / TROP_xx.SFM (names and
descriptions, per language, XML), ICON0.PNG (the set), TROPnnn.PNG (each
trophy) and ICON0/PIC... images.
"""
import os
import struct
import sys


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    with open(src, "rb") as fp:
        data = fp.read()
    magic, version, size, count, esize = struct.unpack_from(">IIQII", data, 0)
    if magic != 0xDCA24D00:
        sys.exit("not a trophy pack (magic %08X)" % magic)
    print("TRP version %d, %d bytes, %d files" % (version, size, count))
    for i in range(count):
        e = 0x40 + i * esize
        name = data[e:e + 32].split(b"\0")[0].decode("ascii")
        off, length, flags = struct.unpack_from(">QQI", data, e + 32)
        with open(os.path.join(out, name), "wb") as o:
            o.write(data[off:off + length])
        print("  %-16s %8d bytes  flags %d" % (name, length, flags))


if __name__ == "__main__":
    main()
