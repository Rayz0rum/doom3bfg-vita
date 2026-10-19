"""Measure text with the game's own font metrics (newfonts/<name>/48.dat)."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bimg

_cache = {}


def load(name):
    if name in _cache:
        return _cache[name]
    f, e = bimg.load(os.path.join(bimg.BASE, "_common.resources"))
    d = bimg.read(f, e["newfonts/%s/48.dat" % name])
    magic, point, asc, desc, n = struct.unpack_from(">IhhhH", d, 0)
    p = 12
    glyphs = []
    for i in range(n):
        w, h, top, left, skip, pad, s, t = struct.unpack_from("<BBbbBBHH", d, p)
        glyphs.append((w, h, top, left, skip))
        p += 10
    chars = struct.unpack_from("<%dI" % n, d, p)
    m = {c: glyphs[i] for i, c in enumerate(chars)}
    _cache[name] = (asc, desc, m)
    return _cache[name]


def width(name, text, height_px):
    """Pen advance of text drawn at height_px (the engine's scale = height/48)."""
    asc, desc, m = load(name)
    scale = height_px / 48.0
    star = m.get(ord("*"))
    return sum((m.get(ord(ch), star)[4]) for ch in text) * scale


def has(name, ch):
    return ord(ch) in load(name)[2]


if __name__ == "__main__":
    for name in ("sarasori_rg", "bankgothic_md_bt", "arial_narrow"):
        asc, desc, m = load(name)
        print(name, "ascender", asc, "descender", desc, "glyphs", len(m), "has degree", has(name, "\xb0"))
        for s in ("Primary Fire / Melee", "Quick-slot 1: Grenades", "Quick-slot 2: BFG 9000", "CONTROLLER LAYOUT"):
            print("   %-24s %6.1f px at 17 px" % (s, width(name, s, 17)))
