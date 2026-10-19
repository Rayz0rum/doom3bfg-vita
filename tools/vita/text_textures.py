"""Finds the textures with text on them (signs, labels, panels, posters,
screens baked into the walls) so the Vita keeps them at full resolution while
every other world texture is shrunk (neo/renderer/Image_load.cpp,
R_VitaSkipLevels).

  python text_textures.py [--montage DIR]   scan every .bimage in the game
        data, write neo/renderer/vita_text_textures.inc (the protected
        texture stems) and text_textures_report.csv beside it; --montage writes
        contact sheets of what was protected / left out, to check by eye.

What it measures, on the luminance of the full-size level (DXT1/DXT5/YCoCg
decoded here, the same data the game uploads):
  - fine detail: what halving the texture destroys - the share of pixels that
    differ strongly from the half-size level scaled back up. Lettering is
    nearly all fine, high-contrast detail; most surfaces keep their look at
    half size.
  - text structure: lettering sits in rows - the edge density per row
    alternates between lines of text and the gaps between them - and has
    strokes in both directions; grain, rust and noise spread evenly.
  - the name: signs, labels, posters, screens, decals with words... help the
    measure decide; a name alone does not protect a texture whose pixels show
    no fine detail, and pixels alone need clearer evidence.
A stem (the texture's path without _d/_s/_local/... suffixes) is protected
as a whole, so a sign's normal and specular maps keep their full size with
its diffuse map.

Needs numpy (and Pillow for --montage).
"""
import os, re, sys, struct, glob, csv
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bimg  # noqa: E402

OUT_INC = os.path.join(HERE, "..", "..", "neo", "renderer", "vita_text_textures.inc")
OUT_CSV = os.path.join(HERE, "text_textures_report.csv")

FMT_RGBA8, FMT_XRGB8, FMT_ALPHA, FMT_L8A8, FMT_LUM8, FMT_INT8, FMT_DXT1, FMT_DXT5 = 1, 2, 3, 4, 5, 6, 7, 8
FMT_RGB565 = 12
CFM_DEFAULT, CFM_NORMAL_DXT5, CFM_YCOCG_DXT5, CFM_GREEN_ALPHA = 0, 1, 2, 3

NAME_HINTS = re.compile(r"sign|text|label|letter|word|poster|font|number|digit|caution|warn|danger|notice|exit|logo|uac|"
                        r"screen|monitor|lcd|readout|display|chart|schematic|diagram|placard|plaque|sticker|stencil|"
                        r"keypad|newspaper|paper|book|binder|magazine|calendar|clock|marquee|directory|gui|menu|"
                        r"restroom|washroom.*sign|arrow|hazard|radiation|biohazard|nosmok|wanted|ad_|advert|billboard", re.I)

SUFFIX = re.compile(r"(_d|_s|_h|_b|_bmp|_local|_norm|_n|_diffuse|_spec|_specular|_add|_glow|_fx|_df|_dif|_hb|_disp|_alpha|_a)$")


def unpack565(c):
    r = ((c >> 11) & 31).astype(np.float32) * (255.0 / 31)
    g = ((c >> 5) & 63).astype(np.float32) * (255.0 / 63)
    b = (c & 31).astype(np.float32) * (255.0 / 31)
    return np.stack([r, g, b], -1)


def dxt_colour_blocks(blk, four):
    # blk: (N, 8) uint8 -> (N, 16, 3)
    c0 = blk[:, 0].astype(np.uint32) | (blk[:, 1].astype(np.uint32) << 8)
    c1 = blk[:, 2].astype(np.uint32) | (blk[:, 3].astype(np.uint32) << 8)
    p0, p1 = unpack565(c0), unpack565(c1)
    third = (c0 > c1) | four
    p2 = np.where(third[:, None], (2 * p0 + p1) / 3, (p0 + p1) / 2)
    p3 = np.where(third[:, None], (p0 + 2 * p1) / 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)  # N,4,3
    idx = blk[:, 4:8].astype(np.uint32)
    bits = idx[:, 0] | (idx[:, 1] << 8) | (idx[:, 2] << 16) | (idx[:, 3] << 24)
    sel = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))[None, :]) & 3
    return np.take_along_axis(pal, sel[:, :, None].astype(np.int64).repeat(3, 2), 1)


def dxt5_alpha_blocks(blk):
    a0 = blk[:, 0].astype(np.float32)
    a1 = blk[:, 1].astype(np.float32)
    k = np.arange(1, 7, dtype=np.float32)
    big = a0 > a1
    pal = np.zeros((blk.shape[0], 8), np.float32)
    pal[:, 0], pal[:, 1] = a0, a1
    for i in range(6):
        pal[:, 2 + i] = np.where(big, ((6 - i) * a0 + (1 + i) * a1) / 7, 0)
    for i in range(4):
        pal[:, 2 + i] = np.where(big, pal[:, 2 + i], ((4 - i) * a0 + (1 + i) * a1) / 5)
    pal[:, 6] = np.where(big, pal[:, 6], 0)
    pal[:, 7] = np.where(big, pal[:, 7], 255)
    bits = np.zeros(blk.shape[0], np.uint64)
    for i in range(6):
        bits |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    sel = (bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))[None, :]) & np.uint64(7)
    return np.take_along_axis(pal, sel.astype(np.int64), 1)


def blocks_to_image(px, w, h, ch):
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    px = px.reshape(bh, bw, 4, 4, ch).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, ch)
    return px[:h, :w]


def decode(img):
    """-> (luminance HxW float 0..1, alpha HxW or None)"""
    lv, w, h, data = img["levels"][0]
    fmt, cfm = img["fmt"], img["cfm"]
    raw = np.frombuffer(data, np.uint8)
    if fmt == FMT_DXT1:
        blk = raw.reshape(-1, 8)
        rgb = blocks_to_image(dxt_colour_blocks(blk, np.zeros(blk.shape[0], bool)), w, h, 3)
        if cfm == CFM_GREEN_ALPHA:
            return None, None
        return rgb.mean(-1) / 255.0, None
    if fmt == FMT_DXT5:
        blk = raw.reshape(-1, 16)
        a = blocks_to_image(dxt5_alpha_blocks(blk[:, :8])[:, :, None], w, h, 1)[..., 0] / 255.0
        if cfm == CFM_YCOCG_DXT5:
            return a, None  # Y lives in alpha
        if cfm == CFM_NORMAL_DXT5:
            return None, None
        rgb = blocks_to_image(dxt_colour_blocks(blk[:, 8:], np.ones(blk.shape[0], bool)), w, h, 3)
        return rgb.mean(-1) / 255.0, a
    if fmt in (FMT_RGBA8, FMT_XRGB8) and len(raw) >= w * h * 4:
        p = raw[:w * h * 4].reshape(h, w, 4).astype(np.float32)
        return p[..., :3].mean(-1) / 255.0, p[..., 3] / 255.0
    if fmt in (FMT_LUM8, FMT_INT8, FMT_ALPHA) and len(raw) >= w * h:
        return raw[:w * h].reshape(h, w).astype(np.float32) / 255.0, None
    return None, None


def measure(L, A):
    h, w = L.shape
    if w < 32 or h < 32:
        return None
    # big ones: measure on a 1024 window budget (same per-pixel statistics)
    if w * h > 1024 * 1024:
        L = L[: (h // 2) * 2, : (w // 2) * 2]
    if A is not None:
        # where it is transparent the colour is meaningless: flatten it
        L = np.where(A > 0.1, L, L.mean())
    h2, w2 = (L.shape[0] // 2) * 2, (L.shape[1] // 2) * 2
    L = L[:h2, :w2]
    half = L.reshape(h2 // 2, 2, w2 // 2, 2).mean((1, 3))
    up = np.repeat(np.repeat(half, 2, 0), 2, 1)
    lost = np.abs(L - up)
    fine = float((lost > 0.10).mean())          # share of pixels a halving visibly changes
    strong = float((lost > 0.20).mean())
    # edges, both directions
    gx = np.abs(np.diff(L, axis=1))[:-1, :]
    gy = np.abs(np.diff(L, axis=0))[:, :-1]
    ex = gx > 0.18
    ey = gy > 0.18
    edges = ex | ey
    density = float(edges.mean())
    both = float(min(ex.mean(), ey.mean()) / max(1e-6, max(ex.mean(), ey.mean())))
    # rows of text: edge density per row, varying strongly at a short period
    rows = edges.mean(1)
    rowcv = float(rows.std() / (rows.mean() + 1e-6))
    cols = edges.mean(0)
    colcv = float(cols.std() / (cols.mean() + 1e-6))
    # contrast of the strokes: bimodality of the luminance around edges
    contrast = float(np.percentile(L, 95) - np.percentile(L, 5))
    # labels on a big prop atlas cover a corner of it: the same tests in
    # 64x64 tiles, counting the tiles that look like lettering
    T = 64
    th, tw = L.shape[0] // T, L.shape[1] // T
    tiles = 0
    if th > 0 and tw > 0:
        Lt = L[:th * T, :tw * T].reshape(th, T, tw, T).transpose(0, 2, 1, 3)
        lt = lost[:th * T, :tw * T].reshape(th, T, tw, T).transpose(0, 2, 1, 3)
        exT = np.abs(np.diff(Lt, axis=3))[:, :, :-1, :] > 0.18
        eyT = np.abs(np.diff(Lt, axis=2))[:, :, :, :-1] > 0.18
        edT = exT | eyT
        fineT = (lt > 0.10).mean((2, 3))
        strongT = (lt > 0.20).mean((2, 3))
        bx, by = exT.mean((2, 3)), eyT.mean((2, 3))
        bothT = np.minimum(bx, by) / np.maximum(1e-6, np.maximum(bx, by))
        r = edT.mean(3)
        rowT = r.std(2) / (r.mean(2) + 1e-6)
        c = edT.mean(2)
        colT = c.std(2) / (c.mean(2) + 1e-6)
        conT = np.percentile(Lt.reshape(th, tw, -1), 95, axis=2) - np.percentile(Lt.reshape(th, tw, -1), 5, axis=2)
        ok = (fineT > 0.12) & (strongT > 0.04) & (bothT > 0.35) & (conT > 0.35) & (rowT > 0.45) & (colT > 0.30)
        tiles = int(ok.sum())
    return dict(fine=fine, strong=strong, density=density, both=both, rowcv=rowcv, colcv=colcv, contrast=contrast, tiles=tiles)


# picked by hand (reported on the console): lettering the measure misses -
# the information kiosks' glowing "INFORMATION" lives in an additive glow
# layer, soft by design, so its fine detail scores low
MANUAL_STEMS = [
    "models/mapobjects/kiosk/infokiosk2",
    "models/mapobjects/kiosk/infokiosk2fx",
    "models/mapobjects/deltakiosk/deltakiosk",
    "textures/decals/military_logo",  # the UAC / UNION AEROSPACE wall logo (mars_city2)
]


def stem_of(name):
    n = name[len("generated/images/"):] if name.startswith("generated/images/") else name
    n = re.sub(r"#__\d{4}\.bimage$", "", n)
    m = re.match(r"(addnormals|heightmap|makealpha|makeintensity|downsize|scale|invertalpha|invertcolor|smoothnormals)/(.*)$", n)
    if m:
        rest = m.group(2)
        # the first texture path inside the program
        p = re.match(r"((?:textures|models|gfx|particles|guis|lights)/.*?)(?:/(?:addnormals|heightmap|\d+)|$)", rest)
        n = p.group(1) if p else rest
    n = n.lower()
    for _ in range(2):
        n = SUFFIX.sub("", n)
    return n


def score(m, name):
    hint = bool(NAME_HINTS.search(name))
    s = 0.0
    s += min(m["fine"] / 0.10, 2.0) * 1.0          # 10% of pixels changed by a halving = 1 point
    s += min(m["strong"] / 0.04, 2.0) * 0.8
    s += min(m["rowcv"] / 0.6, 1.5) * 0.6 if m["density"] > 0.02 else 0
    s += min(m["both"] / 0.6, 1.0) * 0.3 if m["density"] > 0.02 else 0
    s += 0.6 if m["contrast"] > 0.45 else 0
    return s, hint


def is_text(s, hint, m):
    if m["tiles"] >= (2 if hint else 3):
        return True
    if m["density"] < 0.015 or m["fine"] < 0.02:
        return False
    return s >= (2.0 if hint else 3.0)


def main():
    montage = None
    if "--montage" in sys.argv:
        montage = sys.argv[sys.argv.index("--montage") + 1]
        os.makedirs(montage, exist_ok=True)
    files = sorted(glob.glob(bimg.BASE + "/*.resources")) + sorted(glob.glob(bimg.BASE + "/maps/**/*.resources", recursive=True))
    seen = {}
    for p in files:
        f, ents = bimg.load(p)
        for n, e in ents.items():
            if n.endswith(".bimage") and n not in seen:
                seen[n] = (p, e)
        f.close()
    rows = []
    stems = {}
    samples_yes, samples_no, samples_tiles = [], [], []
    handles = {}
    for i, (n, (p, e)) in enumerate(sorted(seen.items())):
        m = re.search(r"#__(\d\d)(\d\d)\.bimage$", n)
        # colour maps decide (specular maps are noisy); a stem protects all its maps
        if not m or m.group(2) != "00" or m.group(1) not in ("01", "02"):
            continue
        if n.startswith(("generated/images/guis/", "generated/images/ui/", "generated/images/gui/", "generated/images/fonts/", "generated/images/lights/")) or "/swf/" in n:
            continue  # never shrunk anyway (interface art)
        if p not in handles:
            handles[p] = open(p, "rb")
        fh = handles[p]
        fh.seek(e[0])
        try:
            img = bimg.bimage(fh.read(e[1]))
            if img["levels"][0][1] < 32:
                continue
            L, A = decode(img)
        except Exception as ex:  # noqa
            print("skip", n, ex)
            continue
        if L is None:
            continue
        mm = measure(L, A)
        if mm is None:
            continue
        s, hint = score(mm, n)
        t = is_text(s, hint, mm)
        st = stem_of(n)
        rows.append([n, st, img["w"], img["h"], "%.3f" % s, int(hint), int(t)] + ["%.4f" % mm[k] for k in ("fine", "strong", "density", "both", "rowcv", "colcv", "contrast", "tiles")])
        if t:
            stems.setdefault(st, n)
        if montage and t and not (mm["density"] >= 0.015 and mm["fine"] >= 0.02 and s >= (2.0 if hint else 3.0)):
            samples_tiles.append((mm["tiles"], n, np.ascontiguousarray(L[::max(1, L.shape[0] // 126), ::max(1, L.shape[1] // 126)][:126, :126])))
        if montage:
            sy, sx = max(1, L.shape[0] // 126), max(1, L.shape[1] // 126)
            (samples_yes if t else samples_no).append((s, n, np.ascontiguousarray(L[::sy, ::sx][:126, :126])))
        if i % 500 == 0:
            print(i, len(seen), len(stems))
    for fh in handles.values():
        fh.close()
    with open(OUT_CSV, "w", newline="") as fcsv:
        wr = csv.writer(fcsv)
        wr.writerow(["image", "stem", "w", "h", "score", "name_hint", "text", "fine", "strong", "density", "both", "rowcv", "colcv", "contrast", "tiles"])
        wr.writerows(rows)
    for m in MANUAL_STEMS:
        stems.setdefault(m, "manual")
    keys = sorted(stems)
    with open(OUT_INC, "w", newline="\n") as fo:
        fo.write("// Generated by tools/vita/text_textures.py - textures with text on them,\n")
        fo.write("// kept at full resolution on the Vita (R_VitaSkipLevels). Sorted (strcmp):\n")
        fo.write("// texture stems, lower case, without _d/_s/_local/... suffixes. One string\n")
        fo.write("// block and offsets into it - no pointer table: every pointer is a\n")
        fo.write("// relocation, and the Vita ELF sits at vita-elf-create's ceiling.\n")
        offs = []
        pos = 0
        fo.write("static const char vitaTextTextureNames[] =\n")
        for k in keys:
            k = k.replace("\\", "/")
            offs.append(pos)
            fo.write('    "%s\\0"\n' % k)
            pos += len(k) + 1
        fo.write("    ;\n")
        fo.write("static const unsigned int vitaTextTextureOffsets[%d] = {\n" % len(offs))
        for i in range(0, len(offs), 12):
            fo.write("    " + ", ".join(str(o) for o in offs[i:i + 12]) + ",\n")
        fo.write("};\n")
    print("%d images measured, %d protected stems -> %s" % (len(rows), len(keys), OUT_INC))
    # what protecting costs: every map of a protected stem at full size
    # instead of half (x4), over all images of those usages
    tot_half = prot_extra = 0
    for n, (p, e) in seen.items():
        m = re.search(r"#__(\d\d)(\d\d)\.bimage$", n)
        if not m or m.group(2) != "00" or m.group(1) not in ("00", "01", "02", "03"):
            continue
        sz = e[1] / 4.0  # roughly the size after one level skipped
        tot_half += sz
        if stem_of(n) in stems:
            prot_extra += sz * 3
    print("world texture data at half size %.0f MB; protecting text adds %.0f MB over all maps together (each map loads only its own)" % (tot_half / 1048576, prot_extra / 1048576))
    if montage:
        from PIL import Image
        def sheet(items, fname, n=120):
            items = sorted(items, key=lambda x: -x[0])[:n]
            if not items:
                return
            cell = 128
            cols = 12
            im = Image.new("L", (cols * cell, ((len(items) + cols - 1) // cols) * cell), 40)
            for k, (s, nm, L) in enumerate(items):
                t = Image.fromarray((np.clip(L, 0, 1) * 255).astype(np.uint8))
                t.thumbnail((cell - 2, cell - 2))
                im.paste(t, ((k % cols) * cell + 1, (k // cols) * cell + 1))
            im.save(os.path.join(montage, fname))
            with open(os.path.join(montage, fname + ".txt"), "w") as ft:
                for k, (s, nm, L) in enumerate(items):
                    ft.write("%3d %.2f %s\n" % (k, s, nm))
        sheet(samples_yes, "protected_top.png")
        sheet(sorted(samples_yes, key=lambda x: x[0])[:120], "protected_weakest.png")
        sheet(samples_no, "not_protected_top.png")
        sheet(samples_tiles, "protected_by_tiles.png", 144)


if __name__ == "__main__":
    main()
