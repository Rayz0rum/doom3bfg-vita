"""Reflow the legal splash text without the Bink Video sentence.

Finds every word of the three legal images (english, french, figs), drops the
"Uses Bink Video ... RAD Game Tools, Inc." words, breaks the rest into lines
with the least raggedness and justifies them into one block starting at the
logos' margin. The block width is picked per language (the one giving the
evenest word spacing with a well-filled last line). Writes previews
(legal_<lang>_reflow.png) and the C table for Common.cpp RenderSplash
(legal_reflow_table.txt).
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bimg import load, read, bimage, decode, png, BASE

OUT = os.path.dirname(os.path.abspath(__file__))
LEFT = 80                       # the text's margin in the images
MAXRIGHT = 1200                 # never closer than 80 px to the right edge
PITCH = 29                      # line pitch (the images' own: 28.9)
SPACE = 9                       # natural word space

# the Bink words: (text line index, word x0) per language
DROP = {
    "english": lambda li, x0: li == 5 and x0 >= 386,
    "french": lambda li, x0: (li == 5 and x0 >= 944) or (li == 6 and x0 < 674),
    "figs": lambda li, x0: li == 5 and x0 < 806,
}


def layout(widths, width):
    """Least total squared stretch; returns (cost, lines, widest space)."""
    n = len(widths)
    INF = float("inf")
    best = [INF] * (n + 1)
    nxt = [0] * (n + 1)
    best[n] = 0.0
    for i in range(n - 1, -1, -1):
        lw = 0
        for j in range(i, n):
            lw += widths[j] + (SPACE if j > i else 0)
            if lw > width:
                break
            # never between "DOOM" (71 px) and the numeral after it ("3,", "II")
            if j < n - 1 and 68 <= widths[j] <= 74 and widths[j + 1] < 22:
                continue
            gaps = j - i
            if j == n - 1:
                # the last line is not stretched; it should fill the block
                # at least halfway so the block keeps its shape
                fill = lw / width
                cost = 0.0 if fill >= 0.55 else (0.55 - fill) * 4000.0
            else:
                per = (width - lw) / gaps if gaps else 99.0
                cost = per * per
            if cost + best[j + 1] < best[i]:
                best[i] = cost + best[j + 1]
                nxt[i] = j + 1
    lines = []
    i = 0
    while i < n:
        lines.append(list(range(i, nxt[i])))
        i = nxt[i]
    widest = 0.0
    for line in lines[:-1]:
        lw = sum(widths[k] for k in line)
        widest = max(widest, (width - lw) / max(1, len(line) - 1))
    return best[0], lines, widest


f, e = load(os.path.join(BASE, "_common.resources"))
table = []
for lang in ["english", "french", "figs"]:
    img = bimage(read(f, e["generated/images/guis/assets/splash/legal_%s#__0200.bimage" % lang]))
    w, h, px = decode(img)
    orig = bytes(px)

    def bright(x, y):
        q = (y * w + x) * 4
        return orig[q] + orig[q + 1] + orig[q + 2] > 300

    rows = [sum(1 for x in range(w) if bright(x, y)) for y in range(h)]
    bands = []
    y = 0
    while y < h:
        if rows[y]:
            s = y
            while y < h and rows[y]:
                y += 1
            bands.append((s, y))
        else:
            y += 1
    logo = bands[0]
    text = bands[1:]
    words = []  # (src x0, x1, text line, baseline)
    for bi, (y0, y1) in enumerate(text):
        # baseline: the lowest row still carrying a quarter of the line's ink
        peak = max(rows[y0:y1])
        base = max(yy for yy in range(y0, y1) if rows[yy] >= peak * 0.25)
        cols = [any(bright(x, yy) for yy in range(y0, y1)) for x in range(w)]
        x = 0
        while x < w:
            if cols[x]:
                s, gap, xe = x, 0, x
                while x < w and gap < 7:
                    if cols[x]:
                        gap, xe = 0, x
                    else:
                        gap += 1
                    x += 1
                if not DROP[lang](bi, s):
                    words.append((s, xe + 1, bi, base))
            else:
                x += 1

    # source rows of each line: its band, grown by up to 2 rows into the
    # blank rows around it (never into a neighbour's band)
    def src_rows(bi):
        y0, y1 = text[bi]
        lo = text[bi - 1][1] if bi > 0 else logo[1]
        hi = text[bi + 1][0] if bi + 1 < len(text) else h
        return max(y0 - 2, lo), min(y1 + 2, hi)

    widths = [x1 - x0 for (x0, x1, bi, base) in words]
    # the block width: the evenest spacing, ties to the wider block
    choice = None
    for width in range(900, MAXRIGHT - LEFT + 1, 2):
        cost, lines, widest = layout(widths, width)
        score = (round(widest, 0), len(lines), cost / len(lines))
        if choice is None or score < choice[0] or (score == choice[0] and width > choice[1]):
            choice = (score, width, lines, widest)
    _, width, lines, widest = choice

    out = bytearray(len(orig))
    for q in range(3, len(out), 4):
        out[q] = 255
    entries = []
    # the logo row as it is, without the Bink logo (x >= 640)
    entries.append((73, logo[0] - 2, 640, logo[1] + 2, 73, logo[0] - 2))
    base0 = words[0][3]
    for li, line in enumerate(lines):
        lw = sum(widths[k] for k in line)
        gaps = len(line) - 1
        last = (li == len(lines) - 1)
        space = SPACE if (last or gaps == 0) else (width - lw) / gaps
        x = float(LEFT)
        newBase = base0 + li * PITCH
        for k in line:
            x0, x1, bi, base = words[k]
            r0, r1 = src_rows(bi)
            dx = int(round(x))
            dy = newBase - (base - r0)
            entries.append((x0 - 2, r0, x1 + 2, r1, dx - 2, dy))
            x += widths[k] + space
    for (sx0, sy0, sx1, sy1, dx, dy) in entries:
        for yy in range(sy0, sy1):
            for xx in range(sx0, sx1):
                s = (yy * w + xx) * 4
                d = ((dy + yy - sy0) * w + (dx + xx - sx0)) * 4
                for c in range(3):
                    out[d + c] = max(out[d + c], orig[s + c])
    png(os.path.join(OUT, "legal_%s_reflow.png" % lang), w, h, out)
    lastw = sum(widths[k] for k in lines[-1]) + SPACE * (len(lines[-1]) - 1)
    print("%s: %d words, block %d..%d, %d lines, widest space %.1f px, last line %d px (%.0f%%)" % (
        lang, len(words), LEFT, LEFT + width, len(lines), widest, lastw, 100.0 * lastw / width))
    table.append((lang, entries))

with open(os.path.join(OUT, "legal_reflow_table.txt"), "w") as fo:
    for lang, entries in table:
        fo.write("    // %s: %d rects\n" % (lang, len(entries)))
        fo.write("    static const short legal_%s[][6] = {\n" % lang)
        for i in range(0, len(entries), 4):
            fo.write("        " + " ".join("{ %d, %d, %d, %d, %d, %d }," % en for en in entries[i:i + 4]) + "\n")
        fo.write("    };\n")
print("ok")
