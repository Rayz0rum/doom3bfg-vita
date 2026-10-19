"""Rear-touch glyph: a Cyberdemon finger (stringy pink-red muscle, knobby joints,
dark hooked claw) pressing one half of the rear pad.

Everything is drawn procedurally at 4x and filtered down. Units in the geometry
below are final pixels of a w x h glyph (multiplied by SS when drawing).
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageOps

SS = 4


def _over(dst, src):
    """Straight-alpha 'over' for float RGBA arrays in 0..1."""
    a = src[..., 3:4]
    out_a = a + dst[..., 3:4] * (1 - a)
    rgb = (src[..., :3] * a + dst[..., :3] * dst[..., 3:4] * (1 - a)) / np.maximum(out_a, 1e-6)
    return np.concatenate([rgb, out_a], axis=-1)


def _layer(mask, colour):
    """RGBA float layer from a 0..1 mask and an RGB(A) colour or a per-pixel RGB array."""
    h, w = mask.shape
    out = np.zeros((h, w, 4), np.float32)
    col = np.asarray(colour, np.float32)
    if col.ndim == 1:
        out[..., :3] = col[:3] / 255.0
        out[..., 3] = mask * (col[3] / 255.0 if col.size > 3 else 1.0)
    else:
        out[..., :3] = col
        out[..., 3] = mask
    return out


def _draw_mask(W, H, fn):
    m = Image.new("L", (W, H), 0)
    fn(ImageDraw.Draw(m))
    return m


def _f(im):
    return np.asarray(im, np.float32) / 255.0


def _noise(rng, W, H, cell, resample):
    small = (rng.random((max(1, H // cell), max(1, W // cell))) * 255).astype(np.uint8)
    return _f(Image.fromarray(small).resize((W, H), resample))


def _catmull(points, n_per):
    """Centripetal-ish Catmull-Rom through points; returns samples and the knot index of each."""
    P = [points[0] * 2 - points[1]] + list(points) + [points[-1] * 2 - points[-2]]
    out, seg = [], []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        for s in np.linspace(0, 1, n_per, endpoint=(i == len(P) - 3)):
            s2, s3 = s * s, s * s * s
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * s + (2 * p0 - 5 * p1 + 4 * p2 - p3) * s2 + (-p0 + 3 * p1 - 3 * p2 + p3) * s3))
            seg.append(i - 1 + s)
    return np.array(out), np.array(seg)


def rear_demon(side, w=112, h=70, seed=666):
    W, H = w * SS, h * SS
    sx, sy = w / 112.0, h / 70.0           # geometry below is authored for 112x70
    S = lambda x, y: np.array([x * sx * SS, y * sy * SS])
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    img = np.zeros((H, W, 4), np.float32)

    # --- the rear pad: dark textured slab with a faint centre seam -------------------
    pad = (2 * SS, 3 * SS, W - 2 * SS, H - 3 * SS)
    rad = 11 * SS
    ins = 1.6 * SS
    pad_mask = _f(_draw_mask(W, H, lambda d: d.rounded_rectangle(pad, rad, fill=255)))
    inner = _f(_draw_mask(W, H, lambda d: d.rounded_rectangle((pad[0] + ins, pad[1] + ins, pad[2] - ins, pad[3] - ins), rad - ins, fill=255)))
    edge = np.clip(pad_mask - inner, 0, 1)
    grain = _noise(rng, W, H, 2, Image.BILINEAR)
    # white rear pad, as on the port author's Vita
    vgrad = 1.0 - 0.10 * (yy - pad[1]) / (pad[3] - pad[1])
    slab = np.stack([(v / 255.0) * vgrad * (0.965 + 0.05 * grain) for v in (238, 238, 235)], -1)
    img = _over(img, _layer(pad_mask * 0.96, slab))
    img = _over(img, _layer(edge, (150, 150, 153, 255)))
    cx = W / 2
    seam = _f(_draw_mask(W, H, lambda d: d.line([(cx, pad[1] + 7 * SS), (cx, pad[3] - 7 * SS)], fill=255, width=SS)))
    img = _over(img, _layer(seam * pad_mask, (176, 176, 180, 200)))

    # Seen "through" the Vita from the front, so the player's left hand is on the
    # left: the finger reaches in from the outer edge and the fleshy pad of its
    # fingertip is what presses the panel, the way a hand actually holds a Vita.
    # The claw rides on the far side of the fingertip and never touches the pad.

    # --- the finger: three knobby segments reaching in from the outer edge ----------
    joints = [S(-12, 66), S(6, 53), S(21, 44.5), S(33, 40)]     # base, knuckle 1, knuckle 2, fingertip
    path, seg = _catmull(joints, 40)
    radii_at = np.array([10.8, 9.6, 8.6, 7.6]) * sx * SS      # broad, pressed-flat fingertip pad
    radius = np.interp(seg, np.arange(len(radii_at)), radii_at)
    for kj, bump in ((1, 1.15), (2, 1.13)):                  # knobby knuckles
        radius = radius * (1.0 + (bump - 1.0) * np.exp(-((seg - kj) / 0.13) ** 2))
    for mid in (0.5, 1.5):                                     # bony narrowing between joints
        radius = radius * (1.0 - 0.08 * np.exp(-((seg - mid) / 0.22) ** 2))

    tang = np.gradient(path, axis=0)
    tang /= np.linalg.norm(tang, axis=1, keepdims=True)
    nrm = np.stack([-tang[:, 1], tang[:, 0]], -1)
    nrm[(nrm[:, 1] + 0.35 * nrm[:, 0]) > 0] *= -1           # every normal points up/left
    # the fingertip pad touches here
    i_pad = int(np.argmin(np.abs(seg - 2.78)))
    contact = path[i_pad] + tang[i_pad] * 0.5 * SS
    tx, ty = contact

    # --- hellfire spreading out from under the pressed pad ---------------------------
    half = _f(_draw_mask(W, H, lambda d: d.rounded_rectangle((pad[0] + ins, pad[1] + ins, cx - SS, pad[3] - ins), rad - 2 * SS, fill=255)))
    dist = np.hypot(xx - tx, yy - ty)
    img = _over(img, _layer(half * 0.10, (150, 20, 6, 255)))
    img = _over(img, _layer(half * np.exp(-(dist / (19 * SS)) ** 2) * 0.85, (255, 70, 10, 255)))
    for r, a, wid in ((10.0, 0.70, 1.3), (14.5, 0.42, 1.0), (19.0, 0.22, 0.8)):
        ring = np.clip(1.0 - np.abs(dist - r * SS) / (wid * SS), 0, 1)
        img = _over(img, _layer(half * ring * a, (255, 150, 40, 255)))

    # --- the claw: thick dark horn jutting past the fingertip, held off the panel ----
    tg, up, r = tang[-1], nrm[-1], radius[-1]
    root = path[-1] - tg * r * 0.55
    tip = path[-1] + tg * (r + 7.0 * sx * SS) + up * 1.2 * SS
    ctl = path[-1] + tg * (r + 3.0 * sx * SS) + up * 1.6 * SS
    cs = np.linspace(0, 1, 48)[:, None]
    cpath = (1 - cs) ** 2 * root + 2 * (1 - cs) * cs * ctl + cs ** 2 * tip
    crad = (r * 0.62) * (1 - cs[:, 0]) ** 0.75 + 0.35 * SS
    px = np.stack([xx, yy], -1).reshape(-1, 2)
    cd = np.full(px.shape[0], 1e9, np.float32)
    ci = np.zeros(px.shape[0], np.int32)
    for i, p in enumerate(cpath):
        d = np.hypot(px[:, 0] - p[0], px[:, 1] - p[1]) - crad[i]
        upd = d < cd
        cd[upd] = d[upd]
        ci[upd] = i
    cd, ci = cd.reshape(H, W), ci.reshape(H, W)
    claw_m = np.clip(0.5 - cd / SS, 0, 1)
    ctan = np.gradient(cpath, axis=0)
    ctan /= np.linalg.norm(ctan, axis=1, keepdims=True)
    cn = np.stack([-ctan[:, 1], ctan[:, 0]], -1)
    cn[(cn[:, 1] + 0.35 * cn[:, 0]) > 0] *= -1
    c_across = np.clip(-((xx - cpath[ci, 0]) * cn[ci, 0] + (yy - cpath[ci, 1]) * cn[ci, 1]) / crad[ci], -1, 1)
    claw_im = Image.fromarray((claw_m * 255).astype(np.uint8))
    # raised off the panel, so its shadow lands further away than the finger's
    cshadow = np.roll(_f(claw_im.filter(ImageFilter.GaussianBlur(1.6 * SS))), (int(3.5 * SS), int(2.5 * SS)), (0, 1))
    img = _over(img, _layer(cshadow * pad_mask * 0.6, (0, 0, 0, 255)))
    img = _over(img, _layer(_f(claw_im.filter(ImageFilter.MaxFilter(SS + 1))), (8, 2, 2, 255)))
    c_bulge = np.sqrt(np.clip(1 - c_across ** 2, 0, 1))
    claw_col = (np.array([40, 24, 22]) / 255.0) + (c_bulge * 0.10)[..., None] * np.array([1.0, 0.7, 0.65])
    glint = np.exp(-((c_across + 0.5) / 0.12) ** 2) * np.clip(1.1 - cs[ci, 0] * 1.2, 0, 1)
    claw_col = claw_col + glint[..., None] * np.array([0.40, 0.30, 0.28])
    claw_col = claw_col + np.exp(-(dist / (12 * SS)) ** 2)[..., None] * np.array([0.30, 0.10, 0.02])
    img = _over(img, _layer(claw_m, np.clip(claw_col, 0, 1)))

    # --- finger shading ---------------------------------------------------------------
    best_d = np.full(px.shape[0], 1e9, np.float32)
    best_i = np.zeros(px.shape[0], np.int32)
    for i, p in enumerate(path):
        d = np.hypot(px[:, 0] - p[0], px[:, 1] - p[1]) - radius[i]
        upd = d < best_d
        best_d[upd] = d[upd]
        best_i[upd] = i
    best_d = best_d.reshape(H, W)
    best_i = best_i.reshape(H, W)
    finger = np.clip(0.5 - best_d / SS, 0, 1)
    seg_px = seg[best_i]
    rel_x, rel_y = xx - path[best_i, 0], yy - path[best_i, 1]
    across = np.clip(-(rel_x * nrm[best_i, 0] + rel_y * nrm[best_i, 1]) / radius[best_i], -1, 1)   # -1 = upper edge

    # Cyberdemon flesh: muted pink-salmon over deep red, wrapped in stringy muscle fibre
    bulge = np.sqrt(np.clip(1 - across ** 2, 0, 1))
    light = 0.20 + 0.66 * bulge - 0.36 * across
    mottle = _noise(rng, W, H, 5, Image.BICUBIC)
    blotch = _noise(rng, W, H, 14, Image.BICUBIC)
    skin_lo = np.array([46, 16, 14], np.float32) / 255.0
    skin_hi = np.array([200, 124, 108], np.float32) / 255.0
    k = np.clip(light * (0.80 + 0.26 * mottle + 0.24 * (blotch - 0.5)), 0, 1.25)[..., None]
    skin = skin_lo + (skin_hi - skin_lo) * k
    # fibres run along the finger; they meander and break up like the model's muscle
    warp = 2.4 * (_noise(rng, W, H, 9, Image.BICUBIC) - 0.5) + 0.9 * seg_px
    fib = np.sin(across * np.pi * 4.5 + warp)
    fib = np.clip((fib - 0.35) / 0.65, 0, 1) ** 1.6
    fib *= 0.55 + 0.45 * _noise(rng, W, H, 4, Image.BICUBIC)
    skin = skin * (1 - 0.55 * fib)[..., None]
    skin = skin * (1 - 0.25 * np.clip(across, 0, 1) ** 2)[..., None]       # deep red underside
    skin = skin + (np.array([0.10, 0.0, 0.0]) * np.clip(across, 0, 1)[..., None])
    # moist highlight on the back of the knuckles
    knuck = np.exp(-((seg_px - 1) / 0.22) ** 2) + np.exp(-((seg_px - 2) / 0.22) ** 2)
    sheen = np.exp(-((across + 0.5) / 0.2) ** 2) * (0.12 + 0.30 * knuck)
    skin = skin + sheen[..., None] * np.array([0.80, 0.62, 0.56])
    # wrinkled folds across each joint
    crease = np.zeros((H, W), np.float32)
    for kj in (1, 2):
        for off, strength in ((-0.11, 0.5), (-0.045, 0.85), (0.035, 0.85), (0.10, 0.5)):
            band = np.exp(-((seg_px - (kj + off)) / 0.018) ** 2)
            crease = np.maximum(crease, band * strength * np.clip(1.15 - np.abs(across), 0, 1))
    skin = skin * (1 - 0.7 * crease)[..., None]
    # the pressed pad: flattened against the panel, blanched pale, rimmed with fire
    along = (rel_x * tang[best_i, 0] + rel_y * tang[best_i, 1])
    pd = np.hypot((xx - tx) / (6.2 * sx * SS), (yy - ty) / (5.4 * sx * SS))
    patch = np.clip(1.15 - pd, 0, 1) ** 0.8 * finger
    skin = skin * (1 - 0.45 * patch)[..., None] + patch[..., None] * np.array([0.86, 0.62, 0.56]) * 0.45
    rim = np.exp(-((pd - 1.0) / 0.16) ** 2) * finger
    skin = skin + rim[..., None] * np.array([0.62, 0.24, 0.04])
    glow_on = np.exp(-(dist / (13 * SS)) ** 2) * finger
    skin = np.clip(skin + glow_on[..., None] * np.array([0.30, 0.10, 0.02]), 0, 1)

    finger_im = Image.fromarray((finger * 255).astype(np.uint8))
    shadow = _f(finger_im.filter(ImageFilter.GaussianBlur(2.2 * SS)))
    shadow = np.roll(shadow, (int(2.0 * SS), int(1.6 * SS)), (0, 1))
    img = _over(img, _layer(shadow * pad_mask * 0.6, (0, 0, 0, 255)))
    img = _over(img, _layer(_f(finger_im.filter(ImageFilter.MaxFilter(SS + 3))), (14, 4, 4, 255)))
    img = _over(img, _layer(finger, skin))
    # fire leaking out from under the pad's edge
    leak = np.clip(_f(finger_im.filter(ImageFilter.GaussianBlur(1.2 * SS))) - finger, 0, 1) * np.exp(-(dist / (10 * SS)) ** 2)
    img = _over(img, _layer(np.clip(leak * 2.2, 0, 1), (255, 170, 60, 255)))

    out = Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").resize((w, h), Image.LANCZOS)
    return out if side == "left" else ImageOps.mirror(out)


if __name__ == "__main__":
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    l, r = rear_demon("left"), rear_demon("right")
    sheet = Image.new("RGBA", (l.width * 2 + 30, l.height + 20), (24, 22, 20, 255))
    sheet.alpha_composite(l, (10, 10))
    sheet.alpha_composite(r, (l.width + 20, 10))
    sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(os.path.join(here, "demon_preview.png"))
    small = Image.new("RGBA", (l.width + 20, l.height + 20), (24, 22, 20, 255))
    small.alpha_composite(l, (10, 10))
    small.save(os.path.join(here, "demon_preview_1x.png"))
