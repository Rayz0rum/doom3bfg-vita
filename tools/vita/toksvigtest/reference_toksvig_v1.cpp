/*
===========================================================================

Toksvig normal map filtering for DOOM 3 BFG's DXT5 normal maps (PS Vita port)

A mip texel of a normal map stands for many surface normals. Renormalised,
it keeps their mean direction and loses how much they disagreed, so a bumpy
surface far away still gets the sharp highlight of a flat one: the highlight
sparkles and crawls from pixel to pixel as the view moves - specular
aliasing, which no MSAA touches. Toksvig (2005): the length |Na| of the
UNnormalised mean normal measures that disagreement, and a specular exponent
s becomes s' = s * |Na| / (|Na| + s * (1 - |Na|)), the highlight wider and
dimmer where the bumps would have scattered it.

BFG's normal maps are DXT5 with X in the alpha block and Y in the colour
block's green; the colour block's red and blue are zero in every retail map
(tools: scratchpad bump_probe.py over _common and the maps' containers). Blue
now carries (10 - s') / 6, s' the exponent Toksvig gives the shaders' fixed 10
(s' from 10 down to 4, |Na| down to 0.87; shorter means clip), so 0 - every
retail texel, and every full-resolution one here - means "a single normal,
nothing to widen". The exponent is worked out here rather than per pixel: the
shader turns blue into its pow() approximation's constant with one mad.

At load, before upload (Image_load.cpp): the top level uploaded is decoded to
normals, those are averaged down the pyramid as UNnormalised vectors (each
level's mean is the mean of the top level's normals under it, exactly), and
each lower level's blue endpoints are fitted to its |Na| with the block's own
colour indices. Green's endpoints and the indices are never touched, so the
normal decodes bit for bit as before - checked block by block, counted in the
log. A block's endpoint order (c0 > c1 or not) is kept too: with equal green
endpoints blue decides it, and a decoder applying the three-colour rule to
DXT5 would otherwise read index 3 differently.

The interaction shaders (RenderProgs_GLSL.cpp, the Vita's; base/renderprogs
for the PC) read blue back: s' = 10 - 6 b.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "tr_local.h"
#include "BinaryImage.h"

idCVar r_toksvig("r_toksvig", "1", CVAR_RENDERER | CVAR_BOOL, "Toksvig filtering of the normal maps' mip levels (widens far highlights instead of letting them sparkle); applied when a normal map loads");

// for the log (Image_load.cpp prints them with the level load's image line)
idSysInterlockedInteger r_toksvigMaps;
idSysInterlockedInteger r_toksvigLevels;
idSysInterlockedInteger r_toksvigGreenChanged; // must stay 0
static uint64 toksvigUs = 0;
static double toksvigFitErr = 0.0;
static int64 toksvigFitTexels = 0;
static double toksvigLenSum[4];
static int64 toksvigLenCount[4];
static idSysMutex toksvigStatsLock;

static inline int Expand5(int v)
{
    return (v << 3) | (v >> 2);
}

static inline int Expand6(int v)
{
    return (v << 2) | (v >> 4);
}

// the four greens of a colour block, four-colour mode (always, in DXT5)
static void GreenPalette(uint16 c0, uint16 c1, int g[4])
{
    g[0] = Expand6((c0 >> 5) & 63);
    g[1] = Expand6((c1 >> 5) & 63);
    g[2] = (2 * g[0] + g[1]) / 3;
    g[3] = (g[0] + 2 * g[1]) / 3;
}

// the eight alphas of a DXT5 alpha block
static void AlphaPalette(const byte* blk, int a[8])
{
    a[0] = blk[0];
    a[1] = blk[1];
    if (a[0] > a[1]) {
        for (int i = 1; i < 7; i++) {
            a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
        }
    } else {
        for (int i = 1; i < 5; i++) {
            a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
        }
        a[6] = 0;
        a[7] = 255;
    }
}

/*
========================
DecodeNormalsToHalf

The level's normals, accumulated into the next level down: sums of unit
vectors and counts at (x >> 1, y >> 1).
========================
*/
static void DecodeNormalsToHalf(const byte* data, int w, int h, float* sums, int hw, int hh)
{
    const int bw = (w + 3) / 4;
    const int bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            const byte* blk = data + (by * bw + bx) * 16;
            int a[8];
            AlphaPalette(blk, a);
            uint64 aBits = 0;
            for (int i = 0; i < 6; i++) {
                aBits |= (uint64)blk[2 + i] << (8 * i);
            }
            const uint16 c0 = blk[8] | (blk[9] << 8);
            const uint16 c1 = blk[10] | (blk[11] << 8);
            int g[4];
            GreenPalette(c0, c1, g);
            const uint32 cBits = blk[12] | (blk[13] << 8) | (blk[14] << 16) | ((uint32)blk[15] << 24);
            for (int t = 0; t < 16; t++) {
                const int x = bx * 4 + (t & 3);
                const int y = by * 4 + (t >> 2);
                if (x >= w || y >= h) {
                    continue;
                }
                // the shader's decode: xy = bump.wy - 0.5, z from them
                const float nx = a[(aBits >> (3 * t)) & 7] / 255.0f - 0.5f;
                const float ny = g[(cBits >> (2 * t)) & 3] / 255.0f - 0.5f;
                const float nz = idMath::Sqrt(idMath::Fabs(nx * nx + ny * ny - 0.25f));
                const float len = idMath::Sqrt(nx * nx + ny * ny + nz * nz);
                const float inv = len > 1e-6f ? 1.0f / len : 0.0f;
                const int hx = Min(x >> 1, hw - 1);
                const int hy = Min(y >> 1, hh - 1);
                float* s = sums + (hy * hw + hx) * 4;
                s[0] += nx * inv;
                s[1] += ny * inv;
                s[2] += nz * inv;
                s[3] += 1.0f;
            }
        }
    }
}

/*
========================
FitBlue

Blue endpoints for one block, given its own colour indices: the least
squares fit of the targets (0..255), rounded, then the best of the
neighbouring 5-bit pairs by the real decode, keeping the endpoint order.
Returns the summed absolute error (8-bit units) and the texels counted.
========================
*/
static float FitBlue(byte* blk, const float target[16], const bool inside[16], int* counted, int* greenChanged)
{
    uint16 c0 = blk[8] | (blk[9] << 8);
    uint16 c1 = blk[10] | (blk[11] << 8);
    const uint32 cBits = blk[12] | (blk[13] << 8) | (blk[14] << 16) | ((uint32)blk[15] << 24);
    const bool wasGreater = c0 > c1;
    int gBefore[4];
    GreenPalette(c0, c1, gBefore);

    // Green flat across the block (equal green - and red - endpoints, 4.7% of
    // the retail blocks): every index decodes the same green, so the indices
    // are free for blue. Blue spans the block's range, each texel takes the
    // nearest of indices 0-2 - never 3, which a decoder applying the
    // three-colour rule (these blocks have c0 <= c1) would read as black,
    // green included - and b0 <= b1 keeps the endpoint order.
    if (((c0 >> 5) & 63) == ((c1 >> 5) & 63) && (c0 >> 11) == (c1 >> 11)) {
        if (wasGreater) {
            return 0.0f; // (never in the retail data) left as it is
        }
        float lo = 255.0f, hi = 0.0f;
        int n = 0;
        for (int t = 0; t < 16; t++) {
            if (inside[t]) {
                lo = Min(lo, target[t]);
                hi = Max(hi, target[t]);
                n++;
            }
        }
        if (n == 0) {
            return 0.0f;
        }
        const int e0 = idMath::ClampInt(0, 31, (int)idMath::Rint(lo * (31.0f / 255.0f)));
        const int e1 = idMath::ClampInt(e0, 31, (int)idMath::Rint(hi * (31.0f / 255.0f)));
        int pal[3];
        pal[0] = Expand5(e0);
        pal[1] = Expand5(e1);
        pal[2] = (2 * pal[0] + pal[1]) / 3;
        uint32 bits = 0;
        float err = 0.0f;
        for (int t = 0; t < 16; t++) {
            int best = 0;
            if (inside[t]) {
                float bestD = 1e30f;
                for (int k = 0; k < 3; k++) {
                    const float d = idMath::Fabs(pal[k] - target[t]);
                    if (d < bestD) {
                        bestD = d;
                        best = k;
                    }
                }
                err += bestD;
            }
            bits |= (uint32)best << (2 * t);
        }
        c0 = (uint16)((c0 & ~31) | e0);
        c1 = (uint16)((c1 & ~31) | e1);
        int gAfter[4];
        GreenPalette(c0, c1, gAfter);
        if (memcmp(gBefore, gAfter, sizeof(gBefore)) != 0 || (c0 > c1) != wasGreater) {
            (*greenChanged)++;
            return 0.0f; // never written
        }
        blk[8] = (byte)c0;
        blk[9] = (byte)(c0 >> 8);
        blk[10] = (byte)c1;
        blk[11] = (byte)(c1 >> 8);
        blk[12] = (byte)bits;
        blk[13] = (byte)(bits >> 8);
        blk[14] = (byte)(bits >> 16);
        blk[15] = (byte)(bits >> 24);
        *counted += n;
        return err;
    }

    // value = w * b0 + (1 - w) * b1 per index (four-colour mode)
    static const float weight[4] = { 1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f };
    double s00 = 0, s01 = 0, s11 = 0, t0 = 0, t1 = 0;
    int n = 0;
    double mean = 0;
    for (int t = 0; t < 16; t++) {
        if (!inside[t]) {
            continue;
        }
        const float wgt = weight[(cBits >> (2 * t)) & 3];
        const double v = target[t] * (31.0 / 255.0);
        s00 += wgt * wgt;
        s01 += wgt * (1.0 - wgt);
        s11 += (1.0 - wgt) * (1.0 - wgt);
        t0 += wgt * v;
        t1 += (1.0 - wgt) * v;
        mean += v;
        n++;
    }
    if (n == 0) {
        return 0.0f;
    }
    mean /= n;
    double b0 = mean, b1 = mean;
    const double det = s00 * s11 - s01 * s01;
    if (idMath::Fabs((float)det) > 1e-9f) {
        b0 = (t0 * s11 - t1 * s01) / det;
        b1 = (t1 * s00 - t0 * s01) / det;
    }

    int bestB0 = 0, bestB1 = 0;
    float bestErr = 1e30f;
    bool found = false;
    auto evaluate = [&](int e0, int e1) {
        const uint16 n0 = (uint16)((c0 & ~31) | e0);
        const uint16 n1 = (uint16)((c1 & ~31) | e1);
        if ((n0 > n1) != wasGreater) {
            return; // the endpoint order is kept
        }
        int pal[4];
        pal[0] = Expand5(e0);
        pal[1] = Expand5(e1);
        pal[2] = (2 * pal[0] + pal[1]) / 3;
        pal[3] = (pal[0] + 2 * pal[1]) / 3;
        float err = 0.0f;
        for (int t = 0; t < 16; t++) {
            if (inside[t]) {
                err += idMath::Fabs(pal[(cBits >> (2 * t)) & 3] - target[t]);
            }
        }
        if (err < bestErr) {
            bestErr = err;
            bestB0 = e0;
            bestB1 = e1;
            found = true;
        }
    };
    const int r0 = idMath::ClampInt(0, 31, (int)idMath::Rint((float)b0));
    const int r1 = idMath::ClampInt(0, 31, (int)idMath::Rint((float)b1));
    for (int d0 = -1; d0 <= 1; d0++) {
        for (int d1 = -1; d1 <= 1; d1++) {
            const int e0 = r0 + d0, e1 = r1 + d1;
            if (e0 >= 0 && e0 <= 31 && e1 >= 0 && e1 <= 31) {
                evaluate(e0, e1);
            }
        }
    }
    if (!found) {
        // the fit wanted the other order: every pair (a rare block)
        for (int e0 = 0; e0 < 32; e0++) {
            for (int e1 = 0; e1 < 32; e1++) {
                evaluate(e0, e1);
            }
        }
    }
    if (!found) {
        return 0.0f; // nothing keeps the order: the block stays as it was
    }
    c0 = (uint16)((c0 & ~31) | bestB0);
    c1 = (uint16)((c1 & ~31) | bestB1);
    int gAfter[4];
    GreenPalette(c0, c1, gAfter);
    if (memcmp(gBefore, gAfter, sizeof(gBefore)) != 0) {
        (*greenChanged)++;
        return 0.0f; // never written
    }
    blk[8] = (byte)c0;
    blk[9] = (byte)(c0 >> 8);
    blk[10] = (byte)c1;
    blk[11] = (byte)(c1 >> 8);
    *counted += n;
    return bestErr;
}

/*
========================
R_ToksvigDXT5Normals

baseLevel: the first level of the file that is uploaded (the Vita may skip
the top ones). Returns the number of levels written.
========================
*/
int R_ToksvigDXT5Normals(idBinaryImage& im, int baseLevel)
{
    if (!r_toksvig.GetBool()) {
        return 0;
    }
    const uint64 t0 = Sys_Microseconds();
    // the 2D levels by level number
    int byLevel[16];
    for (int i = 0; i < 16; i++) {
        byLevel[i] = -1;
    }
    for (int i = 0; i < im.NumImages(); i++) {
        const bimageImage_t& img = im.GetImageHeader(i);
        if (img.destZ == 0 && img.level >= 0 && img.level < 16 && im.GetImageData(i) != NULL) {
            byLevel[img.level] = i;
        }
    }
    if (baseLevel < 0 || baseLevel >= 15 || byLevel[baseLevel] < 0) {
        return 0;
    }
    const bimageImage_t& base = im.GetImageHeader(byLevel[baseLevel]);
    int w = base.width, h = base.height;
    if (w < 1 || h < 1 || base.dataSize < ((w + 3) / 4) * ((h + 3) / 4) * 16) {
        return 0;
    }
    int hw = Max(1, w >> 1), hh = Max(1, h >> 1);
    float* sums = (float*)Mem_ClearedAlloc(hw * hh * 4 * sizeof(float), TAG_TEMP);
    if (sums == NULL) {
        return 0; // no room: the map loads unfiltered
    }
    DecodeNormalsToHalf(im.GetImageData(byLevel[baseLevel]), w, h, sums, hw, hh);

    int levels = 0, greenChanged = 0, counted = 0;
    double fitErr = 0.0;
    double lenSum[4] = { 0, 0, 0, 0 };
    int64 lenCount[4] = { 0, 0, 0, 0 };
    for (int lv = baseLevel + 1; lv < 16; lv++) {
        w = hw;
        h = hh;
        // the means (unnormalised) of this level
        for (int i = 0; i < w * h; i++) {
            float* s = sums + i * 4;
            const float inv = s[3] > 0.0f ? 1.0f / s[3] : 0.0f;
            s[0] *= inv;
            s[1] *= inv;
            s[2] *= inv;
            s[3] = 1.0f;
        }
        const int idx = byLevel[lv];
        if (idx >= 0) {
            const bimageImage_t& img = im.GetImageHeader(idx);
            byte* data = const_cast<byte*>(im.GetImageData(idx));
            const int bw = (w + 3) / 4, bh = (h + 3) / 4;
            if (img.width == w && img.height == h && img.dataSize >= bw * bh * 16) {
                for (int by = 0; by < bh; by++) {
                    for (int bx = 0; bx < bw; bx++) {
                        float target[16];
                        bool inside[16];
                        for (int t = 0; t < 16; t++) {
                            const int x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                            inside[t] = x < w && y < h;
                            target[t] = 0.0f;
                            if (inside[t]) {
                                const float* s = sums + (y * w + x) * 4;
                                const float len = idMath::Sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
                                target[t] = ToksvigBlue(len);
                                const int k = Min(lv - baseLevel - 1, 3);
                                lenSum[k] += len;
                                lenCount[k]++;
                            }
                        }
                        fitErr += FitBlue(data + (by * bw + bx) * 16, target, inside, &counted, &greenChanged);
                    }
                }
                levels++;
            }
        }
        if (w == 1 && h == 1) {
            break;
        }
        // the next level down: the means of the means (every child stands
        // for the same number of top-level normals)
        const int nw = Max(1, w >> 1), nh = Max(1, h >> 1);
        float* next = (float*)Mem_ClearedAlloc(nw * nh * 4 * sizeof(float), TAG_TEMP);
        if (next == NULL) {
            break;
        }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const float* s = sums + (y * w + x) * 4;
                float* d = next + (Min(y >> 1, nh - 1) * nw + Min(x >> 1, nw - 1)) * 4;
                d[0] += s[0];
                d[1] += s[1];
                d[2] += s[2];
                d[3] += 1.0f;
            }
        }
        Mem_Free(sums);
        sums = next;
        hw = nw;
        hh = nh;
    }
    Mem_Free(sums);

    r_toksvigMaps.Increment();
    r_toksvigLevels.Add(levels);
    if (greenChanged > 0) {
        r_toksvigGreenChanged.Add(greenChanged);
    }
    {
        idScopedCriticalSection lock(toksvigStatsLock);
        toksvigUs += Sys_Microseconds() - t0;
        toksvigFitErr += fitErr;
        toksvigFitTexels += counted;
        for (int k = 0; k < 4; k++) {
            toksvigLenSum[k] += lenSum[k];
            toksvigLenCount[k] += lenCount[k];
        }
    }
    return levels;
}

// the log line for what was filtered since the last call (then reset)
void R_ToksvigReport(const char* when)
{
    idScopedCriticalSection lock(toksvigStatsLock);
    const int maps = r_toksvigMaps.GetValue();
    if (maps == 0) {
        return;
    }
    common->Printf("toksvig (%s): %d normal maps, %d mip levels filtered in %.1f ms; blue fit off by %.2f/255 on average; mean |Na| one level down %.3f, two %.3f, three %.3f, four+ %.3f; green changed in %d blocks (must be 0)\n",
        when, maps, r_toksvigLevels.GetValue(), toksvigUs / 1000.0, toksvigFitTexels ? toksvigFitErr / toksvigFitTexels : 0.0,
        toksvigLenCount[0] ? toksvigLenSum[0] / toksvigLenCount[0] : 1.0, toksvigLenCount[1] ? toksvigLenSum[1] / toksvigLenCount[1] : 1.0,
        toksvigLenCount[2] ? toksvigLenSum[2] / toksvigLenCount[2] : 1.0, toksvigLenCount[3] ? toksvigLenSum[3] / toksvigLenCount[3] : 1.0,
        r_toksvigGreenChanged.GetValue());
    r_toksvigMaps.SetValue(0);
    r_toksvigLevels.SetValue(0);
    r_toksvigGreenChanged.SetValue(0);
    toksvigUs = 0;
    toksvigFitErr = 0.0;
    toksvigFitTexels = 0;
    for (int k = 0; k < 4; k++) {
        toksvigLenSum[k] = 0.0;
        toksvigLenCount[k] = 0;
    }
}
