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

Cost, sized for the Vita's Cortex-A9 at 500 MHz: the base level's decode is
most of it - one square root per texel (the length of the shader's vector is
exactly 0.5 for any normal map texel that is not off by quantisation, so no
division), whole-number palettes. The fit compares in whole numbers and reads
blue from a table on |Na|^2. A level of 64 texels high or more is split by
block rows over the job threads, which have nothing else to do while a map
loads. The log line after a level load gives the time (decode and total), the
slowest map, and any map filtered during play - its first frame waits for it.

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
static uint64 toksvigDecodeUs = 0;
static int64 toksvigTexels = 0;
static uint64 toksvigSlowestUs = 0;
static idStr toksvigSlowest;
static int toksvigSlowestW = 0, toksvigSlowestH = 0;
static uint64 toksvigPlayUs = 0; // filtering during play, taken by the bench every frame

static double toksvigLenSum[4];
static int64 toksvigLenCount[4];
static idSysMutex toksvigStatsLock;

uint64 R_ToksvigTakePlayUs()
{
    idScopedCriticalSection lock(toksvigStatsLock);
    const uint64 us = toksvigPlayUs;
    toksvigPlayUs = 0;
    return us;
}

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
DecodeBlockRowsToHalf

The fast decode, for levels whose sides are multiples of 4 (every base level
in the game): block rows [by0, by1) of the level, each block's 16 normals
averaged into the four texels of the next level down it covers, written
(not added) as means with a count of 1. The bands of rows touch disjoint
texels, so the job threads can split a level by rows.

The shader's decode with whole numbers: A = 2 a - 255 and G = 2 g - 255 (the
alpha and green palette values), so x = A / 510, y = G / 510, and with
Q = A^2 + G^2 the vector is (A, G, sqrt(|Q - 65025|)) / 510. Its length is
exactly 0.5 when Q <= 65025 - nearly every texel of a unit normal map -
so the unit normal is (A, G, sqrt(65025 - Q)) / 255: one square root and no
division. Beyond (quantisation), the length is sqrt(2 Q - 65025) / 510.
========================
*/
// a level's mean normals, three 16-bit components each in 1/32767ths (every
// component of a mean of unit vectors is within -1..1): 6 bytes a texel
// instead of 16 - a 512x512 map's next level fits a 480 KB work area kept in
// cached RAM (R_ToksvigInit)
static const float TOKSVIG_Q = 32767.0f;

static inline int16 ToksvigQ(float v)
{
    // rounded half away from zero by truncation: no floorf (a library call on the Vita: VFPv3 has no rounding instruction)
    const float f = v * TOKSVIG_Q;
    return (int16)idMath::ClampInt(-32767, 32767, (int)(f >= 0.0f ? f + 0.5f : f - 0.5f));
}

static void DecodeBlockRowsToHalf(const byte* data, int w, int by0, int by1, int16* means, int hw)
{
    const int bw = w / 4;
    for (int by = by0; by < by1; by++) {
        for (int bx = 0; bx < bw; bx++) {
            const byte* blk = data + (by * bw + bx) * 16;
            int a[8];
            AlphaPalette(blk, a);
            float ax[8];
            int aq[8];
            for (int i = 0; i < 8; i++) {
                const int A = 2 * a[i] - 255;
                ax[i] = (float)A;
                aq[i] = A * A;
            }
            const uint16 c0 = blk[8] | (blk[9] << 8);
            const uint16 c1 = blk[10] | (blk[11] << 8);
            int g[4];
            GreenPalette(c0, c1, g);
            float gy[4];
            int gq[4];
            for (int i = 0; i < 4; i++) {
                const int G = 2 * g[i] - 255;
                gy[i] = (float)G;
                gq[i] = G * G;
            }
            uint64 aBits = 0;
            for (int i = 0; i < 6; i++) {
                aBits |= (uint64)blk[2 + i] << (8 * i);
            }
            uint32 cBits = blk[12] | (blk[13] << 8) | (blk[14] << 16) | ((uint32)blk[15] << 24);
            float acc[4][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
            for (int t = 0; t < 16; t++) {
                const int ai = (int)(aBits & 7);
                const int gi = (int)(cBits & 3);
                aBits >>= 3;
                cBits >>= 2;
                const int Q = aq[ai] + gq[gi];
                float z, inv;
                if (Q <= 65025) {
                    z = idMath::Sqrt((float)(65025 - Q));
                    inv = 1.0f / 255.0f;
                } else {
                    const int d = Q - 65025;
                    z = idMath::Sqrt((float)d);
                    inv = idMath::InvSqrt((float)(Q + d));
                }
                float* c = acc[((t >> 3) & 1) * 2 + ((t >> 1) & 1)];
                c[0] += ax[ai] * inv;
                c[1] += gy[gi] * inv;
                c[2] += z * inv;
            }
            for (int q = 0; q < 4; q++) {
                int16* m = means + ((by * 2 + (q >> 1)) * hw + bx * 2 + (q & 1)) * 3;
                m[0] = ToksvigQ(acc[q][0] * 0.25f);
                m[1] = ToksvigQ(acc[q][1] * 0.25f);
                m[2] = ToksvigQ(acc[q][2] * 0.25f);
            }
        }
    }
}

/*
========================
ToksvigBlue

Blue for a mean normal |Na| long, 0..255: s' = 10 |Na| / (10 - 9 |Na|) for
the shaders' fixed exponent 10, blue = (10 - s') / 6 = 100 (1 - |Na|) /
(6 (10 - 9 |Na|)). The fit reads it from a table on |Na|^2 over 0.75..1
(below: 255, s' would be under 4), 1/8192 apart: off by 0.26/255 at most,
and no square root or division per texel.
========================
*/
static float ToksvigBlue(float len)
{
    len = idMath::ClampFloat(0.0f, 1.0f, len);
    return idMath::ClampFloat(0.0f, 1.0f, 100.0f * (1.0f - len) / (6.0f * (10.0f - 9.0f * len))) * 255.0f;
}

static const int TOKSVIG_LUT = 2048;
static byte toksvigBlueLut[TOKSVIG_LUT + 1];
static bool toksvigLutReady = false;

static void ToksvigInitLut()
{
    for (int i = 0; i <= TOKSVIG_LUT; i++) {
        const float len2 = 0.75f + i * (0.25f / TOKSVIG_LUT);
        toksvigBlueLut[i] = (byte)idMath::ClampInt(0, 255, (int)(ToksvigBlue(idMath::Sqrt(len2)) + 0.5f));
    }
    toksvigLutReady = true;
}

static inline int ToksvigBlueOfLen2(float len2)
{
    const int i = (int)((len2 - 0.75f) * (4.0f * TOKSVIG_LUT) + 0.5f);
    if (i <= 0) {
        return i < 0 ? 255 : toksvigBlueLut[0];
    }
    return i >= TOKSVIG_LUT ? toksvigBlueLut[TOKSVIG_LUT] : toksvigBlueLut[i];
}

/*
========================
FitBlue

Blue endpoints for one block, given its own colour indices: the least
squares fit of the targets (0..255), rounded, then the best of the
neighbouring 5-bit pairs by the real decode, keeping the endpoint order.
Returns the summed absolute error (8-bit units) and the texels counted.
Whole numbers wherever the decode is compared.
========================
*/
static int FitBlue(byte* blk, const int target[16], const bool inside[16], int* counted, int* greenChanged)
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
            return 0; // (never in the retail data) left as it is
        }
        int lo = 255, hi = 0;
        int n = 0;
        for (int t = 0; t < 16; t++) {
            if (inside[t]) {
                lo = Min(lo, target[t]);
                hi = Max(hi, target[t]);
                n++;
            }
        }
        if (n == 0) {
            return 0;
        }
        const int e0 = idMath::ClampInt(0, 31, (lo * 31 + 127) / 255);
        const int e1 = idMath::ClampInt(e0, 31, (hi * 31 + 127) / 255);
        int pal[3];
        pal[0] = Expand5(e0);
        pal[1] = Expand5(e1);
        pal[2] = (2 * pal[0] + pal[1]) / 3;
        uint32 bits = 0;
        int err = 0;
        for (int t = 0; t < 16; t++) {
            int best = 0;
            if (inside[t]) {
                int bestD = 1 << 30;
                for (int k = 0; k < 3; k++) {
                    const int d = abs(pal[k] - target[t]);
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
            return 0; // never written
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

    // every target 0 (flat areas: one normal under the whole block) and blue
    // already 0 in both endpoints (every retail block): nothing to write
    if ((c0 & 31) == 0 && (c1 & 31) == 0) {
        int any = 0, n = 0;
        for (int t = 0; t < 16; t++) {
            if (inside[t]) {
                any |= target[t];
                n++;
            }
        }
        if (any == 0) {
            *counted += n;
            return 0;
        }
    }

    // value = w * b0 + (1 - w) * b1 per index (four-colour mode)
    static const float weight[4] = { 1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f };
    float s00 = 0, s01 = 0, s11 = 0, t0 = 0, t1 = 0;
    int n = 0, sum = 0;
    int idx[16];
    for (int t = 0; t < 16; t++) {
        idx[t] = (cBits >> (2 * t)) & 3;
        if (!inside[t]) {
            continue;
        }
        const float wgt = weight[idx[t]];
        const float v = target[t] * (31.0f / 255.0f);
        s00 += wgt * wgt;
        s01 += wgt * (1.0f - wgt);
        s11 += (1.0f - wgt) * (1.0f - wgt);
        t0 += wgt * v;
        t1 += (1.0f - wgt) * v;
        sum += target[t];
        n++;
    }
    if (n == 0) {
        return 0;
    }
    float b0 = sum * (31.0f / 255.0f) / n, b1 = b0;
    const float det = s00 * s11 - s01 * s01;
    if (idMath::Fabs(det) > 1e-4f) { // (whole sums of ninths: 0 or at least 1/81)
        b0 = (t0 * s11 - t1 * s01) / det;
        b1 = (t1 * s00 - t0 * s01) / det;
    }

    int bestB0 = 0, bestB1 = 0;
    int bestErr = 1 << 30;
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
        int err = 0;
        for (int t = 0; t < 16; t++) {
            if (inside[t]) {
                err += abs(pal[idx[t]] - target[t]);
            }
        }
        if (err < bestErr) {
            bestErr = err;
            bestB0 = e0;
            bestB1 = e1;
            found = true;
        }
    };
    const int r0 = idMath::ClampInt(0, 31, (int)idMath::Rint(b0));
    const int r1 = idMath::ClampInt(0, 31, (int)idMath::Rint(b1));
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
        return 0; // nothing keeps the order: the block stays as it was
    }
    c0 = (uint16)((c0 & ~31) | bestB0);
    c1 = (uint16)((c1 & ~31) | bestB1);
    int gAfter[4];
    GreenPalette(c0, c1, gAfter);
    if (memcmp(gBefore, gAfter, sizeof(gBefore)) != 0) {
        (*greenChanged)++;
        return 0; // never written
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
The pass's work in bands of block rows, run on the job threads during a load
(the loading thread waits for them: they would idle otherwise)
========================
*/
idCVar r_toksvigJobs("r_toksvigJobs", "1", CVAR_RENDERER | CVAR_BOOL, "spread the Toksvig pass of each normal map over the job threads (the load waits for them)");

struct toksvigJob_t {
    int op; // 0 = decode the base level into means, 1 = fit a level's blue
    byte* data;
    int w, h;
    int16* means; // decode: the next level's (w / 2 wide); fit: this level's
    int by0, by1;
    // fit results
    int64 err;
    int counted, greenChanged;
    double len2Sum;
    int64 lenCount;
    uint64 us; // the job's own time (the log's speed-up on the job threads)
};

static void FitBlockRows(toksvigJob_t* j)
{
    const int w = j->w, h = j->h;
    const int bw = (w + 3) / 4;
    const float scale = 1.0f / (TOKSVIG_Q * TOKSVIG_Q);
    for (int by = j->by0; by < j->by1; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int target[16];
            bool inside[16];
            for (int t = 0; t < 16; t++) {
                const int x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                inside[t] = x < w && y < h;
                target[t] = 0;
                if (inside[t]) {
                    const int16* m = j->means + (y * w + x) * 3;
                    const uint32 q2 = (uint32)(m[0] * m[0]) + (uint32)(m[1] * m[1]) + (uint32)(m[2] * m[2]);
                    const float len2 = (float)q2 * scale;
                    target[t] = ToksvigBlueOfLen2(len2);
                    j->len2Sum += len2; // (the log's mean |Na|, from the mean |Na|^2)
                    j->lenCount++;
                }
            }
            j->err += FitBlue(j->data + (by * bw + bx) * 16, target, inside, &j->counted, &j->greenChanged);
        }
    }
}

static void ToksvigJob(toksvigJob_t* j)
{
    const uint64 t0 = Sys_Microseconds();
    if (j->op == 0) {
        DecodeBlockRowsToHalf(j->data, j->w, j->by0, j->by1, j->means, j->w / 2);
    } else {
        FitBlockRows(j);
    }
    j->us = Sys_Microseconds() - t0;
}
REGISTER_PARALLEL_JOB(ToksvigJob, "ToksvigJob");

static int toksvigBandedRuns = 0;    // for the log: how many runs were split
static uint64 toksvigBandedWallUs = 0; // their time, and the jobs' summed: the speed-up
static uint64 toksvigBandedJobUs = 0;

// runs proto's work over block rows 0..blockRows, split when it pays; the
// fit results are summed back into proto
static void ToksvigRun(toksvigJob_t& proto, int blockRows)
{
    static toksvigJob_t jobs[4];
    static idParallelJobList* jobList = NULL;
    int parts = 1;
    // (16 block rows = a 64-texel-high level: smaller ones are not worth a hand-off)
    if (r_toksvigJobs.GetBool() && blockRows >= 16 && R_IsGLThread()) {
        parts = idMath::ClampInt(1, 4, cvarSystem->GetCVarInteger("jobs_numThreads"));
    }
    for (int t = 0; t < parts; t++) {
        jobs[t] = proto;
        jobs[t].by0 = blockRows * t / parts;
        jobs[t].by1 = blockRows * (t + 1) / parts;
        jobs[t].err = 0;
        jobs[t].counted = jobs[t].greenChanged = 0;
        jobs[t].len2Sum = 0.0;
        jobs[t].lenCount = 0;
        jobs[t].us = 0;
    }
    if (parts == 1) {
        ToksvigJob(&jobs[0]);
    } else {
        if (jobList == NULL) {
            jobList = parallelJobManager->AllocJobList(JOBLIST_UTILITY, JOBLIST_PRIORITY_HIGH, 8, 0, NULL);
        }
        const uint64 t0 = Sys_Microseconds();
        for (int t = 0; t < parts; t++) {
            jobList->AddJob((jobRun_t)ToksvigJob, &jobs[t]);
        }
        jobList->Submit();
        jobList->Wait();
        toksvigBandedWallUs += Sys_Microseconds() - t0;
        for (int t = 0; t < parts; t++) {
            toksvigBandedJobUs += jobs[t].us;
        }
        toksvigBandedRuns++;
    }
    for (int t = 0; t < parts; t++) {
        proto.err += jobs[t].err;
        proto.counted += jobs[t].counted;
        proto.greenChanged += jobs[t].greenChanged;
        proto.len2Sum += jobs[t].len2Sum;
        proto.lenCount += jobs[t].lenCount;
    }
}

/*
========================
The work area

The Vita's first Toksvig run (r54) spent 2-3.4 s a level, ~300 ns a texel
even split four ways: its float work buffers (1 MB for a 512x512 map) were
allocated during the load, when the newlib heap had no room left, so they
came from vitaGL's UNCACHED pool - every read of a mean a trip to memory.
Now the means are 16-bit (6 bytes a texel) and live in a work area taken
from the newlib heap at start, while it is cached and has room: enough for
a 512x512 map's next level and the one after (ping-pong: each level's means
go where the level before last was). A bigger map allocates its own, and
the log counts maps whose memory was not cached RAM.
========================
*/
static const int TOKSVIG_SCRATCH = 256 * 256 * 3 + 128 * 128 * 3; // int16s: 480 KB
static int16* toksvigScratch = NULL;
static bool toksvigScratchBusy = false;
static int toksvigUncachedMaps = 0;  // work area or image data outside cached RAM (Vita)
static int toksvigOwnBufferMaps = 0; // maps too big for the work area

#if defined(__vita__)
bool Vita_IsCachedMemory(const void* p);
const char* Vita_MemKind(const void* p);
#endif

void R_ToksvigInit()
{
    if (toksvigScratch != NULL) {
        return;
    }
    toksvigScratch = (int16*)Mem_Alloc16(TOKSVIG_SCRATCH * sizeof(int16), TAG_RENDER);
#if defined(__vita__)
    common->Printf("toksvig: work area %d KB at %p (%s)\n", (int)(TOKSVIG_SCRATCH * sizeof(int16) / 1024), toksvigScratch,
        toksvigScratch != NULL ? Vita_MemKind(toksvigScratch) : "allocation failed");
#endif
}

/*
========================
R_ToksvigDXT5Normals

baseLevel: the first level of the file that is uploaded (the Vita may skip
the top ones). Returns the number of levels written.
========================
*/
int R_ToksvigDXT5Normals(idBinaryImage& im, int baseLevel, const char* name, bool duringPlay)
{
    if (!r_toksvig.GetBool()) {
        return 0;
    }
    const uint64 t0 = Sys_Microseconds();
    if (!toksvigLutReady) {
        ToksvigInitLut();
    }
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
    const bool fast = (w % 4) == 0 && (h % 4) == 0;

    // the means: the work area when they fit (this level's and the next's)
    const int need = hw * hh * 3;
    const int needNext = Max(1, hw >> 1) * Max(1, hh >> 1) * 3;
    int16* area = NULL;
    bool own = false;
    if (toksvigScratch != NULL && !toksvigScratchBusy && need + needNext <= TOKSVIG_SCRATCH) {
        area = toksvigScratch;
        toksvigScratchBusy = true;
    } else {
        area = (int16*)Mem_Alloc16((need + needNext) * sizeof(int16), TAG_TEMP);
        if (area == NULL) {
            return 0; // no room: the map loads unfiltered
        }
        own = true;
    }
    bool uncached = false;
#if defined(__vita__)
    uncached = !Vita_IsCachedMemory(area) || !Vita_IsCachedMemory(im.GetImageData(byLevel[baseLevel]));
#endif
    int16* cur = area;          // this level's means
    int16* other = area + need; // the next level's (then they swap)

    if (fast) {
        toksvigJob_t dec;
        memset(&dec, 0, sizeof(dec));
        dec.op = 0;
        dec.data = const_cast<byte*>(im.GetImageData(byLevel[baseLevel]));
        dec.w = w;
        dec.h = h;
        dec.means = cur;
        ToksvigRun(dec, h / 4);
    } else {
        // odd sizes (none in the game's data): the float decode, then 16-bit
        float* sums = (float*)Mem_ClearedAlloc(hw * hh * 4 * sizeof(float), TAG_TEMP);
        if (sums == NULL) {
            if (own) {
                Mem_Free(area);
            } else {
                toksvigScratchBusy = false;
            }
            return 0;
        }
        DecodeNormalsToHalf(im.GetImageData(byLevel[baseLevel]), w, h, sums, hw, hh);
        for (int i = 0; i < hw * hh; i++) {
            const float* sm = sums + i * 4;
            const float inv = sm[3] > 0.0f ? 1.0f / sm[3] : 0.0f;
            cur[i * 3 + 0] = ToksvigQ(sm[0] * inv);
            cur[i * 3 + 1] = ToksvigQ(sm[1] * inv);
            cur[i * 3 + 2] = ToksvigQ(sm[2] * inv);
        }
        Mem_Free(sums);
    }
    const uint64 tDecoded = Sys_Microseconds();

    int levels = 0, greenChanged = 0, counted = 0;
    int64 fitErr = 0;
    double len2Sum[4] = { 0, 0, 0, 0 };
    int64 lenCount[4] = { 0, 0, 0, 0 };
    for (int lv = baseLevel + 1; lv < 16; lv++) {
        w = hw;
        h = hh;
        const int idx = byLevel[lv];
        if (idx >= 0) {
            const bimageImage_t& img = im.GetImageHeader(idx);
            const int bw = (w + 3) / 4, bh = (h + 3) / 4;
            if (img.width == w && img.height == h && img.dataSize >= bw * bh * 16) {
                toksvigJob_t fit;
                memset(&fit, 0, sizeof(fit));
                fit.op = 1;
                fit.data = const_cast<byte*>(im.GetImageData(idx));
                fit.w = w;
                fit.h = h;
                fit.means = cur;
                ToksvigRun(fit, bh);
                fitErr += fit.err;
                counted += fit.counted;
                greenChanged += fit.greenChanged;
                const int k = Min(lv - baseLevel - 1, 3);
                len2Sum[k] += fit.len2Sum;
                lenCount[k] += fit.lenCount;
                levels++;
            }
        }
        if (w == 1 && h == 1) {
            break;
        }
        // the next level down: the means of the means (every child stands
        // for the same number of top-level normals), rounded
        const int nw = Max(1, w >> 1), nh = Max(1, h >> 1);
        for (int y = 0; y < nh; y++) {
            const int y0 = Min(2 * y, h - 1), y1 = Min(2 * y + 1, h - 1);
            for (int x = 0; x < nw; x++) {
                const int x0 = Min(2 * x, w - 1), x1 = Min(2 * x + 1, w - 1);
                const int16* a00 = cur + (y0 * w + x0) * 3;
                const int16* a01 = cur + (y0 * w + x1) * 3;
                const int16* a10 = cur + (y1 * w + x0) * 3;
                const int16* a11 = cur + (y1 * w + x1) * 3;
                int16* d = other + (y * nw + x) * 3;
                for (int c = 0; c < 3; c++) {
                    const int sum = a00[c] + a01[c] + a10[c] + a11[c];
                    d[c] = (int16)((sum >= 0 ? sum + 2 : sum - 2) / 4);
                }
            }
        }
        // the level after next goes where this one was (it is a quarter of the next)
        int16* old = cur;
        cur = other;
        other = old;
        hw = nw;
        hh = nh;
    }
    if (own) {
        Mem_Free(area);
    } else {
        toksvigScratchBusy = false;
    }

    const uint64 t1 = Sys_Microseconds();
    r_toksvigMaps.Increment();
    r_toksvigLevels.Add(levels);
    if (greenChanged > 0) {
        r_toksvigGreenChanged.Add(greenChanged);
    }
    {
        idScopedCriticalSection lock(toksvigStatsLock);
        toksvigUs += t1 - t0;
        if (duringPlay) {
            toksvigPlayUs += t1 - t0;
        }
        toksvigDecodeUs += tDecoded - t0;
        toksvigTexels += (int64)base.width * base.height;
        if (uncached) {
            toksvigUncachedMaps++;
        }
        if (own) {
            toksvigOwnBufferMaps++;
        }
        if (t1 - t0 > toksvigSlowestUs) {
            toksvigSlowestUs = t1 - t0;
            toksvigSlowest = name != NULL ? name : "?";
            toksvigSlowestW = base.width;
            toksvigSlowestH = base.height;
        }
        toksvigFitErr += (double)fitErr;
        toksvigFitTexels += counted;
        for (int k = 0; k < 4; k++) {
            toksvigLenSum[k] += len2Sum[k];
            toksvigLenCount[k] += lenCount[k];
        }
    }
    // during play the image's first use waits for this: a hitch, to be seen
    // in the log
    if (duringPlay) {
        common->Printf("toksvig: '%s' (%dx%d) filtered during play in %.1f ms%s\n", name != NULL ? name : "?", base.width, base.height, (t1 - t0) / 1000.0,
            uncached ? " (uncached memory)" : "");
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
    // (the mean |Na| from the mean |Na|^2: within 0.001 for lengths over 0.9)
    double meanLen[4];
    for (int k = 0; k < 4; k++) {
        meanLen[k] = toksvigLenCount[k] ? idMath::Sqrt((float)(toksvigLenSum[k] / toksvigLenCount[k])) : 1.0;
    }
    common->Printf("toksvig (%s): %d normal maps, %d mip levels filtered in %.1f ms (decode %.1f ms, %.2f Mtexels, %.0f ns a texel in all; %d runs split over the job threads, x%.2f faster for it; %d maps in uncached memory, %d too big for the work area; slowest '%s' %dx%d %.1f ms); blue fit off by %.2f/255 on average; mean |Na| one level down %.3f, two %.3f, three %.3f, four+ %.3f; green changed in %d blocks (must be 0)\n",
        when, maps, r_toksvigLevels.GetValue(), toksvigUs / 1000.0, toksvigDecodeUs / 1000.0, toksvigTexels / 1000000.0,
        toksvigTexels ? 1000.0 * toksvigUs / toksvigTexels : 0.0, toksvigBandedRuns,
        toksvigBandedWallUs ? (double)toksvigBandedJobUs / toksvigBandedWallUs : 1.0, toksvigUncachedMaps, toksvigOwnBufferMaps,
        toksvigSlowest.c_str(), toksvigSlowestW, toksvigSlowestH, toksvigSlowestUs / 1000.0,
        toksvigFitTexels ? toksvigFitErr / toksvigFitTexels : 0.0,
        meanLen[0], meanLen[1], meanLen[2], meanLen[3],
        r_toksvigGreenChanged.GetValue());
    r_toksvigMaps.SetValue(0);
    r_toksvigLevels.SetValue(0);
    r_toksvigGreenChanged.SetValue(0);
    toksvigUs = 0;
    toksvigDecodeUs = 0;
    toksvigTexels = 0;
    toksvigSlowestUs = 0;
    toksvigSlowest.Clear();
    toksvigBandedRuns = 0;
    toksvigBandedWallUs = 0;
    toksvigBandedJobUs = 0;
    toksvigUncachedMaps = 0;
    toksvigOwnBufferMaps = 0;
    toksvigFitErr = 0.0;
    toksvigFitTexels = 0;
    for (int k = 0; k < 4; k++) {
        toksvigLenSum[k] = 0.0;
        toksvigLenCount[k] = 0;
    }
}
