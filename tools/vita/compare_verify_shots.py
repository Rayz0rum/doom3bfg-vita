#!/usr/bin/env python3
"""Compare the full verification's screenshots from the Vita with a PC run's.

usage: compare_verify_shots.py <vita verify dir> <pc verify dir> <out dir> [top N]

Both runs stand the player on the same open spots and turn the same ways
(VitaVerify.cpp: openspot + shots), so shots with the same name show the same
view. For each pair: the mean difference and the worst 8x8-tile difference
(lighting and shadows that went wrong show up as tiles, resolution and
texture size as a low overall difference), a CSV of all of them, and side by
side pictures (Vita | PC | difference) of the N worst to look at.
"""
import csv
import os
import re
import sys

from PIL import Image, ImageChops, ImageOps


def key(name):
    # the serial number differs between runs: compare by the rest
    return re.sub(r"^\d+_", "", os.path.splitext(name)[0])


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    vdir, pdir, odir = sys.argv[1:4]
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 40
    os.makedirs(odir, exist_ok=True)
    pcs = {key(f): f for f in os.listdir(pdir) if f.lower().endswith(".jpg")}
    rows = []
    for f in sorted(os.listdir(vdir)):
        if not f.lower().endswith(".jpg"):
            continue
        k = key(f)
        if k not in pcs:
            continue
        a = Image.open(os.path.join(vdir, f)).convert("L").resize((320, 180))
        b = Image.open(os.path.join(pdir, pcs[k])).convert("L").resize((320, 180))
        d = ImageChops.difference(a, b)
        px = list(d.getdata())
        mean = sum(px) / len(px)
        worst = 0.0
        for ty in range(0, 180, 20):
            for tx in range(0, 320, 20):
                t = d.crop((tx, ty, tx + 20, ty + 20))
                tp = list(t.getdata())
                worst = max(worst, sum(tp) / len(tp))
        rows.append((k, f, pcs[k], round(mean, 1), round(worst, 1)))
    rows.sort(key=lambda r: -r[4])
    with open(os.path.join(odir, "compare.csv"), "w", newline="") as fp:
        w = csv.writer(fp)
        w.writerow(["view", "vita", "pc", "mean diff", "worst tile diff"])
        w.writerows(rows)
    for i, (k, vf, pf, mean, worst) in enumerate(rows[:top]):
        a = Image.open(os.path.join(vdir, vf)).convert("RGB").resize((640, 360))
        b = Image.open(os.path.join(pdir, pf)).convert("RGB").resize((640, 360))
        d = ImageOps.autocontrast(ImageChops.difference(a, b).convert("L")).convert("RGB")
        sheet = Image.new("RGB", (1920, 360))
        sheet.paste(a, (0, 0))
        sheet.paste(b, (640, 0))
        sheet.paste(d, (1280, 0))
        sheet.save(os.path.join(odir, "%02d_%s.jpg" % (i, k)), quality=85)
    print("%d pairs compared; worst first in %s" % (len(rows), os.path.join(odir, "compare.csv")))
    for r in rows[:15]:
        print("  %-60s mean %5.1f  worst tile %5.1f" % (r[0], r[3], r[4]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
