"""The port author's own PS Vita, as the Vita UI art shows it.

Black front (the source image's), with the two figure-8 plates around the
D-pad / left stick and the face buttons / right stick in white, the clear
"crystal" D-pad and face buttons of a white Vita (taken from a photo of one),
white stick rings around grey rubber tops, and white PS, SELECT and START
buttons with grey printing. (The rear touch pad is white: vita_ui_demon.py.)

recolor_vita(src) returns the recoloured full picture; the glyph builders in
compose.py take their pixels from it and their shapes from the original.
"""
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

# Colours sampled from a photo of a white Vita (the author's buttons): plates
# near white (~230-245), white buttons with mid grey printing (~150) and soft
# light grey rims (~200), the sticks' jet grey collar, white ring and light
# grey rubber top (~175 -> 155).
SILVER_GAIN, SILVER_BIAS = 0.45, 202.0      # plate luma 63..96 -> ~230..245
GAP_GREY = 205.0                            # the soft rim between buttons and plate

# The D-pad and the face buttons of the author's Vita are clear "crystal"
# plastic: see-through light grey with grey edges, bevels and inner
# reflections that no recolouring of the black picture can give. They come
# straight from the photo of the white Vita, each element resampled onto the
# black picture's own centre for it (the two pictures' layouts differ).
PHOTO_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "imgs", "vita_white.png")
PHOTO_SCALE = 2.4                           # black picture px per photo px (button spacing, plate circles)
# centres in pixel-edge coordinates: the D-pad's by mirror symmetry, the face
# buttons' from a circle fit to their rims (radius 16.25 photo px)
PHOTO_DPAD_C = (117.75, 222.75)
PHOTO_FACE_C = {"triangle": (789.4, 188.2), "square": (753.15, 222.9),
                "circle": (825.35, 222.4), "cross": (789.4, 259.25)}
SRC_DPAD_C = (190.0, 306.0)                 # the black picture's, from its arms' extents
SRC_FACE_C = {"triangle": (1808.0, 220.75), "square": (1719.5, 304.5),
              "circle": (1896.5, 304.5), "cross": (1808.0, 388.0)}
TINT = np.array([1.0, 1.0, 0.985], np.float32)   # a touch warm, like the photo's silver


def _lum(a):
    return a[..., :3].mean(-1)


def flood(lum, seed, lo, hi, bbox):
    x0, y0, x1, y1 = bbox
    sub = lum[y0:y1, x0:x1]
    m = ((sub >= lo) & (sub <= hi)).astype(np.uint8) * 255
    im = Image.fromarray(m).copy()
    ImageDraw.floodfill(im, (seed[0] - x0, seed[1] - y0), 128)
    out = np.zeros(lum.shape, bool)
    out[y0:y1, x0:x1] = np.asarray(im) == 128
    return out


def fill_holes(mask):
    """Everything the outside cannot reach (printed symbols inside a body)."""
    im = Image.fromarray((~mask).astype(np.uint8) * 255).copy()
    ImageDraw.floodfill(im, (0, 0), 128)
    return mask | (np.asarray(im) != 128)


def silver(l):
    return np.clip(SILVER_BIAS + SILVER_GAIN * l, 0, 255)


_PHOTO = []


def photo():
    """The white Vita photo, its JPEG colour speckle smoothed away (the
    plastic's faint cool tint stays)."""
    if not _PHOTO:
        y, cb, cr = Image.open(PHOTO_FILE).convert("RGB").convert("YCbCr").split()
        cb, cr = (c.filter(ImageFilter.GaussianBlur(1.5)) for c in (cb, cr))
        _PHOTO.append(Image.merge("YCbCr", (y, cb, cr)).convert("RGB"))
    return _PHOTO[0]


def photo_patch(pc, sc, box, scale=PHOTO_SCALE):
    """RGB float array: the photo resampled so that photo point pc lands on
    point sc (both pixel-edge coordinates) of a picture scale times larger,
    over that picture's rectangle box."""
    x0, y0, x1, y1 = box
    s = 1.0 / scale
    data = (s, 0.0, pc[0] - (sc[0] - x0) * s, 0.0, s, pc[1] - (sc[1] - y0) * s)
    return np.asarray(photo().transform((x1 - x0, y1 - y0), Image.AFFINE, data, resample=Image.BICUBIC), np.float32)


def paste_photo(out, pc, sc, r_solid, r_zero):
    """Photo pixels into out (HxWx4 float) in a disc about sc: all photo up
    to r_solid, blended smoothly into what is there by r_zero. The photo's
    plate around the element is matched to the plate already there first,
    so the seam does not show."""
    x0, y0 = int(sc[0] - r_zero - 2), int(sc[1] - r_zero - 2)
    x1, y1 = int(sc[0] + r_zero + 3), int(sc[1] + r_zero + 3)
    patch = photo_patch(pc, sc, (x0, y0, x1, y1))
    yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32) + 0.5
    r = np.hypot(xx - sc[0], yy - sc[1])
    seam = (r >= r_solid) & (r <= r_zero)
    patch *= np.median(out[y0:y1, x0:x1, :3][seam], 0) / np.maximum(np.median(patch[seam], 0), 1.0)
    t = np.clip((r_zero - r) / (r_zero - r_solid), 0, 1)
    a = (t * t * (3 - 2 * t))[..., None]
    out[y0:y1, x0:x1, :3] = out[y0:y1, x0:x1, :3] * (1 - a) + patch * a


# The analog sticks, outer to inner (the author's own description of their
# Vita): a thin white outline; a glossy jet grey (#2F2F2F) band - the stick's
# collar, not the stick; a flat white ring - the edge of the stick's top; and
# the top itself, regular grey with a rubbery gradient, where the thumb sits.
JET = 47.0                                          # 0x2F


def stick_lum(rr, xn, yn, aa, outline, jet, edge, seed=7):
    """Luminance of the four stick layers.

    rr: radius / R; xn, yn: offsets / R (yn down); aa: one output pixel in R
    units; outline / jet / edge: (inner, outer) radii of those bands; the
    rubbery top fills everything inside edge[0]."""
    def band(r0, r1):
        return np.clip((rr - r0) / aa + 0.5, 0, 1) * np.clip((r1 - rr) / aa + 0.5, 0, 1)
    v = np.clip((yn + 1.0) / 2.0, 0, 1)             # 0 top .. 1 bottom
    lum = np.zeros(rr.shape, np.float32)

    # rubbery top: lighter at the top, a broad matte sheen, a little darker
    # toward its rim, fine grain
    rc = edge[0]
    top = band(-1.0, rc)
    rng = np.random.default_rng(seed)
    grain = rng.standard_normal(rr.shape).astype(np.float32)
    grain = np.asarray(Image.fromarray(((grain * 0.5 + 0.5) * 255).clip(0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8)), np.float32) / 255.0 - 0.5
    sheen = np.exp(-((xn / (rc * 0.75)) ** 2 + ((yn + rc * 0.35) / (rc * 0.55)) ** 2))
    rubber = 176.0 - 20.0 * v + 6.0 * sheen - 10.0 * np.clip(rr / rc, 0, 1) ** 3 + 4.0 * grain
    lum += top * rubber

    # flat white ring at the edge of the top (no gloss)
    lum += band(edge[0], edge[1]) * 248.0

    # glossy jet grey collar: a sharp highlight along its upper arc and a
    # faint reflection low down
    jb = band(jet[0], jet[1])
    mid, half = 0.5 * (jet[0] + jet[1]), 0.5 * (jet[1] - jet[0])
    across = np.exp(-((rr - (mid - 0.15 * half)) / (0.42 * half)) ** 2)
    up = np.clip(-yn / np.maximum(rr, 1e-3), 0, 1) ** 3          # 1 at the top, 0 from the sides down
    low = np.clip(yn / np.maximum(rr, 1e-3), 0, 1) ** 4
    gloss = JET + 8.0 * (1.0 - v) + across * (95.0 * up + 22.0 * low)
    lum += jb * gloss

    # thin white outline
    lum += band(outline[0], outline[1]) * 244.0
    return lum


def disc(shape, c, r):
    yy, xx = np.mgrid[0:shape[0], 0:shape[1]]
    return np.hypot(xx - c[0], yy - c[1]) <= r


def recolor_vita(src, geo):
    """geo: dict of the measured centres/radii from compose.py."""
    a = np.asarray(src.convert("RGBA")).astype(np.float32)
    lum = _lum(a)
    H, W = lum.shape
    out = a.copy()
    newl = lum.copy()

    # plates (traced between their grooves)
    plates = flood(lum, (261, 235), 38, 120, (0, 100, 460, 760)) | flood(lum, (1745, 240), 38, 120, (1540, 100, 2000, 760))
    newl[plates] = silver(lum[plates])

    # sticks: cap inverted, housing silver, the groove between them darkish
    for c in (geo["LSTICK_C"], geo["RSTICK_C"]):
        R = geo["STICK_R"]
        area = disc(lum.shape, c, R) & ~plates
        yy, xx = np.mgrid[0:H, 0:W]
        rr = np.hypot(xx - c[0], yy - c[1]) / R
        # outside the white outline: the housing, silver like the plate
        ring = area & (rr > 0.865)
        hl = lum[ring]
        newl[ring] = np.where(hl < 10, GAP_GREY + hl, silver(np.maximum(hl, 60.0)))
        # inside it: the four layers, on the source's own radii (bright ring
        # 0.83-0.865 R, cap edge 0.67 R)
        inner = area & (rr <= 0.865)
        xn = (xx - c[0]) / R
        yn = (yy - c[1]) / R
        # the photo's proportions: collar and white ring equally thick (~15%
        # of the collar's radius each), the grey top inside 70% of it
        sl = stick_lum(rr, xn, yn, 1.0 / R, outline=(0.83, 0.865), jet=(0.705, 0.83), edge=(0.58, 0.705))
        newl[inner] = sl[inner]

    # PS, SELECT, START: the whole button is light, up to a thin rim (the
    # source's dark bevel ring inside the outline used to stay dark), with
    # its printing in dark grey
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    for (cx, cy, ea, eb) in ((224.0, 715.0, 76.0, 33.0),
                             (geo["SELECT_C"][0], geo["SELECT_C"][1], 42.0, 19.0),
                             (geo["START_C"][0], geo["START_C"][1], 42.0, 19.0)):
        x0, x1 = int(cx - ea - 3), int(cx + ea + 4)
        y0, y1 = int(cy - eb - 3), int(cy + eb + 4)
        sx, sy = xx[y0:y1, x0:x1], yy[y0:y1, x0:x1]
        e = np.sqrt(((sx - cx) / ea) ** 2 + ((sy - cy) / eb) ** 2)
        dist = (e - 1.0) * min(ea, eb)                       # px, negative inside
        inside = np.clip(0.5 - dist, 0, 1)
        rim = inside * np.clip(dist + 1.6 + 0.5, 0, 1)       # the outermost ~1.6 px
        v = (sy - (cy - eb)) / (2 * eb)
        sl = lum[y0:y1, x0:x1]
        t = np.clip((sl - 100.0) / 60.0, 0, 1)               # the printed symbol / label...
        t *= np.clip((0.84 - e) / 0.06, 0, 1)                # ...not the highlight arc along the top edge
        if ea > 60:
            # PS: white, dark grey logo, soft light rim
            body = 250.0 - 16.0 * v
            ink, rim_col = 95.0, 200.0
        else:
            # SELECT / START are small in the controller picture (about 20 px
            # wide in the atlas): a light grey body, a darker rim and a dark
            # grey label with its strokes thickened, or they wash out
            body = 222.0 - 18.0 * v
            ink, rim_col = 85.0, 140.0
            # a firmer threshold first: the source's print is unevenly light
            t = np.clip((sl - 90.0) / 40.0, 0, 1) * np.clip((0.84 - e) / 0.06, 0, 1)
            t = np.asarray(Image.fromarray((t * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3)), np.float32) / 255.0
        body = body * (1 - t) + ink * t
        val = body * (1 - rim) + rim_col * rim
        cur = newl[y0:y1, x0:x1]
        newl[y0:y1, x0:x1] = cur * (1 - inside) + val * inside

    scale = np.where(lum > 1.0, newl / np.maximum(lum, 1.0), 0.0)
    changed = np.abs(newl - lum) > 0.5
    grey = np.repeat(newl[..., None], 3, -1) * TINT
    out[..., :3] = np.where(changed[..., None], grey, a[..., :3])

    # the crystal D-pad and face buttons, from the photo: each in a disc that
    # also takes in the black picture's dark groove around it (the D-pad's
    # reaches the plate's rim, the photo's arms reach 47.5 px = 114 here)
    paste_photo(out, PHOTO_DPAD_C, SRC_DPAD_C, 116.0, 121.0)
    for k, sc in SRC_FACE_C.items():
        paste_photo(out, PHOTO_FACE_C[k], sc, 43.0, 48.0)
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGBA")


if __name__ == "__main__":
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    src = Image.open(os.path.join(os.path.dirname(here), "imgs", "late_18085_4.png"))
    geo = dict(FACE={"triangle": (1808, 218), "square": (1718, 303), "circle": (1897, 303), "cross": (1808, 388)},
               FACE_R=41, DPAD_C=(191, 305), DPAD_HALF=116, LSTICK_C=(245, 541), RSTICK_C=(1755, 541), STICK_R=93,
               SELECT_C=(1722, 715), START_C=(1832, 715))
    res = recolor_vita(src, geo)
    res.save(os.path.join(here, "vita_silver_full.png"))
    small = res.copy()
    small.thumbnail((1000, 1000))
    bg = Image.new("RGBA", small.size, (40, 40, 40, 255))
    bg.alpha_composite(small)
    bg.save(os.path.join(here, "vita_silver_small.png"))
    res.crop((0, 80, 470, 800)).save(os.path.join(here, "vita_silver_left.png"))
    res.crop((1530, 80, 2000, 800)).save(os.path.join(here, "vita_silver_right.png"))
