"""Builds the Vita UI art (button glyphs + controller picture) from the
user-supplied transparent PS Vita image, packs it into one RGBA atlas and
writes neo/sys/vita/vita_ui_assets.cpp with the atlas zlib-compressed.

Glyph canvases keep the exact sizes of the SWF bitmaps they replace, so the
menus' geometry is unchanged; only the pixels are Vita buttons.
"""
import os, sys, json, zlib
from PIL import Image, ImageDraw, ImageFont, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vita_ui_demon import rear_demon  # rear touch: a demon finger pressing one half of the pad

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = os.path.dirname(HERE)
SRC = os.path.join(SCRATCH, "imgs", "late_18085_4.png")
OUT_CPP = r"C:\Users\rayan\DOOM\choco-d3bfg\neo\sys\vita\vita_ui_assets.cpp"
FONT_BOLD = r"C:\Windows\Fonts\arialbd.ttf"
FONT_REG = r"C:\Windows\Fonts\arial.ttf"
SS = 4  # supersampling for everything drawn by hand

vita = Image.open(SRC).convert("RGBA")

# Button centres / extents measured on the 2000x903 source.
FACE = {"triangle": (1808, 218), "square": (1718, 303), "circle": (1897, 303), "cross": (1808, 388)}
FACE_R = 41          # button incl. its dark bevel
DPAD_C, DPAD_HALF = (191, 305), 116
LSTICK_C, RSTICK_C, STICK_R = (245, 541), (1755, 541), 93
SELECT_C, START_C = (1722, 715), (1832, 715)
PILL_W, PILL_H = 92, 44
# The art shows the port author's own Vita (vita_ui_silver.py): black front,
# silver plates, light grey buttons and stick caps with dark grey printing,
# silver PS / SELECT / START, white rear pad. One rim colour and width for
# every button family, darker than the light bodies so they hold their shape
# on the menus' dark backgrounds.
OUTLINE_SOLID = (196, 196, 200, 255)   # the white buttons' soft light grey rim (white Vita photo)
OUTLINE_W = 1.8   # px, every button outline
BODY_TOP, BODY_BOT = 252, 226      # white button body, lit from above
SYM_COL = (128, 128, 133, 255)     # printed symbols and labels: the photo's mid grey


from vita_ui_silver import recolor_vita
vita_silver = recolor_vita(vita, dict(FACE=FACE, FACE_R=FACE_R, DPAD_C=DPAD_C, DPAD_HALF=DPAD_HALF,
                                      LSTICK_C=LSTICK_C, RSTICK_C=RSTICK_C, STICK_R=STICK_R,
                                      SELECT_C=SELECT_C, START_C=START_C))


def circle_mask(size, inset=0.0):
    m = Image.new("L", (size * SS, size * SS), 0)
    d = ImageDraw.Draw(m)
    i = inset * SS
    d.ellipse((i, i, size * SS - 1 - i, size * SS - 1 - i), fill=255)
    return m.resize((size, size), Image.LANCZOS)


def rounded_mask(w, h, r):
    m = Image.new("L", (w * SS, h * SS), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w * SS - 1, h * SS - 1), r * SS, fill=255)
    return m.resize((w, h), Image.LANCZOS)


def crop_circle(c, r):
    x, y = c
    im = vita.crop((x - r, y - r, x + r, y + r)).copy()
    im.putalpha(Image.composite(im.getchannel("A"), Image.new("L", im.size, 0), circle_mask(2 * r)))
    return im


def pointer(canvas, cx, cy, h, colour=(235, 235, 235, 255)):
    """Small left-pointing triangle, as on the stock prompt glyphs."""
    big = Image.new("RGBA", (canvas.width * SS, canvas.height * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.polygon([((cx - h * 0.55) * SS, cy * SS), ((cx + h * 0.35) * SS, (cy - h * 0.5) * SS),
               ((cx + h * 0.35) * SS, (cy + h * 0.5) * SS)], fill=colour)
    canvas.alpha_composite(big.resize(canvas.size, Image.LANCZOS))


def ring(canvas, box, width, colour):
    big = Image.new("RGBA", (canvas.width * SS, canvas.height * SS), (0, 0, 0, 0))
    ImageDraw.Draw(big).ellipse(tuple(v * SS for v in box), outline=colour, width=int(width * SS))
    canvas.alpha_composite(big.resize(canvas.size, Image.LANCZOS))


def face_glyph(sym, w=38, h=34):
    """Vita face button: identical body for all four, symbol centred on it.

    Drawn rather than cropped so every button has exactly the same size,
    centre, ring and symbol weight. The body is centred in the canvas.
    """
    import numpy as np
    W, H = w * SS, h * SS
    d = 31.0 * SS                                   # body diameter incl. ring
    cx, cy = W / 2.0, H / 2.0
    R = d / 2.0
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32) + 0.5
    rr = np.hypot(xx - cx, yy - cy)
    # outline: a solid OUTLINE_W band at the rim, same weight and colour as the
    # shoulder and Select/Start outlines
    # (PIL's outlines come out ~0.4 px heavier than this analytic band, so the
    # band is widened by that much to measure the same once filtered down)
    ow = (OUTLINE_W + 0.45) * SS
    body = np.clip((R - ow) - rr, -0.5 * SS, 0.5 * SS) / SS + 0.5
    ring_m = np.clip((rr - (R - ow)) / SS + 0.5, 0, 1) * np.clip((R - rr) / SS + 0.5, 0, 1)
    # light grey: brighter at the top, a soft specular arc, shaded rim
    v = (yy - (cy - R)) / d
    shade = BODY_TOP - (BODY_TOP - BODY_BOT) * v
    spec = np.exp(-(((xx - cx) / (R * 0.55)) ** 2 + ((yy - (cy - R * 0.45)) / (R * 0.28)) ** 2)) * 14
    rim = np.clip((rr - (R - 5.0 * SS)) / (3.0 * SS), 0, 1) * 22
    lum = np.clip(shade + spec - rim, 6, 255)
    img = np.zeros((H, W, 4), np.float32)
    img[..., 0] = lum * 0.97
    img[..., 1] = lum * 0.97
    img[..., 2] = lum * 1.03
    img[..., 3] = body * 255
    ring_col = np.array(OUTLINE_SOLID[:3], np.float32)
    a = ring_m[..., None]
    img[..., :3] = img[..., :3] * (1 - a) + ring_col * a
    img[..., 3] = np.maximum(img[..., 3], ring_m * 255)
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA")

    # symbols: same stroke for all, sizes balanced optically
    sym_col = SYM_COL
    stroke = 2.1 * SS
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dr = ImageDraw.Draw(layer)
    if sym == "cross":
        k = 0.285 * d / 2                            # the X's arms reach the corners: smaller box
        for a0, a1 in (((-k, -k), (k, k)), ((-k, k), (k, -k))):
            dr.line([(cx + a0[0], cy + a0[1]), (cx + a1[0], cy + a1[1])], fill=sym_col, width=int(stroke))
            for px_, py_ in (a0, a1):
                dr.ellipse((cx + px_ - stroke / 2, cy + py_ - stroke / 2, cx + px_ + stroke / 2, cy + py_ + stroke / 2), fill=sym_col)
    elif sym == "circle":
        k = 0.395 * d / 2
        dr.ellipse((cx - k, cy - k, cx + k, cy + k), outline=sym_col, width=int(stroke))
    elif sym == "square":
        k = 0.33 * d / 2
        dr.rectangle((cx - k, cy - k, cx + k, cy + k), outline=sym_col, width=int(stroke))
    elif sym == "triangle":
        k = 0.47 * d / 2                            # circumradius; centroid on the centre
        import math
        # optical centre: centroid-centred (shift 0) reads high because the
        # apex is light; bounding-box-centred (shift k/4) reads low because
        # the base carries most of the ink. Halfway between looks centred.
        pts = [(cx + k * math.cos(math.radians(a)), cy + 0.125 * k + k * math.sin(math.radians(a))) for a in (-90, 30, 150)]
        dr.polygon(pts, outline=sym_col, width=int(stroke))
    im.alpha_composite(layer)
    return im.resize((w, h), Image.LANCZOS)

def pill_base(w, h, label, font_size, font=FONT_BOLD):
    """Light grey Vita-style pill (shoulder buttons, matching SELECT/START)."""
    W, H = w * SS, h * SS
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    grad = Image.new("RGBA", (W, H))
    gd = ImageDraw.Draw(grad)
    for yy in range(H):
        t = yy / max(1, H - 1)
        v = int(BODY_TOP - (BODY_TOP - BODY_BOT) * t)
        gd.line([(0, yy), (W, yy)], fill=(v, v, v - 2, 255))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((SS, SS, W - 1 - SS, H - 1 - SS), (H // 2) - SS, fill=255)
    im.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((SS, SS, W - 1 - SS, H - 1 - SS), (H // 2) - SS, outline=OUTLINE_SOLID, width=int(round(OUTLINE_W * SS)))
    if label:
        f = ImageFont.truetype(font, int(font_size * SS))
        bb = d.textbbox((0, 0), label, font=f)
        tw, th = bb[2] - bb[0], bb[3] - bb[1]
        d.text(((W - tw) / 2 - bb[0], (H - th) / 2 - bb[1]), label, font=f, fill=SYM_COL)
    return im.resize((w, h), Image.LANCZOS)


def fit(img, w, h, margin=0):
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    s = min((w - 2 * margin) / img.width, (h - 2 * margin) / img.height)
    r = img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.LANCZOS)
    canvas.alpha_composite(r, ((w - r.width) // 2, (h - r.height) // 2))
    return canvas


def shoulder(letter, w, h):
    pill = pill_base(w - 2, min(h - 2, int((w - 2) * 0.52)), letter, min(h, w) * 0.42)
    return fit(pill, w, h)


def stick(c, letter, w, h):
    """Analog stick, drawn as a perfect circle in the photo's proportions.

    Measured on the source (housing radius 93 px): concave cap to 0.67 R,
    grey housing, a bright ring at 0.83-0.86 R, grey housing to the edge.
    The photo's housing is slightly out of round and joins the casing at the
    top, so it is not cropped.
    """
    import numpy as np
    W, H = w * SS, h * SS
    d = (min(w, h) - 2) * SS
    R = d / 2.0
    cx, cy = W / 2.0, H / 2.0
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32) + 0.5
    rr = np.hypot(xx - cx, yy - cy) / R
    v = (yy - (cy - R)) / d                                   # 0 top .. 1 bottom
    aa = SS / R                                               # one output pixel in R units
    def band(r0, r1):
        return np.clip((rr - r0) / aa + 0.5, 0, 1) * np.clip((r1 - rr) / aa + 0.5, 0, 1)
    # the four layers of vita_ui_silver.stick_lum, outer to inner: thin white
    # outline (the outline width every glyph shares), glossy jet grey collar,
    # flat white edge of the top, rubbery grey top
    ow = (OUTLINE_W + 0.2) * SS / R
    from vita_ui_silver import stick_lum
    lum = stick_lum(rr, (xx - cx) / R, (yy - cy) / R, aa,
                    outline=(1.0 - ow, 1.0), jet=(0.78, 1.0 - ow), edge=(0.63, 0.78))   # the photo's proportions
    alpha = band(0.0, 1.0)
    img = np.zeros((H, W, 4), np.float32)
    img[..., 0] = lum * 0.98
    img[..., 1] = lum * 0.98
    img[..., 2] = lum * 1.02
    img[..., 3] = alpha * 255
    big = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA")
    dr = ImageDraw.Draw(big)
    f = ImageFont.truetype(FONT_BOLD, int(d * 0.30))   # fits the grey top
    bb = dr.textbbox((0, 0), letter, font=f)
    # dark grey letter on the light grey top (white would vanish on it)
    dr.text(((W - (bb[2] - bb[0])) / 2 - bb[0], (H - (bb[3] - bb[1])) / 2 - bb[1]), letter, font=f, fill=(62, 62, 66, 255))
    return big.resize((w, h), Image.LANCZOS)

def fit_centered(img, w, h, margin=1):
    """Like fit(), but the image lands on the exact centre of the canvas even
    when that is a half pixel (done at SS resolution, then filtered down)."""
    W, H = w * SS, h * SS
    s = min((W - 2 * margin * SS) / img.width, (H - 2 * margin * SS) / img.height)
    rw, rh = max(1, round(img.width * s)), max(1, round(img.height * s))
    # keep the leftover even so the offset is a whole SS pixel
    rw -= (W - rw) % 2
    rh -= (H - rh) % 2
    big = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    big.alpha_composite(img.resize((rw, rh), Image.LANCZOS), ((W - rw) // 2, (H - rh) // 2))
    return big.resize((w, h), Image.LANCZOS)


_DPAD_CACHE = {}


def dpad_symmetric():
    """The HUD D-pad: the photo's crystal D-pad (vita_ui_silver), rebuilt from
    its up arm - the one with the crispest bevels (averaging all four washes
    them out) - rotated four times about the pad's centre and averaged with
    its mirror image. No base around it (the author's call): the arms' own
    grey edges outline it.

    Returns (rgba float array, mask of the up arm), square, the pad's centre
    at the middle pixel corner.
    """
    import numpy as np
    from vita_ui_silver import photo_patch, PHOTO_DPAD_C
    if "sym" in _DPAD_CACHE:
        return _DPAD_CACHE["sym"]
    Z = 6                                         # working px per photo px
    # the photo's arms, photo px, to the outside of their grey edge line;
    # centred on the up arm itself (half a pixel left of the pad's centre),
    # so its two side edges coincide when it is mirrored
    REACH, HALF_W, CORNER = 47.75, 19.0, 6.0
    # Inside that edge line the photo has a 1 px light rim (the top face's
    # highlight); on the dark menus it read as small white edges around the
    # pad, and greying it out made a thick grey outline. So the arms are cut
    # just inside the rim instead (4 px in at the ends, 3.25 px along the
    # sides) and a thin grey edge line drawn again along the new outline.
    CUT_END, CUT_SIDE = 4.0, 3.25
    centre = (PHOTO_DPAD_C[0] - 0.75, PHOTO_DPAD_C[1])
    R2 = int(np.ceil((REACH + 4.0) * Z))
    N = 2 * R2
    S = photo_patch(centre, (float(R2), float(R2)), (0, 0, N, N), scale=Z) / 255.0

    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32) + 0.5
    px, py = (xx - R2) / Z, (yy - R2) / Z         # photo px from the centre

    def rrect(hx, hy, rc):                        # signed distance, photo px
        qx, qy = np.abs(px) - (hx - rc), np.abs(py) - (hy - rc)
        return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - rc

    dv, dh = rrect(HALF_W, REACH, CORNER), rrect(REACH, HALF_W, CORNER)
    cut = np.minimum(rrect(HALF_W - CUT_SIDE, REACH - CUT_END, CORNER),
                     rrect(REACH - CUT_END, HALF_W - CUT_SIDE, CORNER))
    cov = lambda d: np.clip(0.5 - d * Z, 0, 1)    # one working pixel of anti-aliasing
    alpha = cov(cut)

    # the photo's plate around the pad is matched to the picture's plate
    # colour, as the picture does, so both D-pads have the same tones
    rr = np.hypot(px, py)
    ring = (rr > REACH + 1.5) & (rr < REACH + 3.0) & (np.minimum(dv, dh) > 1.0)
    plate_col = np.array([238.0, 238.0, 240.0], np.float32) / 255.0
    S = S * (plate_col / np.maximum(np.median(S[ring], 0), 1e-3))

    # up wedge, feathered one pixel along the diagonals so the 4 copies sum to 1
    w_up = np.clip((-(py * Z) - np.abs(px * Z)) + 0.5, 0, 1)
    S = S * w_up[..., None]
    S = S + np.rot90(S, 1) + np.rot90(S, 2) + np.rot90(S, 3)
    S = 0.5 * (S + S[:, ::-1])
    # the thin edge line along the cut, in the photo's edge grey
    e = 0.85 * np.clip((cut + 1.4) / 0.6, 0, 1)
    S = S * (1 - e[..., None]) + (145.0 / 255.0) * e[..., None]
    arm_up = alpha * w_up
    res = (np.concatenate([np.clip(S, 0, 1), alpha[..., None]], -1), arm_up)
    _DPAD_CACHE["sym"] = res
    return res


def dpad(w, h, lit=None):
    import numpy as np
    img, arm_up = dpad_symmetric()
    img = img.copy()
    if lit:
        k = {"up": 0, "left": 1, "down": 2, "right": 3}[lit]
        arm = np.rot90(arm_up, k)
        a = np.asarray(Image.fromarray((arm * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.6)), np.float32)[..., None] / 255.0
        rgb = img[..., :3] * 255.0
        lum = rgb.mean(-1, keepdims=True)
        # Mars orange-brown of the game's HUD, run through the button's own
        # shading so the bevel and the printed arrow keep their shape
        hud = np.array([206, 112, 46], np.float32)
        tinted = hud * np.clip(lum / 212.0, 0.0, 1.0) ** 0.9
        arrow = np.clip((150.0 - lum) / 50.0, 0, 1)
        tinted = tinted * (1 - arrow) + np.array([62, 30, 14], np.float32) * arrow
        img[..., :3] = (rgb * (1 - a) + tinted * a) / 255.0
    im = Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")
    # crop the padding away symmetrically about the centre, so the pad fills
    # the glyph and stays exactly centred
    al = img[..., 3]
    n = al.shape[0]
    ys, xs = np.nonzero(al > 0.02)
    half = int(np.ceil(max(n / 2 - ys.min(), ys.max() + 1 - n / 2, n / 2 - xs.min(), xs.max() + 1 - n / 2))) + 1
    c = n // 2
    im = im.crop((c - half, c - half, c + half, c + half))
    return fit_centered(im, w, h, 1)

def dpad_dir(direction, w=48, h=48):
    """D-pad with one arm lit, for prompts that mean a single direction."""
    return dpad(w, h, lit=direction)

FONT_NARROW = r"C:\Windows\Fonts\ARIALN.TTF"   # the Vita's own SELECT/START print is a condensed sans


def oval_pill(label, w, h):
    """SELECT / START, drawn at SS resolution: a clean oval with the light
    body gradient, the shared rim, and the label in dark grey. (The crop of
    the source picture was ~90x44 px and came out pixelated when scaled.)

    Same proportions as the button and its print on the source: the oval is
    2.3x as wide as it is tall, SELECT spans 74% of its width and START uses
    the same font size."""
    W, H = w * SS, h * SS
    m = 1.0 * SS                                         # keep the rim off the canvas edge
    ow = W - 2 * m
    oh = min(H - 2 * m, ow / 2.3)
    box = (m, (H - oh) / 2.0, W - 1 - m, (H + oh) / 2.0 - 1)
    grad = Image.new("RGBA", (W, H))
    gd = ImageDraw.Draw(grad)
    for yy in range(H):
        t = yy / max(1, H - 1)
        v = int(BODY_TOP - (BODY_TOP - BODY_BOT) * t)
        gd.line([(0, yy), (W, yy)], fill=(v, v, v - 2, 255))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).ellipse(box, fill=255)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    im.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(im)
    d.ellipse(box, outline=OUTLINE_SOLID, width=int(round(OUTLINE_W * SS)))
    # font size from SELECT spanning 74% of the oval (START shares it)
    target_w = 0.74 * ow
    size = int(oh)
    while size > 4:
        f = ImageFont.truetype(FONT_NARROW, size)
        bb = d.textbbox((0, 0), "SELECT", font=f)
        if bb[2] - bb[0] <= target_w:
            break
        size -= 1
    f = ImageFont.truetype(FONT_NARROW, size)
    bb = d.textbbox((0, 0), label, font=f)
    d.text(((W - (bb[2] - bb[0])) / 2 - bb[0], (H - (bb[3] - bb[1])) / 2 - bb[1]), label, font=f, fill=SYM_COL)
    return im.resize((w, h), Image.LANCZOS)


def pill_crop(c, w, h):
    """Select / Start: the button only. The photo's dark recess around it is
    cut away by tracing the button body (luma ~34) out from its middle; the
    recess (~13) and the casing (~25) both stop the fill."""
    import numpy as np
    x, y = c
    box = (x - PILL_W // 2, y - PILL_H // 2, x + PILL_W // 2, y + PILL_H // 2)
    im = vita_silver.crop(box).copy()                       # silver pixels...
    lum = np.asarray(vita.crop(box).convert("RGB"), np.float32).mean(-1)   # ...traced on the original
    body = Image.fromarray((lum > 29).astype(np.uint8) * 255).copy()
    row = lum[PILL_H // 2]
    seed_x = next(i for i in range(PILL_W // 2) if 29 < row[i] < 45)   # first body pixel from the left
    ImageDraw.floodfill(body, (seed_x + 2, PILL_H // 2), 128)
    m = np.asarray(body) == 128
    # fill the holes the printed label leaves (anything the outside can't reach)
    outside = Image.fromarray((~m).astype(np.uint8) * 255).copy()
    ImageDraw.floodfill(outside, (0, 0), 128)
    m = m | (np.asarray(outside) != 128)
    # the traced outline is a little ragged where the top highlight is; use the
    # clean oval that fits it
    traced = Image.fromarray(m.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(3))
    x0, y0, x1, y1 = traced.getbbox()
    oval = Image.new("L", (PILL_W * SS, PILL_H * SS), 0)
    ImageDraw.Draw(oval).ellipse((x0 * SS, y0 * SS, x1 * SS - 1, y1 * SS - 1), fill=255)
    im.putalpha(oval.resize((PILL_W, PILL_H), Image.LANCZOS))
    im = im.crop((x0, y0, x1, y1))
    # place it centred at SS resolution and trace the same light outline ring
    # the face buttons have (226,226,230, 1.6 px), along the oval's edge
    W, H = w * SS, h * SS
    sc = min((W - 2 * SS) / im.width, (H - 2 * SS) / im.height)
    rw, rh = round(im.width * sc), round(im.height * sc)
    rw -= (W - rw) % 2
    rh -= (H - rh) % 2
    big = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ox, oy = (W - rw) // 2, (H - rh) // 2
    big.alpha_composite(im.resize((rw, rh), Image.LANCZOS), (ox, oy))
    # outermost band of the oval (PIL draws the outline inward from the box);
    # colour chosen so it renders at the same brightness (~172) as the face
    # buttons' and shoulder buttons' outlines
    ring_w = OUTLINE_W * SS
    ImageDraw.Draw(big).ellipse((ox, oy, ox + rw - 1, oy + rh - 1), outline=OUTLINE_SOLID, width=int(round(ring_w)))
    return big.resize((w, h), Image.LANCZOS)


GLYPHS = [
    # id name                    builder
    ("face_cross",    lambda: face_glyph("cross")),
    ("face_circle",   lambda: face_glyph("circle")),
    ("face_square",   lambda: face_glyph("square")),
    ("face_triangle", lambda: face_glyph("triangle")),
    ("l_52x28",       lambda: shoulder("L", 52, 28)),
    ("r_52x28",       lambda: shoulder("R", 52, 28)),
    ("l_49x35",       lambda: shoulder("L", 49, 35)),
    ("r_49x35",       lambda: shoulder("R", 49, 35)),
    ("l_43x43",       lambda: shoulder("L", 43, 43)),
    ("r_43x43",       lambda: shoulder("R", 43, 43)),
    ("lstick_52x52",  lambda: stick(LSTICK_C, "L", 52, 52)),
    ("rstick_52x52",  lambda: stick(RSTICK_C, "R", 52, 52)),
    ("lstick_52x51",  lambda: stick(LSTICK_C, "L", 52, 51)),
    ("rstick_52x51",  lambda: stick(RSTICK_C, "R", 52, 51)),
    ("dpad_56x56",    lambda: dpad(56, 56)),
    ("dpad_52x51",    lambda: dpad(52, 51)),
    ("select_88x55",  lambda: oval_pill("SELECT", 88, 55)),
    ("select_51x32",  lambda: oval_pill("SELECT", 51, 32)),
    ("start_88x55",   lambda: oval_pill("START", 88, 55)),
    ("dpad_up",       lambda: dpad_dir("up")),
    ("dpad_down",     lambda: dpad_dir("down")),
    ("dpad_left",     lambda: dpad_dir("left")),
    ("dpad_right",    lambda: dpad_dir("right")),
    ("rear_left",     lambda: rear_demon("left")),
    ("rear_right",    lambda: rear_demon("right")),
    ("controller",    lambda: fit(vita_silver, 512, 232)),
]

# which replacement each stock SWF glyph gets
SWF_MAP = {
    "ps_cross": "face_cross", "ps_circle": "face_circle", "ps_square": "face_square", "ps_triangle": "face_triangle",
    "xb_a": "face_cross", "xb_b": "face_circle", "xb_x": "face_square", "xb_y": "face_triangle",
    "ps_l1": "l_52x28", "ps_r1": "r_52x28", "xb_lb": "l_49x35", "xb_rb": "r_49x35",
    "xb_lt": "l_43x43", "xb_rt": "r_43x43",
    "ps_lstick": "lstick_52x52", "ps_rstick": "rstick_52x52", "xb_ls": "lstick_52x51", "xb_rs": "rstick_52x51",
    "ps_dpad": "dpad_56x56", "xb_dpad": "dpad_52x51",
    "ps_select": "select_88x55", "xb_back": "select_51x32",
}

images = [(name, build()) for name, build in GLYPHS]

# simple shelf packer into a power-of-two-width atlas
ATLAS_W = 512
x = y = shelf = 0
rects = {}
for name, im in sorted(images, key=lambda t: -t[1].height):
    if x + im.width > ATLAS_W:
        x, y, shelf = 0, y + shelf + 1, 0
    rects[name] = (x, y, im.width, im.height)
    x += im.width + 1
    shelf = max(shelf, im.height)
ATLAS_H = y + shelf
atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (0, 0, 0, 0))
for name, im in images:
    rx, ry, _, _ = rects[name]
    atlas.alpha_composite(im, (rx, ry))
def save_png(im, name):
    # a viewer holding the file open makes the save fail; try fresh names
    stem, ext = os.path.splitext(name)
    for candidate in (name, stem + "2" + ext, "%s_%d%s" % (stem, os.getpid(), ext)):
        try:
            im.save(os.path.join(HERE, candidate))
            return candidate
        except OSError:
            pass
    return None


save_png(atlas, "vita_ui_atlas.png")

# preview on the menu's dark background
prev = Image.new("RGBA", (ATLAS_W * 2, ATLAS_H * 2), (24, 22, 20, 255))
prev.alpha_composite(atlas.resize((ATLAS_W * 2, ATLAS_H * 2), Image.NEAREST))
PREVIEW = save_png(prev, "vita_ui_preview.png")

matches = json.load(open(os.path.join(SCRATCH, "glyph_matches.json")))
swf_rows = []
for m in matches:
    if m["glyph"] in SWF_MAP:
        swf_rows.append((m["swf"], m["x"], m["y"], m["w"], m["h"], SWF_MAP[m["glyph"]], m["glyph"]))

raw = atlas.tobytes()
z = zlib.compress(raw, 9)
names = [n for n, _ in GLYPHS]
with open(OUT_CPP, "w", newline="\n") as f:
    f.write("// Generated by tools/vita/compose_vita_ui.py from the transparent PS Vita\n")
    f.write("// artwork. Do not edit by hand. Atlas: %dx%d RGBA, %d bytes, %d compressed.\n\n" % (ATLAS_W, ATLAS_H, len(raw), len(z)))
    f.write('#include "vita_ui.h"\n\n')
    f.write("const int vitaUiAtlasWidth = %d;\nconst int vitaUiAtlasHeight = %d;\n" % (ATLAS_W, ATLAS_H))
    f.write("const unsigned int vitaUiAtlasCompressedSize = %d;\n" % len(z))
    f.write("const unsigned char vitaUiAtlasCompressed[%d] = {\n" % len(z))
    for i in range(0, len(z), 24):
        f.write("    " + ", ".join("0x%02x" % b for b in z[i:i + 24]) + ",\n")
    f.write("};\n\n")
    f.write("const vitaUiGlyph_t vitaUiGlyphs[] = {\n")
    for n in names:
        rx, ry, rw, rh = rects[n]
        f.write('    { "%s", %d, %d, %d, %d },\n' % (n, rx, ry, rw, rh))
    f.write("};\nconst int vitaUiNumGlyphs = %d;\n\n" % len(names))
    f.write("// Stock SWF bitmaps, by atlas rectangle, and the Vita glyph that replaces each.\n")
    f.write("const vitaUiSwfGlyph_t vitaUiSwfGlyphs[] = {\n")
    for row in swf_rows:
        f.write('    { "swf/%s.swf", %d, %d, %d, %d, "%s" }, // %s\n' % row)
    f.write("};\nconst int vitaUiNumSwfGlyphs = %d;\n" % len(swf_rows))
print("atlas %dx%d, %d glyphs, %d swf overrides, %d -> %d bytes" % (ATLAS_W, ATLAS_H, len(names), len(swf_rows), len(raw), len(z)))
