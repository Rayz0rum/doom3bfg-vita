"""Checks the Toksvig pass's fast paths (neo/renderer/Toksvig.cpp) on a PC
against the first, straightforward version (reference_toksvig_v1.cpp):

  - the whole-number decode (one square root a texel) against the float
    decode, on random DXT5 normal map blocks, split in two bands of rows as
    the job threads split it;
  - the blue table against the exact formula;
  - the whole-number blue fit against the float fit (same blocks, same
    targets): the blocks must come out identical, green never changed;
  - both timed (x86 timings only say which is slower, not by how much on the
    Vita's Cortex-A9).

Needs MSYS2's mingw64 g++.

    python tools/vita/toksvigtest/run.py
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
GXX = r'C:\msys64\mingw64\bin\g++.exe'

old = open(os.path.join(HERE, 'reference_toksvig_v1.cpp'), encoding='utf-8').read().replace('\r\n', '\n')
new = open(os.path.join(ROOT, 'neo', 'renderer', 'Toksvig.cpp'), encoding='utf-8').read().replace('\r\n', '\n')


def between(t, a, b):
    i = t.index(a)
    return t[i:t.index(b, i)]


common = between(old, 'static inline int Expand5', '/*\n========================\nDecodeNormalsToHalf')
oldDecode = between(old, 'static void DecodeNormalsToHalf', '/*\n========================\nFitBlue')
oldFit = between(old, 'static float FitBlue', '/*\n========================\nR_ToksvigDXT5Normals').replace('static float FitBlue(', 'static float OldFitBlue(')
newDecode = between(new, "// a level's mean normals, three 16-bit", '/*\n========================\nToksvigBlue')
newBlue = between(new, 'static float ToksvigBlue', '/*\n========================\nFitBlue')
newFit = between(new, 'static int FitBlue', '/*\n========================\nThe pass')

src = '''#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <random>
#include <algorithm>
#include <vector>
typedef unsigned char byte; typedef int16_t int16; typedef uint16_t uint16; typedef uint32_t uint32; typedef uint64_t uint64; typedef int64_t int64;
template<class T> T Min(T a, T b) { return a < b ? a : b; }
template<class T> T Max(T a, T b) { return a > b ? a : b; }
struct idMath {
  static float Sqrt(float x) { return sqrtf(x); }
  static float InvSqrt(float x) { return 1.0f / sqrtf(x); }
  static float Fabs(float x) { return fabsf(x); }
  static float Rint(float x) { return floorf(x + 0.5f); }
  static float ClampFloat(float a, float b, float x) { return x < a ? a : (x > b ? b : x); }
  static int ClampInt(int a, int b, int x) { return x < a ? a : (x > b ? b : x); }
  static float InvSqrt16(float x) { return 1.0f / sqrtf(x); }
};
''' + common + oldDecode + oldFit + newDecode + newBlue + newFit + open(os.path.join(HERE, 'main.inc'), encoding='utf-8').read()
cpp = os.path.join(HERE, 'test.cpp')
exe = os.path.join(HERE, 'test.exe')
open(cpp, 'w', encoding='utf-8').write(src)
env = dict(os.environ, PATH=os.path.dirname(GXX) + os.pathsep + os.environ.get('PATH', ''))
r = subprocess.run([GXX, '-O2', '-std=c++17', '-o', exe, cpp], env=env)
if r.returncode != 0:
    sys.exit(1)
sys.exit(subprocess.run([exe], env=env).returncode)
