#!/usr/bin/env python3
"""Makes the Vita's software video streams (.roq) for the screens that play at
the same time as another video.

usage: make_roq.py <ffmpeg.exe> [game base folder] [output base folder]
   (defaults: the GOG install, build-vita/vita_data/base)

The Vita's hardware H.264 decoder serves one player at a time (the VitaSDK:
SceVideodecQueryInitInfoHwAvcdec.numOfStreams "must be set to 1"; on hardware,
a second player never gives a frame - vita_avplayer.cpp). Stock BFG decoded
Bink in software and played any number at once, and the game data does play
some together:
  - mc_underground's bank of invasion monitors: invasion_roq1-5 start on
    overlapping timers (trigger_relay delays), screen 2 running 15.6 s beside
    screens 3-5;
  - recycling2: mainscreen, screen2 and screen4 are switched on together
    (map_recycling2.script), each GUI (pump1b, temp_one, temp_two) drawing both
    of Bert's videos.
Elsewhere only one video is ever visible at a time (the mars_city1 intro's
tracking screen keeps the finished monoflyby under a static image while
dropship and ship play) and the hardware player is handed over
(Vita_CinematicEvict, Cinematic.cpp).

And the mars_city1 screens, as a safety net: sceAvPlayer now and then gives a
player that never starts (the fourth verification run: dropship.mp4, alone);
a video with a .roq that has no frame 1.5 s after its open plays the .roq -
the intro's tracking screen (monoflyby, "Tracking SAT 12 / Livestream") is
2.7 s long. These are bigger on screen: 256 pixels wide.

So only these screens get a .roq: when the hardware player is another video's,
they play it with the engine's own RoQ decoder - the format DOOM 3 shipped
them in - at 128 pixels wide and 15 frames a second (~80 KB/s: these are small
monitors in the scene). Copy the output into ux0:data/doom3bfg/base/ (beside
the .mp4 files).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FF = sys.argv[1] if len(sys.argv) > 1 else "ffmpeg"
BASE = sys.argv[2] if len(sys.argv) > 2 else r"C:\Games\DOOM 3 BFG\base"
OUT = sys.argv[3] if len(sys.argv) > 3 else os.path.join(HERE, "..", "..", "build-vita", "vita_data", "base")

SAFETY_NET = [
    "video/marscity/dropship",
    "video/marscity/monoflyby",
    "video/marscity/ship",
    "video/marscity/sci_team_request",
    "video/marscity/sarge_mission_brief",
]
SAFETY_WIDTH = 256
SIMULTANEOUS = [
    "video/mc_underground/inv1sm",
    "video/mc_underground/inv2sm",
    "video/mc_underground/inv3",
    "video/mc_underground/inv4sm",
    "video/mc_underground/inv5sm",
    "video/mc_underground/inv6sm",
    "video/mc_underground/inv7sm",
    "video/recycling2/cs_bert01_composite",
    "video/recycling2/cs_bert02_composite",
]
WIDTH = 128
FPS = 15


def main():
    total = 0
    for stem, width in [(x, SAFETY_WIDTH) for x in SAFETY_NET] + [(x, WIDTH) for x in SIMULTANEOUS]:
        src = os.path.join(BASE, stem + ".bik")
        dst = os.path.join(OUT, stem + ".roq")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        # RoQ wants both sides a multiple of 16
        vf = "scale=%d:-16:flags=lanczos" % width
        subprocess.check_call([FF, "-hide_banner", "-v", "error", "-y", "-i", src, "-an", "-r", str(FPS), "-vf", vf, "-c:v", "roqvideo", dst])
        size = os.path.getsize(dst)
        total += size
        print("%-45s %7d KB" % (stem + ".roq", size // 1024))
    print("%d files, %.1f MB -> %s" % (len(SAFETY_NET) + len(SIMULTANEOUS), total / 1048576.0, os.path.abspath(OUT)))


if __name__ == "__main__":
    main()
