"""Mock-up of the full-screen Vita controller layout, using the game's own
font textures/metrics and the Vita UI atlas. Needs the game data at the path
in bimg.BASE. Usage: python layout_mock.py [width height] The layout code here is the
reference for Vita_UI_DrawControllerLayoutFullscreen() in vita_ui.cpp."""
import os, re, struct, sys
from PIL import Image, ImageDraw
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bimg

W, H = (int(sys.argv[1]), int(sys.argv[2])) if len(sys.argv) > 2 else (720, 408)
SS = 3  # draw supersampled, filter down: close to the GPU's bilinear look

f, e = bimg.load(os.path.join(bimg.BASE, "_common.resources"))


class Font:
    def __init__(self, name):
        d = bimg.read(f, e["newfonts/%s/48.dat" % name])
        magic, point, self.asc, self.desc, n = struct.unpack_from(">IhhhH", d, 0)
        p = 12
        gl = []
        for i in range(n):
            gl.append(struct.unpack_from("<BBbbBBHH", d, p))
            p += 10
        chars = struct.unpack_from("<%dI" % n, d, p)
        self.g = {c: gl[i] for i, c in enumerate(chars)}
        img = bimg.bimage(bimg.read(f, e["generated/images/newfonts/%s/48#__0400.bimage" % name]))
        w, h, px = bimg.decode(img)
        im = Image.frombytes("RGBA", (w, h), bytes(px))
        self.cov = im.getchannel("G")   # CFM_GREEN_ALPHA: coverage lives in green

    def glyph(self, ch):
        return self.g.get(ord(ch), self.g[ord("*")])

    def width(self, text, th):
        s = th / 48.0
        return sum(self.glyph(c)[4] for c in text) * s

    def draw(self, canvas, x, ytop, text, th, colour, shadow=True):
        s = th / 48.0
        base = ytop + self.asc * s
        for pass_ in ((1, (0, 0, 0, int(colour[3] * 0.85))), (0, colour)) if shadow else ((0, colour),):
            off, col = pass_
            px = x
            for c in text:
                w, h, top, left, skip, pad, gs, gt = self.glyph(c)
                if w and h:
                    cov = self.cov.crop((gs, gt, gs + w, gt + h)).resize((max(1, round(w * s * SS)), max(1, round(h * s * SS))), Image.BILINEAR)
                    layer = Image.new("RGBA", cov.size, col[:3] + (0,))
                    layer.putalpha(cov.point(lambda v, a=col[3]: v * a // 255))
                    canvas.alpha_composite(layer, (int((px + left * s + off) * SS), int((base - top * s + off) * SS)))
                px += skip * s


sar = Font("sarasori_rg")
bank = Font("bankgothic_md_bt")

# the atlas exactly as the game has it: zlib RGBA inside vita_ui_assets.cpp
import zlib
ASSETS = os.path.join(HERE, "..", "..", "neo", "sys", "vita", "vita_ui_assets.cpp")
src = open(ASSETS).read()
aw = int(re.search(r"vitaUiAtlasWidth = (\d+);", src).group(1))
ah = int(re.search(r"vitaUiAtlasHeight = (\d+);", src).group(1))
blob = src[src.index("vitaUiAtlasCompressed["):]
blob = blob[blob.index("{") + 1:blob.index("};")]
raw = zlib.decompress(bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", blob)))
atlas = Image.frombytes("RGBA", (aw, ah), raw)
G = {n: tuple(map(int, r)) for n, *r in re.findall(r'\{ "(\w+)", (\d+), (\d+), (\d+), (\d+) \}', src)}


def glyph_img(name):
    x, y, w, h = G[name]
    return atlas.crop((x, y, x + w, y + h))


def paste_fit(canvas, img, x, y, w, h, alpha=255):
    r = img.resize((max(1, int(w * SS)), max(1, int(h * SS))), Image.LANCZOS)
    canvas.alpha_composite(r, (int(x * SS), int(y * SS)))


def rect(canvas, x0, y0, x1, y1, col):
    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).rectangle((x0 * SS, y0 * SS, x1 * SS, y1 * SS), fill=col)
    canvas.alpha_composite(layer)


def line(canvas, a, b, wpx, col):
    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).line((a[0] * SS, a[1] * SS, b[0] * SS, b[1] * SS), fill=col, width=max(1, int(wpx * SS)))
    canvas.alpha_composite(layer)


# ---------------------------------------------------------------- the rows
LEFT = [("l_49x35", "Flashlight", (0.125, 0.020)),
        ("dpad_up", "Quick-slot 1: Grenades", (0.0955, 0.249)),
        ("dpad_left", "Previous Weapon", (0.0555, 0.338)),
        ("dpad_right", "Next Weapon", (0.1355, 0.338)),
        ("dpad_down", "Quick-slot 2: BFG 9000", (0.0955, 0.426)),
        ("lstick_52x52", "Move", (0.1225, 0.599))]
RIGHT = [("r_49x35", "Primary Fire / Melee", (0.875, 0.020)),
         ("face_triangle", "Use / Interact", (0.904, 0.241)),
         ("face_circle", "Crouch", (0.9485, 0.3355)),
         ("face_square", "Reload", (0.859, 0.3355)),
         ("face_cross", "Jump", (0.904, 0.4297)),
         ("rstick_52x52", "Camera / Aim", (0.8775, 0.599)),
         ("select_88x55", "PDA / Logs", (0.861, 0.792)),
         ("start_88x55", "Pause Menu", (0.916, 0.792))]
REAR = [("rear_left", "180\xb0 Quick Turn", 0.25), ("rear_right", "Sprint (Toggle)", 0.75)]


def split_label(font, text, th, maxw):
    """One line if it fits, else two. Prefer breaking right after a ':' or '/'
    (punctuation ends the first line, like "Quick-slot 1:" / "Grenades"),
    otherwise at the space that balances the two lines."""
    if font.width(text, th) <= maxw:
        return [text]
    best = None
    for i, ch in enumerate(text):
        if ch == " ":
            a, b = text[:i].rstrip(), text[i + 1:].lstrip()
            wmax = max(font.width(a, th), font.width(b, th))
            fits = wmax <= maxw
            punct = a[-1:] in (":", "/") and fits
            lone = len(b.split()) < 2      # a single word left on line two
            key = (0 if punct else 1, 1 if lone else 0, wmax)
            if best is None or key < best[0]:
                best = (key, [a, b])
    return best[1] if best else [text]


def layout(th):
    margin = 0.045 * W
    pw = 0.40 * W
    ph = pw * G["controller"][3] / G["controller"][2]
    icon = 1.35 * th
    gap = 0.35 * th
    lead = 1.1 * th
    col_text = (W - 2 * margin - pw) / 2 - lead - icon - gap
    rows = []
    for side, data in ((0, LEFT), (1, RIGHT)):
        lst = []
        for g, label, uv in data:
            lines = split_label(sar, label, th, col_text)
            lst.append((g, lines, uv))
        rows.append(lst)
    widest = max(sar.width(l, th) for side in rows for g, lines, uv in side for l in lines)
    line_h = 1.08 * th
    def rh(lines):
        return line_h * len(lines) + 0.62 * th
    col_h = [sum(rh(l) for g, l, uv in side) for side in rows]
    content_top, content_bot = 0.20 * H, 0.885 * H
    rear_h = 2.4 * th
    block_h = ph + 0.7 * th + rear_h + 0.2 * th + 2 * 1.08 * th
    fits = widest <= col_text and max(col_h) <= content_bot - content_top and block_h <= content_bot - content_top
    return dict(margin=margin, pw=pw, ph=ph, icon=icon, gap=gap, lead=lead, col_text=col_text, rows=rows,
                line_h=line_h, rh=rh, col_h=col_h, content_top=content_top, content_bot=content_bot,
                rear_h=rear_h, block_h=block_h, fits=fits, widest=widest)


th = 0.046 * H
L = layout(th)
while not L["fits"] and th > 8:
    th *= 0.95
    L = layout(th)
print("text height %.1f px (%.1f%% of the screen), column text room %.0f px, widest line %.0f px, fits %s"
      % (th, 100 * th / H, L["col_text"], L["widest"], L["fits"]))

C = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 255))
# background: near-black with a faint warm lift toward the top
for y in range(H):
    k = 1.0 - y / H
    rect(C, 0, y, W, y + 1, (int(9 + 10 * k), int(8 + 5 * k), int(8 + 3 * k), 255)) if y % 4 == 0 else None
rect(C, 0, 0, W, H, (10, 9, 9, 120))

# header: the stock heading look - white BankGothic over an orange plate fading right
hx0, hy0, hy1 = 0, 0.05 * H, 0.155 * H
steps = 48
for i in range(steps):
    a = (1 - i / steps) ** 1.6
    rect(C, hx0 + W * 0.62 * i / steps, hy0, hx0 + W * 0.62 * (i + 1) / steps, hy1, (150, 44, 12, int(235 * a)))
head_th = 0.072 * H
bank.draw(C, L["margin"], (hy0 + hy1) / 2 - head_th * 0.62, "CONTROLLER LAYOUT", head_th, (255, 255, 255, 255))
rect(C, 0, hy1 + 0.006 * H, W * 0.75, hy1 + 0.006 * H + max(1, 0.004 * H), (247, 148, 30, 200))

# the Vita and the rear pads, centred in the content area
pw, ph = L["pw"], L["ph"]
px0 = (W - pw) / 2
py0 = L["content_top"] + (L["content_bot"] - L["content_top"] - L["block_h"]) / 2
paste_fit(C, glyph_img("controller"), px0, py0, pw, ph)

orange = (247, 148, 30, 240)
dot_col = (255, 160, 40, 255)
for side in (0, 1):
    rows = L["rows"][side]
    colh = L["col_h"][side]
    cy_pic = py0 + ph / 2
    top = max(L["content_top"], min(cy_pic - colh / 2, L["content_bot"] - colh))
    y = top
    for g, lines, uv in rows:
        h = L["rh"](lines)
        cy = y + h / 2
        icon = L["icon"]
        if side == 0:
            ix = px0 - L["lead"] - icon
            lx = ix + icon + 0.12 * th
            edge = min(px0 + 0.02 * pw, px0 + uv[0] * pw)
        else:
            ix = px0 + pw + L["lead"]
            lx = ix - 0.12 * th
            edge = max(px0 + 0.98 * pw, px0 + uv[0] * pw)
        tgt = (px0 + uv[0] * pw, py0 + uv[1] * ph)
        line(C, (lx, cy), (edge, cy), max(1, 0.09 * th), orange)
        line(C, (edge, cy), tgt, max(1, 0.09 * th), orange)
        d = max(3, 0.32 * th)
        rect(C, tgt[0] - d / 2, tgt[1] - d / 2, tgt[0] + d / 2, tgt[1] + d / 2, dot_col)
        gi = glyph_img(g)
        a = gi.width / gi.height
        gw, gh = (icon, icon / a) if a >= 1 else (icon * a, icon)
        paste_fit(C, gi, ix + (icon - gw) / 2, cy - gh / 2, gw, gh)
        ty = cy - (len(lines) * L["line_h"]) / 2
        for k, ln in enumerate(lines):
            tw = sar.width(ln, th)
            tx = (ix - L["gap"] - tw) if side == 0 else (ix + icon + L["gap"])
            sar.draw(C, tx, ty + k * L["line_h"] - 0.08 * th, ln, th, (242, 242, 242, 255))
        y += h

rear_top = py0 + ph + 0.7 * th
for g, label, u in REAR:
    gh = L["rear_h"]
    gw = gh * 1.6
    cx = px0 + u * pw
    paste_fit(C, glyph_img(g), cx - gw / 2, rear_top, gw, gh)
    # the label stays inside its third of the picture, clear of the leader lines
    for k, ln in enumerate(split_label(sar, label, th, 0.36 * pw)):
        tw = sar.width(ln, th)
        sar.draw(C, cx - tw / 2, rear_top + gh + 0.2 * th + k * L["line_h"], ln, th, (242, 242, 242, 255))

# footer prompt, like the menus' command bar
fth = 0.05 * H
fy = 0.93 * H
paste_fit(C, glyph_img("face_circle"), L["margin"], fy - fth * 0.62, fth * 1.25 * 38 / 34, fth * 1.25)
bank.draw(C, L["margin"] + fth * 1.55, fy - fth * 0.55, "BACK", fth, (255, 255, 255, 255))

out = C.resize((W, H), Image.LANCZOS)
out.save(os.path.join(HERE, "layout_mock_%dx%d.png" % (W, H)))
# and as it lands on the 960x544 screen after the display hardware's upscale
out.resize((960, 544), Image.BILINEAR).save(os.path.join(HERE, "layout_mock_%dx%d_onscreen.png" % (W, H)))
print("saved")
