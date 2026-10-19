#!/usr/bin/env python3
"""Makes the Vita's bubble and LiveArea images from the PS3 disc's artwork.

usage: make_livearea.py <PS3_GAME folder> [output folder]
       (default output: neo/sys/vita/sce_sys)

From the PS3 disc (PS3_GAME): ICON0.PNG (320x176, the DOOM 3 BFG Edition logo
on transparency) and PIC1.PNG (1920x1080, the background the XMB shows). The
Vita wants:
  sce_sys/icon0.png                          128x128  the bubble
  sce_sys/pic0.png                           960x544  shown while the game is selected
  sce_sys/livearea/contents/bg.png           840x500  the LiveArea background
  sce_sys/livearea/contents/startup.png      280x158  the "Start" gate
all as 8-bit palette PNGs - the LiveArea shows a truecolour one as a broken
image. These are the user's own disc's images, for this port only.
"""
import os
import sys

from PIL import Image, ImageEnhance, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))


def fit_cover(im, w, h, focus_y=0.5):
    """scaled to cover w x h, cropped around the centre (focus_y: 0 top, 1 bottom)"""
    s = max(w / im.width, h / im.height)
    r = im.resize((max(w, round(im.width * s)), max(h, round(im.height * s))), Image.LANCZOS)
    x = (r.width - w) // 2
    y = int((r.height - h) * focus_y)
    return r.crop((x, y, x + w, y + h))


def paste_logo(bg, logo, width, cy):
    """the logo, width pixels wide, centred horizontally around height cy"""
    s = width / logo.width
    l = logo.resize((round(logo.width * s), round(logo.height * s)), Image.LANCZOS)
    # a soft dark halo so the logo reads over the busy background
    halo = Image.new("RGBA", l.size, (0, 0, 0, 0))
    halo.putalpha(l.getchannel("A").filter(ImageFilter.GaussianBlur(max(2, l.width // 40))))
    shadow = Image.new("RGBA", l.size, (0, 0, 0, 255))
    shadow.putalpha(halo.getchannel("A"))
    x = (bg.width - l.width) // 2
    y = int(cy - l.height / 2)
    bg.alpha_composite(shadow, (x, y))
    bg.alpha_composite(l, (x, y))
    return bg


def save_p8(im, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(path, optimize=True)
    print("%-60s %dx%d" % (path, im.width, im.height))


TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>

<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>

  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
"""


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "..", "neo", "sys", "vita", "sce_sys")
    logo = Image.open(os.path.join(src, "ICON0.PNG")).convert("RGBA")
    pic = Image.open(os.path.join(src, "PIC1.PNG")).convert("RGBA")
    # PIC1 without its logo (the bottom third holds it): the demon's head and the ring
    # bubble: the demon's face, darkened at the bottom, the logo over it
    face = fit_cover(pic.crop((640, 20, 1280, 660)), 128, 128)
    grad = Image.linear_gradient("L").resize((128, 128))
    dark = Image.new("RGBA", (128, 128), (0, 0, 0, 255))
    dark.putalpha(grad.point(lambda v: int(v * 0.85)))
    icon = face.copy()
    icon.alpha_composite(dark)
    # the bubble is shown as a circle: the logo kept inside it
    icon = paste_logo(icon, logo, 102, 76)
    save_p8(icon, os.path.join(out, "icon0.png"))
    # pic0 and the LiveArea background: PIC1 itself (its logo included)
    save_p8(fit_cover(pic, 960, 544), os.path.join(out, "pic0.png"))
    bg = ImageEnhance.Brightness(fit_cover(pic, 840, 500)).enhance(0.92)
    save_p8(bg, os.path.join(out, "livearea", "contents", "bg.png"))
    # the gate: the logo over a dark, blurred slice of the background
    gate = fit_cover(pic, 280, 158, 0.35).filter(ImageFilter.GaussianBlur(3))
    gate = ImageEnhance.Brightness(gate).enhance(0.45)
    gate = paste_logo(gate, logo, 236, 79)
    save_p8(gate, os.path.join(out, "livearea", "contents", "startup.png"))
    with open(os.path.join(out, "livearea", "contents", "template.xml"), "w", encoding="utf-8", newline="\n") as fp:
        fp.write(TEMPLATE)
    print("template.xml written")


if __name__ == "__main__":
    main()
