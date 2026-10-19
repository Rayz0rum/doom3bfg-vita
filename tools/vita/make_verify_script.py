#!/usr/bin/env python3
"""Writes the full verification's built-in script (neo/framework/VitaVerify.cpp)
to neo/framework/VitaVerify_script.inc.

usage: make_verify_script.py

The videos are the port's .mp4 files - H.264 + AAC, played by the Vita's
hardware player; there is no Bink video on the Vita - as listed in
neo/renderer/Cinematic.cpp (vitaExpectedVideos, the 51 that
tools/vita/convert_video.py makes from the retail data). The benchmark's own
scenes come from VitaBench_script.inc.

The script's commands are listed in VitaVerify.cpp and VitaBench.cpp.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
NEO = os.path.join(HERE, "..", "..", "neo", "framework")
CINEMATIC = os.path.join(HERE, "..", "..", "neo", "renderer", "Cinematic.cpp")

DOOM3 = ["mars_city1", "mc_underground", "mars_city2", "admin", "alphalabs1", "alphalabs2", "alphalabs3", "alphalabs4",
         "enpro", "commoutside", "comm1", "recycling1", "recycling2", "monorail", "delta1", "delta2a", "delta2b",
         "delta3", "delta4", "hell1", "delta5", "cpu", "cpuboss", "site3", "caverns1", "caverns2", "hellhole"]
ROE = ["erebus1", "erebus2", "erebus3", "erebus4", "erebus5", "erebus6", "phobos1", "phobos2", "phobos3", "phobos4",
       "deltax", "hell"]
LOST = ["le_enpro1", "le_enpro2", "le_underground", "le_underground2", "le_exis1", "le_exis2", "le_hell", "le_hell_post"]
# maps whose end is a boss arena with no ordinary monsters to spawn
NO_FIGHT = {"hell1", "cpuboss", "hellhole", "hell", "le_hell_post"}


def videos():
    """the .mp4 videos the Vita plays (Cinematic.cpp: vitaExpectedVideos)"""
    with open(CINEMATIC, encoding="utf-8") as fp:
        text = fp.read()
    block = text[text.index("vitaExpectedVideos[] = {"):]
    block = block[:block.index("};")]
    found = re.findall(r'"([^"]+\.mp4)"', block)
    assert len(found) >= 50, "expected the 51 videos, found %d" % len(found)
    assert not any(f.lower().endswith(".bik") for f in found)
    return found


def bench_lines():
    lines = []
    with open(os.path.join(NEO, "VitaBench_script.inc"), encoding="utf-8") as fp:
        for line in fp:
            m = re.match(r'\s*"(.*)\\n"\s*$', line.rstrip("\n"))
            if m:
                lines.append(m.group(1).replace('\\"', '"').replace("\\\\", "\\"))
    return [l for l in lines if l.strip() and l.strip() != "end"]


def campaign(title, maps, out):
    out.append("suite %s" % title)
    for i, m in enumerate(maps):
        out.append("mapcheck game/%s intro" % m)
        out.append("openspot 500")
        out.append("wait 1500")
        out.append("shots 4")
        out.append("shadowcheck")  # Carmack's reverse vs the Z-pass mix, and the inside test timed
        out.append("postcheck")  # the stretch's sharpening and soft alpha-tested edges
        if m not in NO_FIGHT:
            out.append("weapon 6")  # the chaingun, as the benchmark fights
            out.append("fightcheck 3 8")
        if i == 0 or i % 3 == 2 or i == len(maps) - 1:
            out.append("savecheck")
    out.append("menus")


def main():
    out = []
    out.append("title DOOM 3 BFG - full verification of the PS Vita port")
    out.append("log every part of the game, each check with a verdict: PASS, FAIL, WARN, INFO")
    out.append("suite boot")
    out.append("boot")
    out.append("suite menus")
    out.append("menus")
    out.append("suite languages")
    out.append("langcheck")
    out.append("suite trophies")
    out.append("trophies")
    out.append("suite friends")
    out.append("friends 250")
    out.append("netpeer")
    out.append("suite videos")
    for vfile in videos():
        out.append("videotest %s 4" % vfile)
    out.append("menus")
    campaign("DOOM 3", DOOM3, out)
    campaign("Resurrection of Evil", ROE, out)
    campaign("The Lost Mission", LOST, out)
    out.append("suite classic DOOM")
    out.append("classic doom 40 1 1 1 2 1 3 2 1 3 1 4 1")
    out.append("classic doom2 40 1 2 7 15 30")
    out.append("classic nerve 20 1 3 6 9")
    out.append("menus")
    out.append("suite multiplayer")
    for m, mode, bots in (("d3dm1", "dm", 7), ("d3dm2", "tdm", 7), ("d3dm3", "tourney", 1), ("d3dm4", "lms", 5),
                          ("d3dm5", "dm", 3), ("d3ctf1", "ctf", 7), ("d3ctf2", "ctf", 5), ("d3ctf3", "ctf", 3),
                          ("d3ctf4", "ctf", 7), ("d3xpdm1", "dm", 7), ("d3xpdm2", "tdm", 5), ("d3xpdm3", "lms", 3)):
        out.append("mp game/mp/%s %s %d 45" % (m, mode, bots))
    out.append("menus")
    out.append("suite online")
    out.append("online 60")
    out.append("suite benchmark")
    bench = bench_lines()
    out.extend(bench)
    # A/B of the job worker layout (vita_jobWorkerCores, ParallelJobList.cpp):
    # the CPU Complex fights again, still on that map, with the three core 1-2
    # workers kept on core 2 - the same fights, compared segment by segment
    first = next(i for i, l in enumerate(bench) if l.startswith('segment "cpu fight 1'))
    fights = bench[first:]
    out.append("cmd set vita_jobWorkerCores 1")
    for l in fights:
        if l.startswith('segment "cpu fight'):
            l = l.replace('segment "cpu fight', 'segment "cpu fight (job workers on core 2 only, vita_jobWorkerCores 1)', 1)
        out.append(l)
    out.append("cmd set vita_jobWorkerCores 0")
    out.append("menus")
    out.append("suite session")
    out.append("memtrend")
    out.append("boot")
    out.append("end")

    path = os.path.join(NEO, "VitaVerify_script.inc")
    with open(path, "w", newline="\n", encoding="utf-8") as fp:
        fp.write("// the full verification's built-in script (VitaVerify.cpp) - generated by tools/vita/make_verify_script.py\n")
        for l in out:
            fp.write('"%s\\n"\n' % l.replace("\\", "\\\\").replace('"', '\\"'))
    print("%d lines -> %s" % (len(out), path))


if __name__ == "__main__":
    main()
