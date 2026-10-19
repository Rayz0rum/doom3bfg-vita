#!/usr/bin/env python3
"""Drop the relocations vita-elf-create has no use for from a linked Vita ELF.

The Windows vita-elf-create in this VitaSDK corrupts its heap and exits (MSYS
exit 127, no message) once an ELF carries more than ~284k relocations, and the
DOOM 3 BFG ELF sits right under that. Close to half of them are Thumb/ARM
branch relocations (R_ARM_THM_CALL, R_ARM_THM_JUMP24, ...): PC-relative,
always from code to code inside the text segment, so the module loader never
has to touch them - vita-elf-create reads them and emits nothing for them.
R_ARM_NONE entries are the same. vita-elf-create does turn the branches into
loader relocations, but a PC-relative branch between two addresses of one
segment resolves to the same bits wherever the segment is loaded, so those
only rewrote instructions with what they already held. Checked when this was
added (2026-10-17, 279,639 -> 168,959 relocations): both .velf files' loadable
segments were identical but for the one byte of the input file name, and every
dropped branch had both ends in the text segment except three calls to weak
undefined unwinder hooks (__register_frame_info & co.), which never run.

usage: strip_branch_relocs.py <elf> [--dry-run]

Edits the file in place: in each non-debug SHT_REL section the kept entries
are packed to the front and sh_size is shrunk; the bytes left behind are
zeroed and simply no longer part of the section.
"""
import struct
import sys

SHT_REL = 9
# R_ARM_NONE, R_ARM_CALL, R_ARM_JUMP24, R_ARM_THM_CALL, R_ARM_THM_JUMP24,
# R_ARM_THM_JUMP19, R_ARM_THM_JUMP11 (THM_PC11), R_ARM_THM_JUMP8 (THM_PC8)
DROP = {0, 28, 29, 10, 30, 51, 102, 103}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    dry = "--dry-run" in sys.argv
    d = bytearray(open(path, "rb").read())
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        print("strip_branch_relocs: %s is not a little-endian ELF32" % path)
        return 1
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    sh = [struct.unpack_from("<10I", d, shoff + i * shentsize) for i in range(shnum)]
    stroff = sh[shstrndx][4]

    def name(s):
        o = stroff + s[0]
        return d[o:d.index(b"\0", o)].decode()

    before = after = 0
    for i, s in enumerate(sh):
        if s[1] != SHT_REL:
            continue
        n = name(s)
        off, size, entsize = s[4], s[5], s[9] or 8
        count = size // entsize
        if "debug" in n:
            continue
        kept = []
        for k in range(count):
            r_offset, r_info = struct.unpack_from("<II", d, off + k * entsize)
            if (r_info & 0xFF) not in DROP:
                kept.append((r_offset, r_info))
        before += count
        after += len(kept)
        print("strip_branch_relocs: %-18s %7d -> %7d" % (n, count, len(kept)))
        if dry:
            continue
        for k, (a, b) in enumerate(kept):
            struct.pack_into("<II", d, off + k * entsize, a, b)
        d[off + len(kept) * entsize:off + size] = bytes(size - len(kept) * entsize)
        struct.pack_into("<I", d, shoff + i * shentsize + 0x14, len(kept) * entsize)
    print("strip_branch_relocs: non-debug relocations %d -> %d%s" % (before, after, " (dry run)" if dry else ""))
    if not dry:
        open(path, "wb").write(d)
    return 0


if __name__ == "__main__":
    sys.exit(main())
