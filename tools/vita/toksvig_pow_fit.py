"""The constants of the interaction shaders' pow() stand-in for Toksvig
(RenderProgs_GLSL.cpp vitaSpecularToksvig, base/renderprogs/interaction*.pixel).

Toksvig gives each texel an exponent s' between 4 and 10 (blue = (10 - s') / 6,
neo/renderer/Toksvig.cpp). pow( h, s' ) on the SGX is log2 + exp2 per pixel;
instead the shaders compute

    max( 1 - k * (1 - h), 0 )^m,   k = (a + b * s') / m

with m = 8 (three squarings). a and b are fitted here to the smallest worst
error of pow( h, s' ) * (s' + 1) / 11 over h in 0..1 and s' in 4.12..10.
Result: m = 8, a = 0.75, b = 0.9: 1.93/255 at worst, 1.56/255 at s' = 10
(blue 0, the full-resolution level - the stock look). The shader folds the
encoding in: k = 1.21875 - 0.675 * blue, the scale 1 - 0.5454545 * blue.

    python tools/vita/toksvig_pow_fit.py
"""
import numpy as np

H = np.linspace(0, 1, 1001)
S = np.linspace(4.12, 10, 60)


def worst(a, b, m):
    w = 0.0
    for s in S:
        k = (a + b * s) / m
        e = np.max(np.abs(H ** s - np.maximum(1 - k * (1 - H), 0) ** m)) * (s + 1) / 11
        w = max(w, e)
    k = (a + 10 * b) / m
    return w * 255, np.max(np.abs(H ** 10 - np.maximum(1 - k * (1 - H), 0) ** m)) * 255


for m in (4, 6, 8):
    best = None
    for b in np.linspace(0.85, 1.15, 31):
        for a in np.linspace(-0.5, 1.5, 41):
            w = worst(a, b, m)
            if best is None or w[0] < best[0][0]:
                best = (w, a, b)
    (w, w10), a, b = best
    print("m %d: a %.3f b %.3f - worst %.2f/255, at s' = 10 %.2f/255" % (m, a, b, w, w10))
