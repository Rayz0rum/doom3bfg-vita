#!/usr/bin/env python3
"""A frame-rate report from a Vita run's benchmark_results.txt (the L+R
benchmark or the full verification).

usage: fps_report.py <benchmark_results.txt> [report.md]

Every segment of the run (a map's opening cutscene, looking around, a fight,
the benchmark's scenes...) with its frame rate, frame times (average, median,
95th / 99th percentile, worst), how often a frame was on screen on time (2
vblanks = 30 fps) or late, and where the time went (game, front end, back end,
GPU wait). Then a verdict per segment, per map and per campaign, by the
user's rules (2026-10-19): 30 fps is the norm and dips are acceptable; the
20 fps cap is playable but the exception, and must not last over 15 s
(preferably 10); under 20 fps - or the cap held too long - is unplayable.

  30 fps       on time (2 vblanks) for most frames, under 2% below 20 fps
  dips         under 5% of frames below 20 fps, no 20 fps stretch over 10 s
  20 too long  a 20 fps stretch over 10 s (15 s: unplayable)
  unplayable   5% or more of the frames below 20 fps (shown for 4+ vblanks)

A frame on screen for 3 vblanks (50 ms) is a 20 fps frame; for 4 or more,
under 20 fps. The 20 fps stretches come from the run's "20 fps cap" lines
(r48 on) or console.txt beside it ("frame cap 30 -> 20").
"""
import re
import sys
from collections import OrderedDict


def verdict(fps, p95, under20=None, longest20=0.0):
    """under20: % of frames shown for 4+ vblanks; longest20: longest 20 fps stretch (s)"""
    if under20 is None:
        under20 = 0.0 if fps >= 25 else 10.0
    if under20 >= 5.0 or fps < 20.0 or longest20 > 15.0:
        return "unplayable"
    if longest20 > 10.0:
        return "20 too long"
    if under20 >= 2.0 or fps < 27.0:
        return "dips"
    return "30 fps"


def cap_stretches(results_path):
    """{map: [seconds at 20, ...]} from the results' cap lines, else console.txt beside it"""
    import os
    per = {}
    mapname = "?"
    found = False
    with open(results_path, encoding="utf-8", errors="replace") as fp:
        for line in fp:
            m = re.search(r"verify map (game/\w+)", line) or re.search(r'segment \d+: "(game/\w+)', line)
            if m:
                mapname = m.group(1)
            m = re.search(r"20 fps cap: off after ([\d.]+) s", line)
            if m:
                found = True
                per.setdefault(mapname, []).append(float(m.group(1)))
    if found:
        return per
    console = os.path.join(os.path.dirname(os.path.abspath(results_path)), "console.txt")
    if not os.path.exists(console):
        return per
    t = 0
    start = None
    with open(console, encoding="utf-8", errors="replace") as fp:
        for line in fp:
            m = re.match(r"\[ *(\d+) ms\]", line)
            if m:
                t = int(m.group(1))
            m = re.match(r"Map: (\S+)", line)
            if m:
                mapname = m.group(1)
            if "frame cap 30 -> 20" in line:
                start = (t, mapname)
            elif ("frame cap 20 -> 30" in line or "frame cap back to 30" in line) and start:
                per.setdefault(start[1], []).append((t - start[0]) / 1000.0)
                start = None
    return per


def parse(path):
    segs = []
    suite = ""
    cur = None
    with open(path, encoding="utf-8", errors="replace") as fp:
        for line in fp:
            m = re.match(r"bench ######## suite: (.*) ########", line)
            if m:
                suite = m.group(1)
                continue
            m = re.match(r'bench segment (\d+) "(.*)" summary:', line)
            if m:
                cur = {"n": int(m.group(1)), "name": m.group(2), "suite": suite}
                segs.append(cur)
                continue
            if cur is None:
                continue
            m = re.search(r"frames (\d+) in ([\d.]+) s = ([\d.]+) fps \| frame avg ([\d.]+) ms, median ([\d.]+), 95th ([\d.]+), 99th ([\d.]+), worst ([\d.]+)", line)
            if m:
                cur.update(frames=int(m.group(1)), secs=float(m.group(2)), fps=float(m.group(3)), avg=float(m.group(4)),
                           med=float(m.group(5)), p95=float(m.group(6)), p99=float(m.group(7)), worst=float(m.group(8)))
                continue
            m = re.search(r"for 2 vblanks \(30 fps, on time\) ([\d.]+)% \| 3 \(a missed vblank: 50 ms on screen\) ([\d.]+)% \| 4 ([\d.]+)% \| 5 or more ([\d.]+)%", line)
            if m:
                cur.update(ontime=float(m.group(1)), late1=float(m.group(2)), late2=float(m.group(3)) + float(m.group(4)))
                continue
            m = re.search(r"per frame avg \(worst\): game ([\d.]+) \(([\d.]+)\) ms in ([\d.]+) tics \| front end ([\d.]+) \(([\d.]+)\) \| back end ([\d.]+) \(([\d.]+)\) \| GPU/swap wait ([\d.]+)", line)
            if m:
                cur.update(game=float(m.group(1)), front=float(m.group(4)), back=float(m.group(6)), gpu=float(m.group(8)))
                continue
            m = re.search(r"3D view: step avg ([\d.]+), lowest size reached (\d+x\d+)", line)
            if m:
                cur.update(resstep=float(m.group(1)), lowres=m.group(2))
    return [s for s in segs if "fps" in s]


def map_of(name):
    m = re.match(r"(game/[\w/]+|[\w/]+?):", name)
    return m.group(1) if m else name.split(":")[0]


def bound(s):
    # what limits the frame: the game/draw thread (game + front end) or the main thread (back end + GPU)
    draw = s.get("game", 0) + s.get("front", 0)
    main = s.get("back", 0) + s.get("gpu", 0)
    return "game/draw thread" if draw > main else "render back end / GPU"


def main():
    allsegs = parse(sys.argv[1])
    # Runs before r47 counted the test's own screenshots (~300 ms of JPEG
    # encoding each, 4 per "looking around" segment) as game frames: those
    # segments are listed but left out of the per-map and per-part figures
    # unless the run says the frames were skipped.
    with open(sys.argv[1], encoding="utf-8", errors="replace") as fp:
        skipped = "frames skipped for the test's screenshots" in fp.read()
    segs = [x for x in allsegs if skipped or "looking around" not in x["name"]]
    excluded = [x for x in allsegs if x not in segs]
    out = []
    w = out.append
    w("# DOOM 3 BFG (PS Vita) - frame rate report")
    w("")
    w("From `%s`: %d measured segments. Target 30 fps (33.4 ms a frame)." % (sys.argv[1].replace("\\", "/").split("/")[-1], len(segs)))
    w("Verdicts (the user's rules): **30 fps** - the norm, dips allowed (under 2% of frames below 20 fps); "
      "**dips** - acceptable (under 5% below 20 fps, no 20 fps stretch over 10 s); **20 too long** - a 20 fps "
      "stretch over 10 s; **unplayable** - 5% or more of the frames below 20 fps, or a 20 fps stretch over 15 s.")
    w("")
    # overall
    tot_frames = sum(s["frames"] for s in segs)
    tot_secs = sum(s["secs"] for s in segs)
    caps = cap_stretches(sys.argv[1])
    counts = OrderedDict((k, 0) for k in ("30 fps", "dips", "20 too long", "unplayable"))
    for s in segs:
        s["verdict"] = verdict(s["fps"], s["p95"], s.get("late2"))
        counts[s["verdict"]] += 1
    w("## Overall")
    w("")
    w("- %d frames in %.0f s of measured play: **%.1f fps** on average" % (tot_frames, tot_secs, tot_frames / max(tot_secs, 0.1)))
    w("- segments: " + ", ".join("%d %s" % (v, k) for k, v in counts.items()))
    worst = sorted(segs, key=lambda s: s["fps"])[:8]
    w("- slowest segments: " + "; ".join("%s %.1f fps" % (s["name"], s["fps"]) for s in worst))
    allcaps = [d for ds in caps.values() for d in ds]
    if allcaps:
        w("- 20 fps cap: %d stretches, %.0f s in all; longest %.1f s; %d over 10 s, %d over 15 s" % (
            len(allcaps), sum(allcaps), max(allcaps), len([d for d in allcaps if d > 10]), len([d for d in allcaps if d > 15])))
    w("")
    # per campaign / suite
    suites = OrderedDict()
    for s in segs:
        suites.setdefault(s["suite"] or "(no suite)", []).append(s)
    w("## Per part of the game")
    w("")
    w("| part | segments | fps (frame-weighted) | median frame | 95th pct | on time | verdict |")
    w("|---|---|---|---|---|---|---|")
    for name, ss in suites.items():
        fr = sum(x["frames"] for x in ss)
        sec = sum(x["secs"] for x in ss)
        fps = fr / max(sec, 0.1)
        med = sorted(x["med"] for x in ss)[len(ss) // 2]
        p95 = sum(x["p95"] * x["frames"] for x in ss) / max(fr, 1)
        on = sum(x.get("ontime", 0) * x["frames"] for x in ss) / max(fr, 1)
        u20 = sum(x.get("late2", 0) * x["frames"] for x in ss) / max(fr, 1)
        w("| %s | %d | %.1f | %.1f ms | %.1f ms | %.0f%% | %s |" % (name, len(ss), fps, med, p95, on, verdict(fps, p95, u20)))
    w("")
    # per map
    maps = OrderedDict()
    for s in segs:
        maps.setdefault(map_of(s["name"]), []).append(s)
    w("## Per map")
    w("")
    w("| map | fps | 95th pct | worst frame | at 30 (on time) | at 20 (3 vblanks) | under 20 (4+) | longest 20 fps stretch | limited by | verdict |")
    w("|---|---|---|---|---|---|---|---|---|---|")
    for name, ss in maps.items():
        fr = sum(x["frames"] for x in ss)
        sec = sum(x["secs"] for x in ss)
        fps = fr / max(sec, 0.1)
        p95 = sum(x["p95"] * x["frames"] for x in ss) / max(fr, 1)
        worst_f = max(x["worst"] for x in ss)
        on = sum(x.get("ontime", 0) * x["frames"] for x in ss) / max(fr, 1)
        l1 = sum(x.get("late1", 0) * x["frames"] for x in ss) / max(fr, 1)
        l2 = sum(x.get("late2", 0) * x["frames"] for x in ss) / max(fr, 1)
        biggest = max(ss, key=lambda x: x["frames"])
        longest = max(caps.get(name, [0.0]))
        w("| %s | %.1f | %.1f ms | %.0f ms | %.0f%% | %.0f%% | %.1f%% | %s | %s | %s |" % (name, fps, p95, worst_f, on, l1, l2,
            ("%.1f s" % longest) if longest > 0 else "-", bound(biggest), verdict(fps, p95, l2, longest)))
    w("")
    # A/B pairs: the same segment run again with a setting changed, named
    # "<name> (..., vita_x N) <rest>" (make_verify_script.py) - side by side
    pairs = []
    for b in segs:
        m = re.match(r'(.*?) \(([^()]*\bvita_\w+ [-\d.]+)\)(.*)$', b["name"])
        if not m:
            continue
        base = (m.group(1) + m.group(3)).strip()
        a = next((x for x in segs if x["name"] == base or x["name"].replace(":", "") == base.replace(":", "")), None)
        if a is not None:
            pairs.append((a, b, m.group(2)))
    if pairs:
        w("## A/B: the same scene with a setting changed")
        w("")
        w("| scene | setting | fps before | fps with it | median before | median with it | 95th before | 95th with it |")
        w("|---|---|---|---|---|---|---|---|")
        for a, b, what in pairs:
            w("| %s | %s | %.1f | %.1f | %.1f ms | %.1f ms | %.1f ms | %.1f ms |" % (a["name"].replace("|", "/"), what, a["fps"], b["fps"], a["med"], b["med"], a["p95"], b["p95"]))
        w("")
    if excluded:
        w("## Left out of the figures above")
        w("")
        w("These segments took the test's screenshots while being measured (each ~300 ms of JPEG encoding counted as a "
          "game frame - fixed in r47), so their frame rate is the test's, not the game's:")
        w("")
        for x in excluded:
            x["verdict"] = verdict(x["fps"], x["p95"], x.get("late2"))
            w("- %s: %.1f fps as measured, median frame %.1f ms (the median is not skewed by the pauses)" % (x["name"], x["fps"], x["med"]))
        w("")
    w("## Every segment")
    w("")
    w("| # | segment | fps | avg | median | 95th | 99th | worst | on time | game | front end | back end | GPU wait | 3D res (lowest) | verdict |")
    w("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for s in allsegs:
        w("| %d | %s | %.1f | %.1f | %.1f | %.1f | %.1f | %.0f | %.0f%% | %.1f | %.1f | %.1f | %.1f | %s | %s |" % (
            s["n"], s["name"].replace("|", "/"), s["fps"], s["avg"], s["med"], s["p95"], s["p99"], s["worst"], s.get("ontime", 0),
            s.get("game", 0), s.get("front", 0), s.get("back", 0), s.get("gpu", 0), s.get("lowres", "?"), s["verdict"]))
    text = "\n".join(out) + "\n"
    if len(sys.argv) > 2:
        # the output must be a report: a log passed here by mistake was
        # overwritten once (console.txt, 2026-10-19)
        if not sys.argv[2].lower().endswith(".md"):
            raise SystemExit("fps_report.py: the report must be a .md file, not %s - nothing written" % sys.argv[2])
        with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as fp:
            fp.write(text)
        print("report: %s (%d segments)" % (sys.argv[2], len(segs)))
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
