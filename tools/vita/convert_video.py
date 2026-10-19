import os, subprocess, sys, json

SP   = os.path.dirname(os.path.abspath(__file__))
FF   = os.path.join(SP, "tools", "ffmpeg.exe")
FP   = os.path.join(SP, "tools", "ffprobe.exe")
SRC  = r"C:\Users\rayan\OneDrive\Desktop\DOOM3\base\video"
DST  = os.path.join(SP, "video_mp4")

# usage: convert_video.py [ffmpeg.exe] [source video folder] [output folder]
if len(sys.argv) > 1:
    FF = sys.argv[1]
    FP = None  # probing goes through ffmpeg itself when only it is given
if len(sys.argv) > 2:
    SRC = sys.argv[2]
if len(sys.argv) > 3:
    DST = sys.argv[3]

MAX_W, MAX_H = 960, 544


# H.264 levels: (name, max frame size in macroblocks, max macroblocks a second).
# The Vita's decoder sizes its frame buffers for the level the stream declares,
# not for the picture: every video was level 3.1 (frames up to 1280x720), and a
# player asked for ~2.2 MB a frame buffer even for a 512x512 video (~13 MB a
# player, two at most in the 26 MB the decoder may use). Each video now
# declares the lowest level its size and frame rate allow.
LEVELS = [
    ("1.1", 396, 3000),
    ("1.2", 396, 6000),
    ("1.3", 396, 11880),
    ("2.1", 792, 19800),
    ("2.2", 1620, 20250),
    ("3", 1620, 40500),
    ("3.1", 3600, 108000),
    ("3.2", 5120, 216000),
    ("4", 8192, 245760),
]


def level_for(w, h, fps):
    mbs = ((w + 15) // 16) * ((h + 15) // 16)
    rate = mbs * fps
    for name, maxfs, maxmbps in LEVELS:
        if mbs <= maxfs and rate <= maxmbps:
            return name
    return "4"


def probe(path):
    if FP and os.path.isfile(FP):
        out = subprocess.check_output([FP, "-v", "error", "-show_streams",
                                       "-of", "json", path])
        info = json.loads(out)
        v = a = None
        for s in info["streams"]:
            if s["codec_type"] == "video" and v is None:
                v = s
            elif s["codec_type"] == "audio" and a is None:
                a = s
        return v, a
    # no ffprobe: read what ffmpeg prints about the input
    p = subprocess.run([FF, "-hide_banner", "-i", path], capture_output=True, text=True)
    v = a = None
    import re
    for line in p.stderr.splitlines():
        if "Video:" in line and v is None:
            m = re.search(r"(\d{2,5})x(\d{2,5})", line)
            f = re.search(r"([\d.]+) fps", line)
            if m:
                v = {"width": int(m.group(1)), "height": int(m.group(2)),
                     "r_frame_rate": f.group(1) if f else "30"}
        elif "Audio:" in line and a is None:
            a = {"codec_type": "audio"}
    return v, a


def fps_of(v):
    r = str(v.get("r_frame_rate", "30"))
    if "/" in r:
        n, d = r.split("/")
        return float(n) / float(d) if float(d) else 30.0
    return float(r)


def target_box(w, h):
    """Box to encode into.

    H.264 codes in 16x16 macroblocks, so a stream whose height is not a
    multiple of 16 is really coded larger and cropped on display. The Vita
    reports the cropped size, but GXM works out where the chroma plane starts
    from the size it is given - so a cropped stream puts chroma two rows off
    and the colour smears. Encoding at exact multiples of 16 removes the
    discrepancy. Sources that are already aligned and fit on screen are left
    completely alone, which avoids a resample."""
    if w <= MAX_W and h <= MAX_H and w % 16 == 0 and h % 16 == 0:
        return w, h, False
    scale = min(MAX_W / float(w), MAX_H / float(h), 1.0)
    bw = int(round(w * scale))
    bh = int(round(h * scale))
    # Round up to the macroblock grid and letterbox into it, rather than
    # stretching, so the aspect ratio is preserved exactly.
    return ((bw + 15) // 16) * 16, ((bh + 15) // 16) * 16, True


converted = skipped = failed = 0
for root, dirs, files in os.walk(SRC):
    for name in sorted(files):
        if not name.lower().endswith(".bik"):
            continue
        src = os.path.join(root, name)
        rel = os.path.relpath(root, SRC)
        outdir = DST if rel == "." else os.path.join(DST, rel)
        if not os.path.isdir(outdir):
            os.makedirs(outdir)
        dst = os.path.join(outdir, name[:-4] + ".mp4")

        v, a = probe(src)
        if v is None:
            print("no video stream: %s" % name)
            skipped += 1
            continue

        w, h = int(v["width"]), int(v["height"])
        fps = fps_of(v)
        bw, bh, scaled = target_box(w, h)
        level = level_for(bw, bh, fps)

        if scaled:
            vf = ("scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,"
                  "pad=%d:%d:(ow-iw)/2:(oh-ih)/2,format=yuv420p" % (bw, bh, bw, bh))
        else:
            vf = "format=yuv420p"

        cmd = [FF, "-y", "-v", "error", "-i", src,
               "-vf", vf,
               "-c:v", "libx264",
               # Main profile with no 8x8 transform: the most broadly supported
               # thing the Vita decoder takes. The level is the lowest the
               # picture allows (see LEVELS above).
               "-profile:v", "main", "-level", level,
               "-preset", "slow", "-crf", "20",
               "-bf", "2", "-refs", "3",
               "-pix_fmt", "yuv420p", "-g", "60"]
        if a is not None:
            cmd += ["-c:a", "aac", "-profile:a", "aac_low",
                    "-b:a", "128k", "-ar", "48000", "-ac", "2"]
        else:
            cmd += ["-an"]
        cmd += ["-movflags", "+faststart", dst]

        r = subprocess.call(cmd)
        if r != 0 or not os.path.isfile(dst):
            print("FAILED %s" % os.path.join(rel, name))
            failed += 1
            continue

        converted += 1
        tag = "%dx%d" % (bw, bh)
        if (bw, bh) != (w, h):
            tag = "%dx%d -> %s" % (w, h, tag)
        extra = ""

        print("%-44s %-20s level %-4s %7.2f MB%s%s" % (
            os.path.join(rel, name) if rel != "." else name,
            tag, level,
            os.path.getsize(dst) / 1048576.0,
            "  +audio" if a is not None else "", extra))

print("\nconverted %d, skipped %d, failed %d" % (converted, skipped, failed))
