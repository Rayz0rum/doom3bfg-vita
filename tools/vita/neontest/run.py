"""Checks the Vita's NEON code paths on a PC: the NEON intrinsics they use are
emulated in scalar C (neon_emu.h), the blocks are cut out of the engine
sources as they are, and each is compared with the scalar engine code on
random data. Needs MSYS2's mingw64 g++.

    python tools/vita/neontest/run.py
"""
import io, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
GXX = r'C:\msys64\mingw64\bin\g++.exe'


def cut(src, start, last_function):
    t = io.open(os.path.join(ROOT, src), encoding='utf-8').read().replace('\r\n', '\n')
    s = t.index(start)
    e = t.index('#endif\n', t.index(last_function)) + len('#endif\n')
    return t[s:e]


blocks = {
    'block_under_test.inc': cut('neo/renderer/jobs/dynamicshadowvolume/DynamicShadowVolume.cpp',
                                '#if defined(__vita__) && defined(__ARM_NEON__)\n/*\n=====================\nVita NEON shadow volume setup',
                                'static void Vita_SkinPositions_NEON'),
    'cullbits_under_test.inc': cut('neo/renderer/jobs/ShadowShared.cpp',
                                '#if defined(__vita__) && defined(__ARM_NEON__)\n#include <arm_neon.h>',
                                'static void Vita_ShadowVolumeCullBits_NEON'),
    'skin_under_test.inc': '#if defined(__vita__) && defined(__ARM_NEON__)\n' + cut('neo/renderer/Model_md5.cpp',
                                '/*\n============\nVita_SkinColumns',
                                'static void Vita_TransformPositions_NEON'),
}
# the LCP kernels: Multiply_SIMD through LowerTriangularSolveTranspose_SIMD
lcp = io.open(os.path.join(ROOT, 'neo/idlib/math/Lcp.cpp'), encoding='utf-8').read().replace('\r\n', '\n')
blocks['lcp_under_test.inc'] = lcp[lcp.index('/*\n========================\nMultiply_SIMD'):lcp.index('/*\n========================\nUpperTriangularSolve_SIMD')]
# render model traces' sidedness bits (tr_trace.cpp)
blocks['tracecull_under_test.inc'] = cut('neo/renderer/tr_trace.cpp', '#if defined(__vita__) && defined(__ARM_NEON__)\n#include <arm_neon.h>', 'static void Vita_TraceCullBitsPositions_NEON')
# TransformJoints' NEON loop (Simd_Generic.cpp): the body between the #if and its return
sg = io.open(os.path.join(ROOT, 'neo/idlib/math/Simd_Generic.cpp'), encoding='utf-8').read().replace('\r\n', '\n')
f = sg.index('void VPCALL idSIMD_Generic::TransformJoints')
s = sg.index('#if defined(__vita__) && defined(__ARM_NEON__)\n', f) + len('#if defined(__vita__) && defined(__ARM_NEON__)\n')
e = sg.index('    return;\n#endif', s)
blocks['joints_under_test.inc'] = sg[s:e]
# the shadow jobs' line-versus-expanded-triangle test (ShadowShared.cpp), as the engine has it
ss = io.open(os.path.join(ROOT, 'neo/renderer/jobs/ShadowShared.cpp'), encoding='utf-8').read().replace('\r\n', '\n')
blocks['sphere_under_test.inc'] = ss[ss.index('static float R_SegmentToSegmentDistanceSquare'):ss.index('/*\n===================\nR_ViewInsideShadowVolume')]
# the render matrix kernels (RenderMatrix.cpp), between their markers
rm = io.open(os.path.join(ROOT, 'neo/idlib/geometry/RenderMatrix.cpp'), encoding='utf-8').read().replace('\r\n', '\n')
blocks['rendermatrix_under_test.inc'] = rm[rm.index('// ---- Vita NEON render matrix kernels'):rm.index('// ---- end of the Vita NEON render matrix kernels')]
for name, text in blocks.items():
    text = text.replace('#include <arm_neon.h>\n', '') # neon_emu.h stands in for it
    io.open(os.path.join(HERE, name), 'w', encoding='utf-8').write(text)

env = dict(os.environ, PATH=os.path.dirname(GXX) + os.pathsep + os.environ.get('PATH', ''))
ok = True
for test in ('test_shadow', 'test_skin', 'test_cullbits', 'test_lcp', 'test_tracecull', 'test_joints', 'test_inside', 'test_rendermatrix'):
    exe = os.path.join(HERE, test + '.exe')
    r = subprocess.run([GXX, '-O1', '-ffp-contract=off', '-std=c++17', '-o', exe, os.path.join(HERE, test + '.cpp')], cwd=HERE, env=env)
    if r.returncode != 0:
        ok = False
        continue
    r = subprocess.run([exe], cwd=HERE, env=env)
    ok = ok and r.returncode == 0
sys.exit(0 if ok else 1)
