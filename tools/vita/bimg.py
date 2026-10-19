"""Read BFG .resources containers and decode .bimage textures to PNG."""
import struct, zlib, os, sys

BASE = r"C:\Users\rayan\OneDrive\Desktop\DOOM3\base"


def load(path):
    f = open(path, "rb")
    magic, toff, tlen = struct.unpack(">III", f.read(12))
    f.seek(toff)
    t = f.read(tlen)
    p = 0
    n = struct.unpack_from(">i", t, p)[0]
    p += 4
    ents = {}
    for _ in range(n):
        sl = struct.unpack_from("<i", t, p)[0]
        p += 4
        name = t[p:p + sl].decode("latin1")
        p += sl
        off, ln = struct.unpack_from(">ii", t, p)
        p += 8
        ents[name.replace("\\", "/").lower()] = (off, ln)
    return f, ents


def read(f, ent):
    f.seek(ent[0])
    return f.read(ent[1])


def bimage(d):
    t, magic, ttype, fmt, cfm, w, h, nl = struct.unpack_from(">qiiiiiii", d, 0)
    p = struct.calcsize(">qiiiiiii")
    levels = []
    for _ in range(nl * (6 if ttype == 2 else 1)):
        lv, dz, lw, lh, ds = struct.unpack_from(">iiiii", d, p)
        p += 20
        levels.append((lv, lw, lh, d[p:p + ds]))
        p += ds
    return dict(fmt=fmt, cfm=cfm, w=w, h=h, levels=levels)


def c565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def dxt_colour(b, four):
    c0 = b[0] | (b[1] << 8)
    c1 = b[2] | (b[3] << 8)
    p0, p1 = c565(c0), c565(c1)
    if c0 > c1 or four:
        p2 = tuple((2 * a + c) // 3 for a, c in zip(p0, p1))
        p3 = tuple((a + 2 * c) // 3 for a, c in zip(p0, p1))
        a3 = 255
    else:
        p2 = tuple((a + c) // 2 for a, c in zip(p0, p1))
        p3 = (0, 0, 0)
        a3 = 0
    pal = [p0 + (255,), p1 + (255,), p2 + (255,), p3 + (a3,)]
    idx = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24)
    return [pal[(idx >> (2 * t)) & 3] for t in range(16)]


def dxt_alpha(b):
    a0, a1 = b[0], b[1]
    if a0 > a1:
        pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    bits = int.from_bytes(b[2:8], "little")
    return [pal[(bits >> (3 * t)) & 7] for t in range(16)]


def decode(img, level=0):
    lv, w, h, data = img["levels"][level]
    fmt = img["fmt"]
    px = bytearray(w * h * 4)
    if fmt in (7, 8):
        bs = 8 if fmt == 7 else 16
        bw = (w + 3) // 4
        bh = (h + 3) // 4
        for by in range(bh):
            for bx in range(bw):
                o = (by * bw + bx) * bs
                if fmt == 7:
                    cols = dxt_colour(data[o:o + 8], False)
                    alph = [c[3] for c in cols]
                else:
                    alph = dxt_alpha(data[o:o + 8])
                    cols = dxt_colour(data[o + 8:o + 16], True)
                for t in range(16):
                    x = bx * 4 + (t & 3)
                    y = by * 4 + (t >> 2)
                    if x < w and y < h:
                        q = (y * w + x) * 4
                        px[q:q + 3] = bytes(cols[t][:3])
                        px[q + 3] = alph[t]
    elif fmt == 1:  # RGBA8
        px[:] = data[:w * h * 4]
    elif fmt == 12:  # RGB565 big-endian
        for i in range(w * h):
            c = (data[2 * i] << 8) | data[2 * i + 1]
            px[i * 4:i * 4 + 3] = bytes(c565(c))
            px[i * 4 + 3] = 255
    else:
        raise ValueError("fmt %d" % fmt)
    return w, h, px


def png(path, w, h, px):
    raw = b"".join(b"\x00" + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                          + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    f, e = load(os.path.join(BASE, "_common.resources"))
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for k in sys.argv[2:]:
        img = bimage(read(f, e[k]))
        w, h, px = decode(img)
        name = k.split("/")[-1].replace("#", "_").replace(".bimage", ".png")
        png(os.path.join(out, name), w, h, px)
        print(k, "fmt", img["fmt"], "cfm", img["cfm"], w, "x", h)
