"""Checks the Vita's restart-free SortWallEdges merge loop against id's
original on random edge sets: cuts the block out of AAS_pathing.cpp as it is,
builds tools/vita/sortwalledges_test.cpp with MSYS2's mingw64 g++ and runs it.

    python tools/vita/sortwalledges_test.py
"""
import io, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
GXX = r'C:\msys64\mingw64\bin\g++.exe'

src = io.open(os.path.join(ROOT, 'neo', 'd3xp', 'ai', 'AAS_pathing.cpp'), encoding='utf-8').read().replace('\r\n', '\n')
s = src.index('// VITA_SORT_WALL_EDGES_BEGIN')
e = src.index('// VITA_SORT_WALL_EDGES_END')
io.open(os.path.join(HERE, 'sortwalledges_under_test.inc'), 'w', encoding='utf-8').write(src[s:e])

env = dict(os.environ, PATH=os.path.dirname(GXX) + os.pathsep + os.environ.get('PATH', ''))
exe = os.path.join(HERE, 'sortwalledges_test.exe')
r = subprocess.run([GXX, '-O2', '-std=c++17', '-o', exe, os.path.join(HERE, 'sortwalledges_test.cpp')], cwd=HERE, env=env)
if r.returncode != 0:
    sys.exit(1)
sys.exit(subprocess.run([exe], cwd=HERE, env=env).returncode)
