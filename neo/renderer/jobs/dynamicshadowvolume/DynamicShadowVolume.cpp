/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "DynamicShadowVolume_local.h"
#include "../../../sys/vita/vita_zones.h" // VITA_ZONE: timing zones for the perf log
#if defined(__vita__)
const char* Vita_MemKind(const void* p); // vita_main.cpp: "cached", "UNCACHED", ...
#endif

#include "../../../idlib/sys/sys_intrinsics.h"
#include "../../../idlib/geometry/DrawVert_intrinsics.h"

#if defined(USE_INTRINSICS)
static const __m128i vector_int_neg_one = _mm_set_epi32(-1, -1, -1, -1);
#endif
#if defined(__vita__) && defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

bool R_ForceZFailStencilShadows(); // tr_backend_draw.cpp: the verification's z-fail check (r_forceZFailStencilShadows)
#if defined(__vita__)
uint64 Sys_Microseconds();                  // sys_public.h (this job file keeps to the idlib headers)
void Vita_Checkpoint(const char* fmt, ...); // vita_main.cpp: timestamped line in the log
#endif

/*
=====================
TriangleFacing_SSE2
=====================
*/
#if defined(USE_INTRINSICS)
static ID_FORCE_INLINE __m128i TriangleFacing_SSE2(const __m128& vert0X, const __m128& vert0Y, const __m128& vert0Z,
    const __m128& vert1X, const __m128& vert1Y, const __m128& vert1Z,
    const __m128& vert2X, const __m128& vert2Y, const __m128& vert2Z,
    const __m128& lightOriginX, const __m128& lightOriginY, const __m128& lightOriginZ)
{
    const __m128 sX = _mm_sub_ps(vert1X, vert0X);
    const __m128 sY = _mm_sub_ps(vert1Y, vert0Y);
    const __m128 sZ = _mm_sub_ps(vert1Z, vert0Z);

    const __m128 tX = _mm_sub_ps(vert2X, vert0X);
    const __m128 tY = _mm_sub_ps(vert2Y, vert0Y);
    const __m128 tZ = _mm_sub_ps(vert2Z, vert0Z);

    const __m128 normalX = _mm_nmsub_ps(tZ, sY, _mm_mul_ps(tY, sZ));
    const __m128 normalY = _mm_nmsub_ps(tX, sZ, _mm_mul_ps(tZ, sX));
    const __m128 normalZ = _mm_nmsub_ps(tY, sX, _mm_mul_ps(tX, sY));
    const __m128 normalW = _mm_madd_ps(normalX, vert0X, _mm_madd_ps(normalY, vert0Y, _mm_mul_ps(normalZ, vert0Z)));

    const __m128 delta = _mm_nmsub_ps(lightOriginX, normalX, _mm_nmsub_ps(lightOriginY, normalY, _mm_nmsub_ps(lightOriginZ, normalZ, normalW)));
    return _mm_castps_si128(_mm_cmplt_ps(delta, _mm_setzero_ps()));
}
#endif

/*
=====================
TriangleCulled

The clip space of the 'lightProject' is assumed to be in the range [0, 1].
=====================
*/
#if defined(USE_INTRINSICS)
static ID_FORCE_INLINE __m128i TriangleCulled_SSE2(const __m128& vert0X, const __m128& vert0Y, const __m128& vert0Z,
    const __m128& vert1X, const __m128& vert1Y, const __m128& vert1Z,
    const __m128& vert2X, const __m128& vert2Y, const __m128& vert2Z,
    const __m128& lightProjectX, const __m128& lightProjectY, const __m128& lightProjectZ, const __m128& lightProjectW)
{

    const __m128 mvpX0 = _mm_splat_ps(lightProjectX, 0);
    const __m128 mvpX1 = _mm_splat_ps(lightProjectX, 1);
    const __m128 mvpX2 = _mm_splat_ps(lightProjectX, 2);
    const __m128 mvpX3 = _mm_splat_ps(lightProjectX, 3);

    const __m128 c0X = _mm_madd_ps(vert0X, mvpX0, _mm_madd_ps(vert0Y, mvpX1, _mm_madd_ps(vert0Z, mvpX2, mvpX3)));
    const __m128 c1X = _mm_madd_ps(vert1X, mvpX0, _mm_madd_ps(vert1Y, mvpX1, _mm_madd_ps(vert1Z, mvpX2, mvpX3)));
    const __m128 c2X = _mm_madd_ps(vert2X, mvpX0, _mm_madd_ps(vert2Y, mvpX1, _mm_madd_ps(vert2Z, mvpX2, mvpX3)));

    const __m128 mvpY0 = _mm_splat_ps(lightProjectY, 0);
    const __m128 mvpY1 = _mm_splat_ps(lightProjectY, 1);
    const __m128 mvpY2 = _mm_splat_ps(lightProjectY, 2);
    const __m128 mvpY3 = _mm_splat_ps(lightProjectY, 3);

    const __m128 c0Y = _mm_madd_ps(vert0X, mvpY0, _mm_madd_ps(vert0Y, mvpY1, _mm_madd_ps(vert0Z, mvpY2, mvpY3)));
    const __m128 c1Y = _mm_madd_ps(vert1X, mvpY0, _mm_madd_ps(vert1Y, mvpY1, _mm_madd_ps(vert1Z, mvpY2, mvpY3)));
    const __m128 c2Y = _mm_madd_ps(vert2X, mvpY0, _mm_madd_ps(vert2Y, mvpY1, _mm_madd_ps(vert2Z, mvpY2, mvpY3)));

    const __m128 mvpZ0 = _mm_splat_ps(lightProjectZ, 0);
    const __m128 mvpZ1 = _mm_splat_ps(lightProjectZ, 1);
    const __m128 mvpZ2 = _mm_splat_ps(lightProjectZ, 2);
    const __m128 mvpZ3 = _mm_splat_ps(lightProjectZ, 3);

    const __m128 c0Z = _mm_madd_ps(vert0X, mvpZ0, _mm_madd_ps(vert0Y, mvpZ1, _mm_madd_ps(vert0Z, mvpZ2, mvpZ3)));
    const __m128 c1Z = _mm_madd_ps(vert1X, mvpZ0, _mm_madd_ps(vert1Y, mvpZ1, _mm_madd_ps(vert1Z, mvpZ2, mvpZ3)));
    const __m128 c2Z = _mm_madd_ps(vert2X, mvpZ0, _mm_madd_ps(vert2Y, mvpZ1, _mm_madd_ps(vert2Z, mvpZ2, mvpZ3)));

    const __m128 mvpW0 = _mm_splat_ps(lightProjectW, 0);
    const __m128 mvpW1 = _mm_splat_ps(lightProjectW, 1);
    const __m128 mvpW2 = _mm_splat_ps(lightProjectW, 2);
    const __m128 mvpW3 = _mm_splat_ps(lightProjectW, 3);

    const __m128 c0W = _mm_madd_ps(vert0X, mvpW0, _mm_madd_ps(vert0Y, mvpW1, _mm_madd_ps(vert0Z, mvpW2, mvpW3)));
    const __m128 c1W = _mm_madd_ps(vert1X, mvpW0, _mm_madd_ps(vert1Y, mvpW1, _mm_madd_ps(vert1Z, mvpW2, mvpW3)));
    const __m128 c2W = _mm_madd_ps(vert2X, mvpW0, _mm_madd_ps(vert2Y, mvpW1, _mm_madd_ps(vert2Z, mvpW2, mvpW3)));

    const __m128 zero = _mm_setzero_ps();

    __m128 b0 = _mm_or_ps(_mm_or_ps(_mm_cmpgt_ps(c0X, zero), _mm_cmpgt_ps(c1X, zero)), _mm_cmpgt_ps(c2X, zero));
    __m128 b1 = _mm_or_ps(_mm_or_ps(_mm_cmpgt_ps(c0Y, zero), _mm_cmpgt_ps(c1Y, zero)), _mm_cmpgt_ps(c2Y, zero));
    __m128 b2 = _mm_or_ps(_mm_or_ps(_mm_cmpgt_ps(c0Z, zero), _mm_cmpgt_ps(c1Z, zero)), _mm_cmpgt_ps(c2Z, zero));
    __m128 b3 = _mm_or_ps(_mm_or_ps(_mm_cmpgt_ps(c0W, c0X), _mm_cmpgt_ps(c1W, c1X)), _mm_cmpgt_ps(c2W, c2X));
    __m128 b4 = _mm_or_ps(_mm_or_ps(_mm_cmpgt_ps(c0W, c0Y), _mm_cmpgt_ps(c1W, c1Y)), _mm_cmpgt_ps(c2W, c2Y));
    __m128 b5 = _mm_or_ps(_mm_or_ps(_mm_cmpgt_ps(c0W, c0Z), _mm_cmpgt_ps(c1W, c1Z)), _mm_cmpgt_ps(c2W, c2Z));

    b0 = _mm_and_ps(b0, b1);
    b2 = _mm_and_ps(b2, b3);
    b4 = _mm_and_ps(b4, b5);
    b0 = _mm_and_ps(b0, b2);
    b0 = _mm_and_ps(b0, b4);

    return _mm_castps_si128(_mm_cmpeq_ps(b0, zero));
}

#else

/*
=====================
TriangleFacing

Returns 255 if the triangle is facing the light origin, otherwise returns 0.
=====================
*/
static byte TriangleFacing_Generic(const idVec3& v1, const idVec3& v2, const idVec3& v3, const idVec3& lightOrigin)
{
    const float sx = v2.x - v1.x;
    const float sy = v2.y - v1.y;
    const float sz = v2.z - v1.z;

    const float tx = v3.x - v1.x;
    const float ty = v3.y - v1.y;
    const float tz = v3.z - v1.z;

    const float normalX = ty * sz - tz * sy;
    const float normalY = tz * sx - tx * sz;
    const float normalZ = tx * sy - ty * sx;
    const float normalW = normalX * v1.x + normalY * v1.y + normalZ * v1.z;

    const float d = lightOrigin.x * normalX + lightOrigin.y * normalY + lightOrigin.z * normalZ - normalW;
    return (d > 0.0f) ? 255 : 0;
}

/*
=====================
TriangleCulled

Returns 255 if the triangle is culled to the light projection matrix, otherwise returns 0.
The clip space of the 'lightProject' is assumed to be in the range [0, 1].
=====================
*/
static byte TriangleCulled_Generic(const idVec3& v1, const idVec3& v2, const idVec3& v3, const idRenderMatrix& lightProject)
{
    // transform the triangle
    idVec4 c[3];
    for (int i = 0; i < 4; i++) {
        c[0][i] = v1[0] * lightProject[i][0] + v1[1] * lightProject[i][1] + v1[2] * lightProject[i][2] + lightProject[i][3];
        c[1][i] = v2[0] * lightProject[i][0] + v2[1] * lightProject[i][1] + v2[2] * lightProject[i][2] + lightProject[i][3];
        c[2][i] = v3[0] * lightProject[i][0] + v3[1] * lightProject[i][1] + v3[2] * lightProject[i][2] + lightProject[i][3];
    }

    // calculate the culled bits
    int bits = 0;
    for (int i = 0; i < 3; i++) {
        const float minW = 0.0f;
        const float maxW = c[i][3];

        if (c[i][0] > minW) {
            bits |= (1 << 0);
        }
        if (c[i][0] < maxW) {
            bits |= (1 << 1);
        }
        if (c[i][1] > minW) {
            bits |= (1 << 2);
        }
        if (c[i][1] < maxW) {
            bits |= (1 << 3);
        }
        if (c[i][2] > minW) {
            bits |= (1 << 4);
        }
        if (c[i][2] < maxW) {
            bits |= (1 << 5);
        }
    }

    // if any bits weren't set, the triangle is completely off one side of the frustum
    return (bits != 63) ? 255 : 0;
}

#endif

#if defined(__vita__) && defined(__ARM_NEON__)
/*
=====================
Vita NEON shadow volume setup

What the SSE path does on a PC, for the Vita's Cortex-A9: each triangle's
facing (to the light origin) and its cull against the light frustum, four
triangles at a time, and the skinning of the vertices the skinned variant
needs first. The scalar code took 10-13 ms for one big model's shadow - a
single job everything else then waited for. Same arithmetic, grouped
differently (and NEON flushes denormals), so a triangle exactly on a plane can
come out the other way, as with the SSE path.

'positions' points at vertex 0's x, y, z and the next vertex is 'stride'
bytes on; the float after z is loaded with them and ignored.
=====================
*/
static ID_FORCE_INLINE float32x4_t Vita_LoadPos(const byte* positions, int stride, int index)
{
    return vld1q_f32((const float*)(positions + index * stride));
}

// four x,y,z,(w) vectors -> X, Y, Z lanes
static ID_FORCE_INLINE void Vita_Transpose3(float32x4_t a, float32x4_t b, float32x4_t c, float32x4_t d, float32x4_t& X, float32x4_t& Y, float32x4_t& Z)
{
    const float32x4x2_t ab = vtrnq_f32(a, b); // a0 b0 a2 b2 | a1 b1 a3 b3
    const float32x4x2_t cd = vtrnq_f32(c, d);
    X = vcombine_f32(vget_low_f32(ab.val[0]), vget_low_f32(cd.val[0]));
    Y = vcombine_f32(vget_low_f32(ab.val[1]), vget_low_f32(cd.val[1]));
    Z = vcombine_f32(vget_high_f32(ab.val[0]), vget_high_f32(cd.val[0]));
}

// row: one row of the light projection, loaded once as a vector; multiplied
// by lane (GCC re-materialized float scalars through the stack inside the
// loops, and the kernels spilled heavily). The same multiply-adds by scalar.
static ID_FORCE_INLINE float32x4_t Vita_Clip(float32x4_t X, float32x4_t Y, float32x4_t Z, const float32x4_t row)
{
    const float32x2_t r01 = vget_low_f32(row);
    const float32x2_t r23 = vget_high_f32(row);
    return vmlaq_lane_f32(vmlaq_lane_f32(vmlaq_lane_f32(vdupq_lane_f32(r23, 1), X, r01, 0), Y, r01, 1), Z, r23, 0);
}

// lightOrigin . n, n in lanes, the origin a vector (x, y, z, -): by lane, as above
static ID_FORCE_INLINE float32x4_t Vita_DotOrigin(float32x4_t nX, float32x4_t nY, float32x4_t nZ, const float32x4_t origin)
{
    const float32x2_t oxy = vget_low_f32(origin);
    const float32x2_t oz = vget_high_f32(origin);
    return vmlaq_lane_f32(vmlaq_lane_f32(vmulq_lane_f32(nX, oxy, 0), nY, oxy, 1), nZ, oz, 0);
}

static ID_FORCE_INLINE float32x4_t Vita_LoadOrigin(const idVec3& o)
{
    const float a[4] = { o.x, o.y, o.z, 0.0f };
    return vld1q_f32(a);
}

static int Vita_TriangleFacingCulled_NEON(byte* __restrict facing, byte* __restrict culled, const triIndex_t* __restrict indexes, int numIndexes,
    const byte* positions, int stride, const idVec3& lightOrigin, const idRenderMatrix& lightProject, bool cullShadowTrianglesToLight)
{
    const float32x4_t zero = vdupq_n_f32(0.0f);
    const uint32x4_t cullMask = vdupq_n_u32(cullShadowTrianglesToLight ? 0xFFFFFFFFu : 0u);
    const uint32x4_t one = vdupq_n_u32(1);
    uint32x4_t frontCount = vdupq_n_u32(0);
    const float32x4_t lp0 = vld1q_f32(lightProject[0]);
    const float32x4_t lp1 = vld1q_f32(lightProject[1]);
    const float32x4_t lp2 = vld1q_f32(lightProject[2]);
    const float32x4_t lp3 = vld1q_f32(lightProject[3]);
    const float32x4_t origin = Vita_LoadOrigin(lightOrigin);

    const int numTris = numIndexes / 3;
    int t = 0;
    for (; t + 4 <= numTris; t += 4) {
        const triIndex_t* ix = indexes + t * 3;
        float32x4_t v[3][3]; // [corner][x, y, z], four triangles per lane
        for (int k = 0; k < 3; k++) {
            Vita_Transpose3(Vita_LoadPos(positions, stride, ix[k]), Vita_LoadPos(positions, stride, ix[3 + k]),
                Vita_LoadPos(positions, stride, ix[6 + k]), Vita_LoadPos(positions, stride, ix[9 + k]),
                v[k][0], v[k][1], v[k][2]);
        }

        // facing: d = lightOrigin . n - n . v0 > 0, n = t x s
        const float32x4_t sX = vsubq_f32(v[1][0], v[0][0]);
        const float32x4_t sY = vsubq_f32(v[1][1], v[0][1]);
        const float32x4_t sZ = vsubq_f32(v[1][2], v[0][2]);
        const float32x4_t tX = vsubq_f32(v[2][0], v[0][0]);
        const float32x4_t tY = vsubq_f32(v[2][1], v[0][1]);
        const float32x4_t tZ = vsubq_f32(v[2][2], v[0][2]);
        const float32x4_t nX = vmlsq_f32(vmulq_f32(tY, sZ), tZ, sY);
        const float32x4_t nY = vmlsq_f32(vmulq_f32(tZ, sX), tX, sZ);
        const float32x4_t nZ = vmlsq_f32(vmulq_f32(tX, sY), tY, sX);
        const float32x4_t nW = vmlaq_f32(vmlaq_f32(vmulq_f32(nX, v[0][0]), nY, v[0][1]), nZ, v[0][2]);
        const float32x4_t d = vsubq_f32(Vita_DotOrigin(nX, nY, nZ, origin), nW);
        uint32x4_t front = vcgtq_f32(d, zero);

        // culled unless, for each of the six clip planes, some corner is inside
        uint32x4_t b0 = vdupq_n_u32(0), b1 = b0, b2 = b0, b3 = b0, b4 = b0, b5 = b0;
        for (int k = 0; k < 3; k++) {
            const float32x4_t cX = Vita_Clip(v[k][0], v[k][1], v[k][2], lp0);
            const float32x4_t cY = Vita_Clip(v[k][0], v[k][1], v[k][2], lp1);
            const float32x4_t cZ = Vita_Clip(v[k][0], v[k][1], v[k][2], lp2);
            const float32x4_t cW = Vita_Clip(v[k][0], v[k][1], v[k][2], lp3);
            b0 = vorrq_u32(b0, vcgtq_f32(cX, zero));
            b1 = vorrq_u32(b1, vcgtq_f32(cW, cX));
            b2 = vorrq_u32(b2, vcgtq_f32(cY, zero));
            b3 = vorrq_u32(b3, vcgtq_f32(cW, cY));
            b4 = vorrq_u32(b4, vcgtq_f32(cZ, zero));
            b5 = vorrq_u32(b5, vcgtq_f32(cW, cZ));
        }
        const uint32x4_t inside = vandq_u32(vandq_u32(vandq_u32(b0, b1), vandq_u32(b2, b3)), vandq_u32(b4, b5));
        const uint32x4_t cull = vmvnq_u32(inside);

        // triangles outside the light frustum count as facing (optional)
        front = vorrq_u32(front, vandq_u32(cull, cullMask));
        frontCount = vaddq_u32(frontCount, vandq_u32(front, one));

        // the 0 / ~0 lane masks as 0 / 255 bytes
        const uint16x4_t f16 = vmovn_u32(front);
        const uint16x4_t c16 = vmovn_u32(cull);
        const uint8x8_t fc8 = vmovn_u16(vcombine_u16(f16, c16));
        vst1_lane_u32((uint32_t*)(facing + t), vreinterpret_u32_u8(fc8), 0);
        vst1_lane_u32((uint32_t*)(culled + t), vreinterpret_u32_u8(fc8), 1);
    }
    const uint32x2_t s = vadd_u32(vget_low_u32(frontCount), vget_high_u32(frontCount));
    int numFrontFacing = (int)vget_lane_u32(vpadd_u32(s, s), 0);

    const byte cullShadowTrianglesToLightByte = cullShadowTrianglesToLight ? 255 : 0;
    for (; t < numTris; t++) {
        const idVec3& v1 = *(const idVec3*)(positions + indexes[t * 3 + 0] * stride);
        const idVec3& v2 = *(const idVec3*)(positions + indexes[t * 3 + 1] * stride);
        const idVec3& v3 = *(const idVec3*)(positions + indexes[t * 3 + 2] * stride);
        const byte triangleCulled = TriangleCulled_Generic(v1, v2, v3, lightProject);
        byte triangleFacing = TriangleFacing_Generic(v1, v2, v3, lightOrigin);
        triangleFacing |= (triangleCulled & cullShadowTrianglesToLightByte);
        culled[t] = triangleCulled;
        facing[t] = triangleFacing;
        numFrontFacing += (triangleFacing & 1);
    }
    return numFrontFacing;
}

/*
=====================
Vita_TriangleFacingCulled_PerVertex_NEON

The same results as Vita_TriangleFacingCulled_NEON, byte for byte, for a
third of the work. That one clips each triangle's three corners against the
light frustum - four plane values a corner - and a vertex is a corner of
about six triangles, so every vertex was clipped about six times, and its
position gathered from a 32-byte idDrawVert six times. Here each vertex is
clipped once, its six "inside this plane" bits kept in a byte and its
position packed at 16 bytes; a triangle then costs its facing (a cross
product) and three byte ORs (culled when some plane has no corner inside).
The clip values are the same NEON arithmetic on the same positions, lane for
lane, so the bits are exactly the ones the per-corner loop compared.
'packed' holds numVerts rounded up to 4 idVec4s, 'insideBits' as many bytes
(4-aligned). tools/vita/neontest checks the two against each other.
=====================
*/
static ID_FORCE_INLINE uint32x4_t Vita_InsideBits(float32x4_t X, float32x4_t Y, float32x4_t Z,
    const float32x4_t lp0, const float32x4_t lp1, const float32x4_t lp2, const float32x4_t lp3)
{
    const float32x4_t zero = vdupq_n_f32(0.0f);
    const float32x4_t cX = Vita_Clip(X, Y, Z, lp0);
    const float32x4_t cY = Vita_Clip(X, Y, Z, lp1);
    const float32x4_t cZ = Vita_Clip(X, Y, Z, lp2);
    const float32x4_t cW = Vita_Clip(X, Y, Z, lp3);
    uint32x4_t bits = vandq_u32(vcgtq_f32(cX, zero), vdupq_n_u32(1));
    bits = vorrq_u32(bits, vandq_u32(vcgtq_f32(cW, cX), vdupq_n_u32(2)));
    bits = vorrq_u32(bits, vandq_u32(vcgtq_f32(cY, zero), vdupq_n_u32(4)));
    bits = vorrq_u32(bits, vandq_u32(vcgtq_f32(cW, cY), vdupq_n_u32(8)));
    bits = vorrq_u32(bits, vandq_u32(vcgtq_f32(cZ, zero), vdupq_n_u32(16)));
    bits = vorrq_u32(bits, vandq_u32(vcgtq_f32(cW, cZ), vdupq_n_u32(32)));
    return bits;
}

static int Vita_TriangleFacingCulled_PerVertex_NEON(byte* __restrict facing, byte* __restrict culled, const triIndex_t* __restrict indexes, int numIndexes,
    const byte* positions, int stride, int numVerts, float* __restrict packed, byte* __restrict insideBits,
    const idVec3& lightOrigin, const idRenderMatrix& lightProject, bool cullShadowTrianglesToLight)
{
    const float32x4_t lp0 = vld1q_f32(lightProject[0]);
    const float32x4_t lp1 = vld1q_f32(lightProject[1]);
    const float32x4_t lp2 = vld1q_f32(lightProject[2]);
    const float32x4_t lp3 = vld1q_f32(lightProject[3]);
    const float32x4_t origin = Vita_LoadOrigin(lightOrigin);

    // each vertex once: packed position and inside bits (a short last group
    // repeats the last vertex in its spare lanes; they land in the padding)
    for (int v = 0; v < numVerts; v += 4) {
        float32x4_t p[4];
        for (int k = 0; k < 4; k++) {
            const int vi = (v + k < numVerts) ? v + k : numVerts - 1;
            p[k] = Vita_LoadPos(positions, stride, vi);
            vst1q_f32(packed + (v + k) * 4, p[k]);
        }
        float32x4_t X, Y, Z;
        Vita_Transpose3(p[0], p[1], p[2], p[3], X, Y, Z);
        const uint32x4_t bits = Vita_InsideBits(X, Y, Z, lp0, lp1, lp2, lp3);
        const uint16x4_t b16 = vmovn_u32(bits);
        const uint8x8_t b8 = vmovn_u16(vcombine_u16(b16, b16));
        vst1_lane_u32((uint32_t*)(insideBits + v), vreinterpret_u32_u8(b8), 0);
    }

    const float32x4_t zero = vdupq_n_f32(0.0f);
    const uint32x4_t cullMask = vdupq_n_u32(cullShadowTrianglesToLight ? 0xFFFFFFFFu : 0u);
    const uint32x4_t one = vdupq_n_u32(1);
    uint32x4_t frontCount = vdupq_n_u32(0);
    const byte* packedBytes = (const byte*)packed;

    const int numTris = numIndexes / 3;
    int t = 0;
    for (; t + 4 <= numTris; t += 4) {
        const triIndex_t* ix = indexes + t * 3;
        float32x4_t v[3][3]; // [corner][x, y, z], four triangles per lane
        for (int k = 0; k < 3; k++) {
            Vita_Transpose3(Vita_LoadPos(packedBytes, 16, ix[k]), Vita_LoadPos(packedBytes, 16, ix[3 + k]),
                Vita_LoadPos(packedBytes, 16, ix[6 + k]), Vita_LoadPos(packedBytes, 16, ix[9 + k]),
                v[k][0], v[k][1], v[k][2]);
        }

        // facing: d = lightOrigin . n - n . v0 > 0, n = t x s
        const float32x4_t sX = vsubq_f32(v[1][0], v[0][0]);
        const float32x4_t sY = vsubq_f32(v[1][1], v[0][1]);
        const float32x4_t sZ = vsubq_f32(v[1][2], v[0][2]);
        const float32x4_t tX = vsubq_f32(v[2][0], v[0][0]);
        const float32x4_t tY = vsubq_f32(v[2][1], v[0][1]);
        const float32x4_t tZ = vsubq_f32(v[2][2], v[0][2]);
        const float32x4_t nX = vmlsq_f32(vmulq_f32(tY, sZ), tZ, sY);
        const float32x4_t nY = vmlsq_f32(vmulq_f32(tZ, sX), tX, sZ);
        const float32x4_t nZ = vmlsq_f32(vmulq_f32(tX, sY), tY, sX);
        const float32x4_t nW = vmlaq_f32(vmlaq_f32(vmulq_f32(nX, v[0][0]), nY, v[0][1]), nZ, v[0][2]);
        const float32x4_t d = vsubq_f32(Vita_DotOrigin(nX, nY, nZ, origin), nW);
        uint32x4_t front = vcgtq_f32(d, zero);

        // culled when some plane has none of the corners inside
        uint32_t c4[4];
        for (int j = 0; j < 4; j++) {
            const int o = insideBits[ix[j * 3 + 0]] | insideBits[ix[j * 3 + 1]] | insideBits[ix[j * 3 + 2]];
            c4[j] = (o == 63) ? 0u : 0xFFFFFFFFu;
        }
        const uint32x4_t cull = vld1q_u32(c4);

        // triangles outside the light frustum count as facing (optional)
        front = vorrq_u32(front, vandq_u32(cull, cullMask));
        frontCount = vaddq_u32(frontCount, vandq_u32(front, one));

        // the 0 / ~0 lane masks as 0 / 255 bytes
        const uint16x4_t f16 = vmovn_u32(front);
        const uint16x4_t c16 = vmovn_u32(cull);
        const uint8x8_t fc8 = vmovn_u16(vcombine_u16(f16, c16));
        vst1_lane_u32((uint32_t*)(facing + t), vreinterpret_u32_u8(fc8), 0);
        vst1_lane_u32((uint32_t*)(culled + t), vreinterpret_u32_u8(fc8), 1);
    }
    const uint32x2_t s = vadd_u32(vget_low_u32(frontCount), vget_high_u32(frontCount));
    int numFrontFacing = (int)vget_lane_u32(vpadd_u32(s, s), 0);

    // the last 1-3 triangles exactly as the per-corner version does them
    const byte cullShadowTrianglesToLightByte = cullShadowTrianglesToLight ? 255 : 0;
    for (; t < numTris; t++) {
        const idVec3& v1 = *(const idVec3*)(positions + indexes[t * 3 + 0] * stride);
        const idVec3& v2 = *(const idVec3*)(positions + indexes[t * 3 + 1] * stride);
        const idVec3& v3 = *(const idVec3*)(positions + indexes[t * 3 + 2] * stride);
        const byte triangleCulled = TriangleCulled_Generic(v1, v2, v3, lightProject);
        byte triangleFacing = TriangleFacing_Generic(v1, v2, v3, lightOrigin);
        triangleFacing |= (triangleCulled & cullShadowTrianglesToLightByte);
        culled[t] = triangleCulled;
        facing[t] = triangleFacing;
        numFrontFacing += (triangleFacing & 1);
    }
    return numFrontFacing;
}

// the per-vertex version's buffers live on the job's stack (256 KB, the
// SPU's local store size); a model bigger than this uses the per-corner one
#define VITA_PERVERTEX_MAX_VERTS 6144

// the precise "is the view inside the shadow volume" test, on the triangles
// facing away from the light, until one says yes.
//
// It tested every one of them (0.77 ms a call in a fight); now a triangle is
// first checked against four planes around the view-to-light segment - two
// through it, one at each end facing out - pushed out by the sphere radius
// plus half a unit for rounding. A triangle whose three vertices all lie past
// the same one is farther than the radius from the segment (the segment is
// on the other side of that plane), so the full test cannot say yes for it
// and is skipped; the answer is the same. (The static and pre-light shadow
// jobs' R_ViewInsideShadowVolume culls the same way.) One pass over the
// vertices sets the bits, four at a time.
static bool Vita_InsideShadowVolume(const byte* facing, const triIndex_t* indexes, int numIndexes, const byte* positions, int stride, int numVerts,
    const idVec3& lineStart, const idVec3& lineEnd, const idVec3& lineDir, const float lineLength, const float radius)
{
    idVec3 n0, n1;
    lineDir.NormalVectors(n0, n1);
    const float planes[4][4] = {
        { n0.x, n0.y, n0.z, -(n0.x * lineStart.x + n0.y * lineStart.y + n0.z * lineStart.z) },
        { n1.x, n1.y, n1.z, -(n1.x * lineStart.x + n1.y * lineStart.y + n1.z * lineStart.z) },
        { lineDir.x, lineDir.y, lineDir.z, -(lineDir.x * lineStart.x + lineDir.y * lineStart.y + lineDir.z * lineStart.z) },
        { -lineDir.x, -lineDir.y, -lineDir.z, lineDir.x * lineEnd.x + lineDir.y * lineEnd.y + lineDir.z * lineEnd.z },
    };
    const float r = radius + 0.5f;
    byte* bits = (byte*)_alloca16((numVerts + 4) & ~3);
    int i = 0;
    {
        const float32x4_t pr = vdupq_n_f32(r);
        const float32x4_t nr = vdupq_n_f32(-r);
        for (; i + 4 <= numVerts; i += 4) {
            // four vertices' x, y, z side by side (each load also takes the float after z)
            const float32x4_t v0 = vld1q_f32((const float*)(positions + (i + 0) * stride));
            const float32x4_t v1 = vld1q_f32((const float*)(positions + (i + 1) * stride));
            const float32x4_t v2 = vld1q_f32((const float*)(positions + (i + 2) * stride));
            const float32x4_t v3 = vld1q_f32((const float*)(positions + (i + 3) * stride));
            const float32x4x2_t t01 = vtrnq_f32(v0, v1);
            const float32x4x2_t t23 = vtrnq_f32(v2, v3);
            const float32x4_t x = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
            const float32x4_t y = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
            const float32x4_t z = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
            float32x4_t d[4];
            for (int k = 0; k < 4; k++) {
                d[k] = vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(vdupq_n_f32(planes[k][3]), x, planes[k][0]), y, planes[k][1]), z, planes[k][2]);
            }
            uint32x4_t b = vandq_u32(vcgtq_f32(d[0], pr), vdupq_n_u32(1));
            b = vorrq_u32(b, vandq_u32(vcltq_f32(d[0], nr), vdupq_n_u32(2)));
            b = vorrq_u32(b, vandq_u32(vcgtq_f32(d[1], pr), vdupq_n_u32(4)));
            b = vorrq_u32(b, vandq_u32(vcltq_f32(d[1], nr), vdupq_n_u32(8)));
            b = vorrq_u32(b, vandq_u32(vcltq_f32(d[2], nr), vdupq_n_u32(16)));
            b = vorrq_u32(b, vandq_u32(vcltq_f32(d[3], nr), vdupq_n_u32(32)));
            const uint16x4_t b16 = vmovn_u32(b);
            const uint8x8_t b8 = vmovn_u16(vcombine_u16(b16, b16));
            vst1_lane_u32((uint32_t*)(bits + i), vreinterpret_u32_u8(b8), 0);
        }
    }
    for (; i < numVerts; i++) {
        const float* p = (const float*)(positions + i * stride);
        float d[4];
        for (int k = 0; k < 4; k++) {
            d[k] = planes[k][3] + p[0] * planes[k][0] + p[1] * planes[k][1] + p[2] * planes[k][2];
        }
        bits[i] = (byte)((d[0] > r ? 1 : 0) | (d[0] < -r ? 2 : 0) | (d[1] > r ? 4 : 0) | (d[1] < -r ? 8 : 0) | (d[2] < -r ? 16 : 0) | (d[3] < -r ? 32 : 0));
    }

    for (int k = 0, n = 0; k + 3 <= numIndexes; k += 3, n++) {
        if (!facing[n]) {
            if (bits[indexes[k + 0]] & bits[indexes[k + 1]] & bits[indexes[k + 2]]) {
                continue; // all three past one plane: out of the sphere's reach
            }
            const idVec3& v0 = *(const idVec3*)(positions + indexes[k + 0] * stride);
            const idVec3& v1 = *(const idVec3*)(positions + indexes[k + 1] * stride);
            const idVec3& v2 = *(const idVec3*)(positions + indexes[k + 2] * stride);
            if (R_LineIntersectsTriangleExpandedWithSphere(lineStart, lineEnd, lineDir, lineLength, radius, v2, v1, v0)) {
                return true;
            }
        }
    }
    return false;
}

// the skinned positions (w = 1): four weighted joint matrices blended and
// applied, as Scalar_LoadSkinnedDrawVertPosition does, in NEON
static void Vita_SkinPositions_NEON(idVec4* __restrict tempVerts, const idDrawVert* __restrict verts, const int numVerts, const idJointMat* __restrict joints)
{
    const float32x2_t wOne = vdup_n_f32(1.0f);
    for (int i = 0; i < numVerts; i++) {
        const idDrawVert& dv = verts[i];
        const float* j0 = joints[dv.color[0]].ToFloatPtr();
        const float* j1 = joints[dv.color[1]].ToFloatPtr();
        const float* j2 = joints[dv.color[2]].ToFloatPtr();
        const float* j3 = joints[dv.color[3]].ToFloatPtr();
        const float w0 = dv.color2[0] * (1.0f / 255.0f);
        const float w1 = dv.color2[1] * (1.0f / 255.0f);
        const float w2 = dv.color2[2] * (1.0f / 255.0f);
        const float w3 = dv.color2[3] * (1.0f / 255.0f);
        float32x4_t r[3];
        for (int row = 0; row < 3; row++) {
            float32x4_t m = vmulq_n_f32(vld1q_f32(j0 + row * 4), w0);
            m = vmlaq_n_f32(m, vld1q_f32(j1 + row * 4), w1);
            m = vmlaq_n_f32(m, vld1q_f32(j2 + row * 4), w2);
            r[row] = vmlaq_n_f32(m, vld1q_f32(j3 + row * 4), w3);
        }
        const float32x4_t p = vsetq_lane_f32(1.0f, vld1q_f32(dv.xyz.ToFloatPtr()), 3); // x y z 1
        const float32x4_t p0 = vmulq_f32(r[0], p);
        const float32x4_t p1 = vmulq_f32(r[1], p);
        const float32x4_t p2 = vmulq_f32(r[2], p);
        const float32x2_t xy = vpadd_f32(vadd_f32(vget_low_f32(p0), vget_high_f32(p0)), vadd_f32(vget_low_f32(p1), vget_high_f32(p1)));
        const float32x2_t zz = vadd_f32(vget_low_f32(p2), vget_high_f32(p2));
        const float32x2_t z1 = vext_f32(vpadd_f32(zz, zz), wOne, 1); // z, 1
        vst1q_f32(tempVerts[i].ToFloatPtr(), vcombine_f32(xy, z1));
    }
}
#endif

/*
=====================
CalculateTriangleFacingCulledStatic
=====================
*/
static int CalculateTriangleFacingCulledStatic(byte* __restrict facing, byte* __restrict culled, const triIndex_t* __restrict indexes, int numIndexes,
    const idDrawVert* __restrict verts, const int numVerts,
    const idVec3& lightOrigin, const idVec3& viewOrigin,
    bool cullShadowTrianglesToLight, const idRenderMatrix& lightProject,
    bool* insideShadowVolume, const float radius)
{

    assert_spu_local_store(facing);
    assert_not_spu_local_store(indexes);
    assert_not_spu_local_store(verts);

    if (insideShadowVolume != NULL) {
        *insideShadowVolume = false;
    }

    // calculate the start, end, dir and length of the line from the view origin to the light origin
    const idVec3 lineStart = viewOrigin;
    const idVec3 lineEnd = lightOrigin;
    const idVec3 lineDelta = lineEnd - lineStart;
    const float lineLengthSqr = lineDelta.LengthSqr();
    const float lineLengthRcp = idMath::InvSqrt(lineLengthSqr);
    const idVec3 lineDir = lineDelta * lineLengthRcp;
    const float lineLength = lineLengthSqr * lineLengthRcp;

#if defined(USE_INTRINSICS)

    idODSStreamedIndexedArray<idDrawVert, triIndex_t, 32, SBT_QUAD, 4 * 3> indexedVertsODS(verts, numVerts, indexes, numIndexes);

    const __m128 lightOriginX = _mm_splat_ps(_mm_load_ss(&lightOrigin.x), 0);
    const __m128 lightOriginY = _mm_splat_ps(_mm_load_ss(&lightOrigin.y), 0);
    const __m128 lightOriginZ = _mm_splat_ps(_mm_load_ss(&lightOrigin.z), 0);

    const __m128 lightProjectX = _mm_loadu_ps(lightProject[0]);
    const __m128 lightProjectY = _mm_loadu_ps(lightProject[1]);
    const __m128 lightProjectZ = _mm_loadu_ps(lightProject[2]);
    const __m128 lightProjectW = _mm_loadu_ps(lightProject[3]);

    const __m128i cullShadowTrianglesToLightMask = cullShadowTrianglesToLight ? vector_int_neg_one : vector_int_zero;

    __m128i numFrontFacing = _mm_setzero_si128();

    for (int i = 0, j = 0; i < numIndexes;) {

        const int batchStart = i;
        const int batchEnd = indexedVertsODS.FetchNextBatch();
        const int batchEnd4x = batchEnd - 4 * 3;
        const int indexStart = j;

        for (; i <= batchEnd4x; i += 4 * 3, j += 4) {
            const __m128 vertA0 = _mm_load_ps(indexedVertsODS[i + 0 * 3 + 0].xyz.ToFloatPtr());
            const __m128 vertA1 = _mm_load_ps(indexedVertsODS[i + 0 * 3 + 1].xyz.ToFloatPtr());
            const __m128 vertA2 = _mm_load_ps(indexedVertsODS[i + 0 * 3 + 2].xyz.ToFloatPtr());

            const __m128 vertB0 = _mm_load_ps(indexedVertsODS[i + 1 * 3 + 0].xyz.ToFloatPtr());
            const __m128 vertB1 = _mm_load_ps(indexedVertsODS[i + 1 * 3 + 1].xyz.ToFloatPtr());
            const __m128 vertB2 = _mm_load_ps(indexedVertsODS[i + 1 * 3 + 2].xyz.ToFloatPtr());

            const __m128 vertC0 = _mm_load_ps(indexedVertsODS[i + 2 * 3 + 0].xyz.ToFloatPtr());
            const __m128 vertC1 = _mm_load_ps(indexedVertsODS[i + 2 * 3 + 1].xyz.ToFloatPtr());
            const __m128 vertC2 = _mm_load_ps(indexedVertsODS[i + 2 * 3 + 2].xyz.ToFloatPtr());

            const __m128 vertD0 = _mm_load_ps(indexedVertsODS[i + 3 * 3 + 0].xyz.ToFloatPtr());
            const __m128 vertD1 = _mm_load_ps(indexedVertsODS[i + 3 * 3 + 1].xyz.ToFloatPtr());
            const __m128 vertD2 = _mm_load_ps(indexedVertsODS[i + 3 * 3 + 2].xyz.ToFloatPtr());

            const __m128 r0X = _mm_unpacklo_ps(vertA0, vertC0); // vertA0.x, vertC0.x, vertA0.z, vertC0.z
            const __m128 r0Y = _mm_unpackhi_ps(vertA0, vertC0); // vertA0.y, vertC0.y, vertA0.w, vertC0.w
            const __m128 r0Z = _mm_unpacklo_ps(vertB0, vertD0); // vertB0.x, vertD0.x, vertB0.z, vertD0.z
            const __m128 r0W = _mm_unpackhi_ps(vertB0, vertD0); // vertB0.y, vertD0.y, vertB0.w, vertD0.w

            const __m128 vert0X = _mm_unpacklo_ps(r0X, r0Z); // vertA0.x, vertB0.x, vertC0.x, vertD0.x
            const __m128 vert0Y = _mm_unpackhi_ps(r0X, r0Z); // vertA0.y, vertB0.y, vertC0.y, vertD0.y
            const __m128 vert0Z = _mm_unpacklo_ps(r0Y, r0W); // vertA0.z, vertB0.z, vertC0.z, vertD0.z

            const __m128 r1X = _mm_unpacklo_ps(vertA1, vertC1); // vertA1.x, vertC1.x, vertA1.z, vertC1.z
            const __m128 r1Y = _mm_unpackhi_ps(vertA1, vertC1); // vertA1.y, vertC1.y, vertA1.w, vertC1.w
            const __m128 r1Z = _mm_unpacklo_ps(vertB1, vertD1); // vertB1.x, vertD1.x, vertB1.z, vertD1.z
            const __m128 r1W = _mm_unpackhi_ps(vertB1, vertD1); // vertB1.y, vertD1.y, vertB1.w, vertD1.w

            const __m128 vert1X = _mm_unpacklo_ps(r1X, r1Z); // vertA1.x, vertB1.x, vertC1.x, vertD1.x
            const __m128 vert1Y = _mm_unpackhi_ps(r1X, r1Z); // vertA1.y, vertB1.y, vertC1.y, vertD1.y
            const __m128 vert1Z = _mm_unpacklo_ps(r1Y, r1W); // vertA1.z, vertB1.z, vertC1.z, vertD1.z

            const __m128 r2X = _mm_unpacklo_ps(vertA2, vertC2); // vertA2.x, vertC2.x, vertA2.z, vertC2.z
            const __m128 r2Y = _mm_unpackhi_ps(vertA2, vertC2); // vertA2.y, vertC2.y, vertA2.w, vertC2.w
            const __m128 r2Z = _mm_unpacklo_ps(vertB2, vertD2); // vertB2.x, vertD2.x, vertB2.z, vertD2.z
            const __m128 r2W = _mm_unpackhi_ps(vertB2, vertD2); // vertB2.y, vertD2.y, vertB2.w, vertD2.w

            const __m128 vert2X = _mm_unpacklo_ps(r2X, r2Z); // vertA2.x, vertB2.x, vertC2.x, vertD2.x
            const __m128 vert2Y = _mm_unpackhi_ps(r2X, r2Z); // vertA2.y, vertB2.y, vertC2.y, vertD2.y
            const __m128 vert2Z = _mm_unpacklo_ps(r2Y, r2W); // vertA2.z, vertB2.z, vertC2.z, vertD2.z

            const __m128i triangleCulled = TriangleCulled_SSE2(vert0X, vert0Y, vert0Z, vert1X, vert1Y, vert1Z, vert2X, vert2Y, vert2Z, lightProjectX, lightProjectY, lightProjectZ, lightProjectW);

            __m128i triangleFacing = TriangleFacing_SSE2(vert0X, vert0Y, vert0Z, vert1X, vert1Y, vert1Z, vert2X, vert2Y, vert2Z, lightOriginX, lightOriginY, lightOriginZ);

            // optionally make triangles that are outside the light frustum facing so they do not contribute to the shadow volume
            triangleFacing = _mm_or_si128(triangleFacing, _mm_and_si128(triangleCulled, cullShadowTrianglesToLightMask));

            // store culled
            const __m128i culled_s = _mm_packs_epi32(triangleCulled, triangleCulled);
            const __m128i culled_b = _mm_packs_epi16(culled_s, culled_s);
            *(int*)&culled[j] = _mm_cvtsi128_si32(culled_b);

            // store facing
            const __m128i facing_s = _mm_packs_epi32(triangleFacing, triangleFacing);
            const __m128i facing_b = _mm_packs_epi16(facing_s, facing_s);
            *(int*)&facing[j] = _mm_cvtsi128_si32(facing_b);

            // count the number of facing triangles
            numFrontFacing = _mm_add_epi32(numFrontFacing, _mm_and_si128(triangleFacing, vector_int_one));
        }

        if (insideShadowVolume != NULL) {
            for (int k = batchStart, n = indexStart; k <= batchEnd - 3; k += 3, n++) {
                if (!facing[n]) {
                    if (R_LineIntersectsTriangleExpandedWithSphere(lineStart, lineEnd, lineDir, lineLength, radius, indexedVertsODS[k + 2].xyz, indexedVertsODS[k + 1].xyz, indexedVertsODS[k + 0].xyz)) {
                        *insideShadowVolume = true;
                        insideShadowVolume = NULL;
                        break;
                    }
                }
            }
        }
    }

    numFrontFacing = _mm_add_epi32(numFrontFacing, _mm_shuffle_epi32(numFrontFacing, _MM_SHUFFLE(1, 0, 3, 2)));
    numFrontFacing = _mm_add_epi32(numFrontFacing, _mm_shuffle_epi32(numFrontFacing, _MM_SHUFFLE(2, 3, 0, 1)));

    return _mm_cvtsi128_si32(numFrontFacing);

#elif defined(__vita__) && defined(__ARM_NEON__)

    int numFrontFacing;
    if (numVerts <= VITA_PERVERTEX_MAX_VERTS) {
        VITA_ZONE("shadow job: facing + light cull (NEON, per vertex)");
        const int numVerts4 = (numVerts + 3) & ~3;
        float* packed = (float*)_alloca16(numVerts4 * 4 * sizeof(float));
        byte* insideBits = (byte*)_alloca16(numVerts4);
        numFrontFacing = Vita_TriangleFacingCulled_PerVertex_NEON(facing, culled, indexes, numIndexes, (const byte*)verts, sizeof(idDrawVert),
            numVerts, packed, insideBits, lightOrigin, lightProject, cullShadowTrianglesToLight);
    } else {
        VITA_ZONE("shadow job: facing + light cull (NEON)");
        numFrontFacing = Vita_TriangleFacingCulled_NEON(facing, culled, indexes, numIndexes, (const byte*)verts, sizeof(idDrawVert),
            lightOrigin, lightProject, cullShadowTrianglesToLight);
    }
    if (insideShadowVolume != NULL) {
        VITA_ZONE("shadow job: precise view-inside test");
        *insideShadowVolume = Vita_InsideShadowVolume(facing, indexes, numIndexes, (const byte*)verts, sizeof(idDrawVert), numVerts,
            lineStart, lineEnd, lineDir, lineLength, radius);
    }
    return numFrontFacing;

#else

    idODSStreamedIndexedArray<idDrawVert, triIndex_t, 32, SBT_QUAD, 1> indexedVertsODS(verts, numVerts, indexes, numIndexes);

    const byte cullShadowTrianglesToLightMask = cullShadowTrianglesToLight ? 255 : 0;

    int numFrontFacing = 0;

    for (int i = 0, j = 0; i < numIndexes;) {

        const int batchStart = i;
        const int batchEnd = indexedVertsODS.FetchNextBatch();
        const int indexStart = j;

        for (; i <= batchEnd - 3; i += 3, j++) {
            const idVec3& v1 = indexedVertsODS[i + 0].xyz;
            const idVec3& v2 = indexedVertsODS[i + 1].xyz;
            const idVec3& v3 = indexedVertsODS[i + 2].xyz;

            const byte triangleCulled = TriangleCulled_Generic(v1, v2, v3, lightProject);

            byte triangleFacing = TriangleFacing_Generic(v1, v2, v3, lightOrigin);

            // optionally make triangles that are outside the light frustum facing so they do not contribute to the shadow volume
            triangleFacing |= (triangleCulled & cullShadowTrianglesToLightMask);

            culled[j] = triangleCulled;
            facing[j] = triangleFacing;

            // count the number of facing triangles
            numFrontFacing += (triangleFacing & 1);
        }

        if (insideShadowVolume != NULL) {
            for (int k = batchStart, n = indexStart; k <= batchEnd - 3; k += 3, n++) {
                if (!facing[n]) {
                    if (R_LineIntersectsTriangleExpandedWithSphere(lineStart, lineEnd, lineDir, lineLength, radius, indexedVertsODS[k + 2].xyz, indexedVertsODS[k + 1].xyz, indexedVertsODS[k + 0].xyz)) {
                        *insideShadowVolume = true;
                        insideShadowVolume = NULL;
                        break;
                    }
                }
            }
        }
    }

    return numFrontFacing;

#endif
}

/*
=====================
CalculateTriangleFacingCulledSkinned
=====================
*/
static int CalculateTriangleFacingCulledSkinned(byte* __restrict facing, byte* __restrict culled, idVec4* __restrict tempVerts, const triIndex_t* __restrict indexes, int numIndexes,
    const idDrawVert* __restrict verts, const int numVerts, const idJointMat* __restrict joints,
    const idVec3& lightOrigin, const idVec3& viewOrigin,
    bool cullShadowTrianglesToLight, const idRenderMatrix& lightProject,
    bool* insideShadowVolume, const float radius)
{
    assert_spu_local_store(facing);
    assert_spu_local_store(joints);
    assert_not_spu_local_store(indexes);
    assert_not_spu_local_store(verts);

    if (insideShadowVolume != NULL) {
        *insideShadowVolume = false;
    }

    // calculate the start, end, dir and length of the line from the view origin to the light origin
    const idVec3 lineStart = viewOrigin;
    const idVec3 lineEnd = lightOrigin;
    const idVec3 lineDelta = lineEnd - lineStart;
    const float lineLengthSqr = lineDelta.LengthSqr();
    const float lineLengthRcp = idMath::InvSqrt(lineLengthSqr);
    const idVec3 lineDir = lineDelta * lineLengthRcp;
    const float lineLength = lineLengthSqr * lineLengthRcp;

#if defined(USE_INTRINSICS)

    idODSStreamedArray<idDrawVert, 32, SBT_DOUBLE, 1> vertsODS(verts, numVerts);

    for (int i = 0; i < numVerts;) {

        const int nextNumVerts = vertsODS.FetchNextBatch() - 1;

        for (; i <= nextNumVerts; i++) {
            __m128 v = LoadSkinnedDrawVertPosition(vertsODS[i], joints);
            _mm_store_ps(tempVerts[i].ToFloatPtr(), v);
        }
    }

    idODSStreamedArray<triIndex_t, 256, SBT_QUAD, 4 * 3> indexesODS(indexes, numIndexes);

    const __m128 lightOriginX = _mm_splat_ps(_mm_load_ss(&lightOrigin.x), 0);
    const __m128 lightOriginY = _mm_splat_ps(_mm_load_ss(&lightOrigin.y), 0);
    const __m128 lightOriginZ = _mm_splat_ps(_mm_load_ss(&lightOrigin.z), 0);

    const __m128 lightProjectX = _mm_loadu_ps(lightProject[0]);
    const __m128 lightProjectY = _mm_loadu_ps(lightProject[1]);
    const __m128 lightProjectZ = _mm_loadu_ps(lightProject[2]);
    const __m128 lightProjectW = _mm_loadu_ps(lightProject[3]);

    const __m128i cullShadowTrianglesToLightMask = cullShadowTrianglesToLight ? vector_int_neg_one : vector_int_zero;

    __m128i numFrontFacing = _mm_setzero_si128();

    for (int i = 0, j = 0; i < numIndexes;) {

        const int batchStart = i;
        const int batchEnd = indexesODS.FetchNextBatch();
        const int batchEnd4x = batchEnd - 4 * 3;
        const int indexStart = j;

        for (; i <= batchEnd4x; i += 4 * 3, j += 4) {
            const int indexA0 = indexesODS[(i + 0 * 3 + 0)];
            const int indexA1 = indexesODS[(i + 0 * 3 + 1)];
            const int indexA2 = indexesODS[(i + 0 * 3 + 2)];

            const int indexB0 = indexesODS[(i + 1 * 3 + 0)];
            const int indexB1 = indexesODS[(i + 1 * 3 + 1)];
            const int indexB2 = indexesODS[(i + 1 * 3 + 2)];

            const int indexC0 = indexesODS[(i + 2 * 3 + 0)];
            const int indexC1 = indexesODS[(i + 2 * 3 + 1)];
            const int indexC2 = indexesODS[(i + 2 * 3 + 2)];

            const int indexD0 = indexesODS[(i + 3 * 3 + 0)];
            const int indexD1 = indexesODS[(i + 3 * 3 + 1)];
            const int indexD2 = indexesODS[(i + 3 * 3 + 2)];

            const __m128 vertA0 = _mm_load_ps(tempVerts[indexA0].ToFloatPtr());
            const __m128 vertA1 = _mm_load_ps(tempVerts[indexA1].ToFloatPtr());
            const __m128 vertA2 = _mm_load_ps(tempVerts[indexA2].ToFloatPtr());

            const __m128 vertB0 = _mm_load_ps(tempVerts[indexB0].ToFloatPtr());
            const __m128 vertB1 = _mm_load_ps(tempVerts[indexB1].ToFloatPtr());
            const __m128 vertB2 = _mm_load_ps(tempVerts[indexB2].ToFloatPtr());

            const __m128 vertC0 = _mm_load_ps(tempVerts[indexC0].ToFloatPtr());
            const __m128 vertC1 = _mm_load_ps(tempVerts[indexC1].ToFloatPtr());
            const __m128 vertC2 = _mm_load_ps(tempVerts[indexC2].ToFloatPtr());

            const __m128 vertD0 = _mm_load_ps(tempVerts[indexD0].ToFloatPtr());
            const __m128 vertD1 = _mm_load_ps(tempVerts[indexD1].ToFloatPtr());
            const __m128 vertD2 = _mm_load_ps(tempVerts[indexD2].ToFloatPtr());

            const __m128 r0X = _mm_unpacklo_ps(vertA0, vertC0); // vertA0.x, vertC0.x, vertA0.z, vertC0.z
            const __m128 r0Y = _mm_unpackhi_ps(vertA0, vertC0); // vertA0.y, vertC0.y, vertA0.w, vertC0.w
            const __m128 r0Z = _mm_unpacklo_ps(vertB0, vertD0); // vertB0.x, vertD0.x, vertB0.z, vertD0.z
            const __m128 r0W = _mm_unpackhi_ps(vertB0, vertD0); // vertB0.y, vertD0.y, vertB0.w, vertD0.w

            const __m128 vert0X = _mm_unpacklo_ps(r0X, r0Z); // vertA0.x, vertB0.x, vertC0.x, vertD0.x
            const __m128 vert0Y = _mm_unpackhi_ps(r0X, r0Z); // vertA0.y, vertB0.y, vertC0.y, vertD0.y
            const __m128 vert0Z = _mm_unpacklo_ps(r0Y, r0W); // vertA0.z, vertB0.z, vertC0.z, vertD0.z

            const __m128 r1X = _mm_unpacklo_ps(vertA1, vertC1); // vertA1.x, vertC1.x, vertA1.z, vertC1.z
            const __m128 r1Y = _mm_unpackhi_ps(vertA1, vertC1); // vertA1.y, vertC1.y, vertA1.w, vertC1.w
            const __m128 r1Z = _mm_unpacklo_ps(vertB1, vertD1); // vertB1.x, vertD1.x, vertB1.z, vertD1.z
            const __m128 r1W = _mm_unpackhi_ps(vertB1, vertD1); // vertB1.y, vertD1.y, vertB1.w, vertD1.w

            const __m128 vert1X = _mm_unpacklo_ps(r1X, r1Z); // vertA1.x, vertB1.x, vertC1.x, vertD1.x
            const __m128 vert1Y = _mm_unpackhi_ps(r1X, r1Z); // vertA1.y, vertB1.y, vertC1.y, vertD1.y
            const __m128 vert1Z = _mm_unpacklo_ps(r1Y, r1W); // vertA1.z, vertB1.z, vertC1.z, vertD1.z

            const __m128 r2X = _mm_unpacklo_ps(vertA2, vertC2); // vertA2.x, vertC2.x, vertA2.z, vertC2.z
            const __m128 r2Y = _mm_unpackhi_ps(vertA2, vertC2); // vertA2.y, vertC2.y, vertA2.w, vertC2.w
            const __m128 r2Z = _mm_unpacklo_ps(vertB2, vertD2); // vertB2.x, vertD2.x, vertB2.z, vertD2.z
            const __m128 r2W = _mm_unpackhi_ps(vertB2, vertD2); // vertB2.y, vertD2.y, vertB2.w, vertD2.w

            const __m128 vert2X = _mm_unpacklo_ps(r2X, r2Z); // vertA2.x, vertB2.x, vertC2.x, vertD2.x
            const __m128 vert2Y = _mm_unpackhi_ps(r2X, r2Z); // vertA2.y, vertB2.y, vertC2.y, vertD2.y
            const __m128 vert2Z = _mm_unpacklo_ps(r2Y, r2W); // vertA2.z, vertB2.z, vertC2.z, vertD2.z

            const __m128i triangleCulled = TriangleCulled_SSE2(vert0X, vert0Y, vert0Z, vert1X, vert1Y, vert1Z, vert2X, vert2Y, vert2Z, lightProjectX, lightProjectY, lightProjectZ, lightProjectW);

            __m128i triangleFacing = TriangleFacing_SSE2(vert0X, vert0Y, vert0Z, vert1X, vert1Y, vert1Z, vert2X, vert2Y, vert2Z, lightOriginX, lightOriginY, lightOriginZ);

            // optionally make triangles that are outside the light frustum facing so they do not contribute to the shadow volume
            triangleFacing = _mm_or_si128(triangleFacing, _mm_and_si128(triangleCulled, cullShadowTrianglesToLightMask));

            // store culled
            const __m128i culled_s = _mm_packs_epi32(triangleCulled, triangleCulled);
            const __m128i culled_b = _mm_packs_epi16(culled_s, culled_s);
            *(int*)&culled[j] = _mm_cvtsi128_si32(culled_b);

            // store facing
            const __m128i facing_s = _mm_packs_epi32(triangleFacing, triangleFacing);
            const __m128i facing_b = _mm_packs_epi16(facing_s, facing_s);
            *(int*)&facing[j] = _mm_cvtsi128_si32(facing_b);

            // count the number of facing triangles
            numFrontFacing = _mm_add_epi32(numFrontFacing, _mm_and_si128(triangleFacing, vector_int_one));
        }

        if (insideShadowVolume != NULL) {
            for (int k = batchStart, n = indexStart; k <= batchEnd - 3; k += 3, n++) {
                if (!facing[n]) {
                    const int i0 = indexesODS[k + 0];
                    const int i1 = indexesODS[k + 1];
                    const int i2 = indexesODS[k + 2];
                    if (R_LineIntersectsTriangleExpandedWithSphere(lineStart, lineEnd, lineDir, lineLength, radius, tempVerts[i2].ToVec3(), tempVerts[i1].ToVec3(), tempVerts[i0].ToVec3())) {
                        *insideShadowVolume = true;
                        insideShadowVolume = NULL;
                        break;
                    }
                }
            }
        }
    }

    numFrontFacing = _mm_add_epi32(numFrontFacing, _mm_shuffle_epi32(numFrontFacing, _MM_SHUFFLE(1, 0, 3, 2)));
    numFrontFacing = _mm_add_epi32(numFrontFacing, _mm_shuffle_epi32(numFrontFacing, _MM_SHUFFLE(2, 3, 0, 1)));

    return _mm_cvtsi128_si32(numFrontFacing);

#elif defined(__vita__) && defined(__ARM_NEON__)

    Vita_SkinPositions_NEON(tempVerts, verts, numVerts, joints);
    const int numFrontFacing = Vita_TriangleFacingCulled_NEON(facing, culled, indexes, numIndexes, (const byte*)tempVerts, sizeof(idVec4),
        lightOrigin, lightProject, cullShadowTrianglesToLight);
    if (insideShadowVolume != NULL) {
        VITA_ZONE("shadow job: precise view-inside test");
        *insideShadowVolume = Vita_InsideShadowVolume(facing, indexes, numIndexes, (const byte*)tempVerts, sizeof(idVec4), numVerts,
            lineStart, lineEnd, lineDir, lineLength, radius);
    }
    return numFrontFacing;

#else

    idODSStreamedArray<idDrawVert, 32, SBT_DOUBLE, 1> vertsODS(verts, numVerts);

    for (int i = 0; i < numVerts;) {

        const int nextNumVerts = vertsODS.FetchNextBatch() - 1;

        for (; i <= nextNumVerts; i++) {
            tempVerts[i].ToVec3() = Scalar_LoadSkinnedDrawVertPosition(vertsODS[i], joints);
            tempVerts[i].w = 1.0f;
        }
    }

    idODSStreamedArray<triIndex_t, 256, SBT_QUAD, 1> indexesODS(indexes, numIndexes);

    const byte cullShadowTrianglesToLightMask = cullShadowTrianglesToLight ? 255 : 0;

    int numFrontFacing = 0;

    for (int i = 0, j = 0; i < numIndexes;) {

        const int batchStart = i;
        const int batchEnd = indexesODS.FetchNextBatch();
        const int indexStart = j;

        for (; i <= batchEnd - 3; i += 3, j++) {
            const int i0 = indexesODS[i + 0];
            const int i1 = indexesODS[i + 1];
            const int i2 = indexesODS[i + 2];

            const idVec3& v1 = tempVerts[i0].ToVec3();
            const idVec3& v2 = tempVerts[i1].ToVec3();
            const idVec3& v3 = tempVerts[i2].ToVec3();

            const byte triangleCulled = TriangleCulled_Generic(v1, v2, v3, lightProject);

            byte triangleFacing = TriangleFacing_Generic(v1, v2, v3, lightOrigin);

            // optionally make triangles that are outside the light frustum facing so they do not contribute to the shadow volume
            triangleFacing |= (triangleCulled & cullShadowTrianglesToLightMask);

            culled[j] = triangleCulled;
            facing[j] = triangleFacing;

            // count the number of facing triangles
            numFrontFacing += (triangleFacing & 1);
        }

        if (insideShadowVolume != NULL) {
            for (int k = batchStart, n = indexStart; k <= batchEnd - 3; k += 3, n++) {
                if (!facing[n]) {
                    const int i0 = indexesODS[k + 0];
                    const int i1 = indexesODS[k + 1];
                    const int i2 = indexesODS[k + 2];
                    if (R_LineIntersectsTriangleExpandedWithSphere(lineStart, lineEnd, lineDir, lineLength, radius, tempVerts[i2].ToVec3(), tempVerts[i1].ToVec3(), tempVerts[i0].ToVec3())) {
                        *insideShadowVolume = true;
                        insideShadowVolume = NULL;
                        break;
                    }
                }
            }
        }
    }

    return numFrontFacing;

#endif
}

/*
============
StreamOut
============
*/
static void StreamOut(void* dst, const void* src, int numBytes)
{
    numBytes = (numBytes + 15) & ~15;
    assert_16_byte_aligned(dst);
    assert_16_byte_aligned(src);

#if defined(USE_INTRINSICS)
    int i = 0;
    for (; i + 128 <= numBytes; i += 128) {
        __m128i d0 = _mm_load_si128((const __m128i*)((byte*)src + i + 0 * 16));
        __m128i d1 = _mm_load_si128((const __m128i*)((byte*)src + i + 1 * 16));
        __m128i d2 = _mm_load_si128((const __m128i*)((byte*)src + i + 2 * 16));
        __m128i d3 = _mm_load_si128((const __m128i*)((byte*)src + i + 3 * 16));
        __m128i d4 = _mm_load_si128((const __m128i*)((byte*)src + i + 4 * 16));
        __m128i d5 = _mm_load_si128((const __m128i*)((byte*)src + i + 5 * 16));
        __m128i d6 = _mm_load_si128((const __m128i*)((byte*)src + i + 6 * 16));
        __m128i d7 = _mm_load_si128((const __m128i*)((byte*)src + i + 7 * 16));
        _mm_stream_si128((__m128i*)((byte*)dst + i + 0 * 16), d0);
        _mm_stream_si128((__m128i*)((byte*)dst + i + 1 * 16), d1);
        _mm_stream_si128((__m128i*)((byte*)dst + i + 2 * 16), d2);
        _mm_stream_si128((__m128i*)((byte*)dst + i + 3 * 16), d3);
        _mm_stream_si128((__m128i*)((byte*)dst + i + 4 * 16), d4);
        _mm_stream_si128((__m128i*)((byte*)dst + i + 5 * 16), d5);
        _mm_stream_si128((__m128i*)((byte*)dst + i + 6 * 16), d6);
        _mm_stream_si128((__m128i*)((byte*)dst + i + 7 * 16), d7);
    }
    for (; i + 16 <= numBytes; i += 16) {
        __m128i d = _mm_load_si128((__m128i*)((byte*)src + i));
        _mm_stream_si128((__m128i*)((byte*)dst + i), d);
    }
#else
    memcpy(dst, src, numBytes);
#endif
}

/*
============
R_CreateShadowVolumeTriangles
============
*/
static void R_CreateShadowVolumeTriangles(triIndex_t* __restrict shadowIndices, triIndex_t* __restrict indexBuffer, int& numShadowIndexesTotal,
    const byte* __restrict facing, const silEdge_t* __restrict silEdges, const int numSilEdges,
    const triIndex_t* __restrict indexes, const int numIndexes, const bool includeCaps)
{
    assert_spu_local_store(facing);
    assert_not_spu_local_store(shadowIndices);
    assert_not_spu_local_store(silEdges);
    assert_not_spu_local_store(indexes);

#if 1

    const int IN_BUFFER_SIZE = 64;
    const int OUT_BUFFER_SIZE = IN_BUFFER_SIZE * 8; // each silhouette edge or cap triangle may create 6 indices (8 > 6)
    const int OUT_BUFFER_DEPTH = 4;                 // quad buffer to allow overlapped output streaming
    const int OUT_BUFFER_MASK = (OUT_BUFFER_SIZE * OUT_BUFFER_DEPTH - 1);

    compile_time_assert(OUT_BUFFER_SIZE * OUT_BUFFER_DEPTH * sizeof(triIndex_t) == OUTPUT_INDEX_BUFFER_SIZE);
    assert_16_byte_aligned(indexBuffer);

    int numShadowIndices = 0;
    int numStreamedIndices = 0;

    {
        idODSStreamedArray<silEdge_t, IN_BUFFER_SIZE, SBT_DOUBLE, 1> silEdgesODS(silEdges, numSilEdges);

        for (int i = 0; i < numSilEdges;) {

            const int nextNumSilEdges = silEdgesODS.FetchNextBatch();

            // NOTE: we rely on FetchNextBatch() to wait for all previous DMAs to complete
            while (numShadowIndices - numStreamedIndices >= OUT_BUFFER_SIZE) {
                StreamOut(shadowIndices + numStreamedIndices, &indexBuffer[numStreamedIndices & OUT_BUFFER_MASK], OUT_BUFFER_SIZE * sizeof(triIndex_t));
                numStreamedIndices += OUT_BUFFER_SIZE;
            }

            for (; i + 4 <= nextNumSilEdges; i += 4) {
                const silEdge_t& sil0 = silEdgesODS[i + 0];
                const silEdge_t& sil1 = silEdgesODS[i + 1];
                const silEdge_t& sil2 = silEdgesODS[i + 2];
                const silEdge_t& sil3 = silEdgesODS[i + 3];

                {
                    const byte f1a = facing[sil0.p1];
                    const byte f2a = facing[sil0.p2];
                    const byte ta = (f1a ^ f2a) & 6;
                    const triIndex_t v1a = sil0.v1 << 1;
                    const triIndex_t v2a = sil0.v2 << 1;

                    WriteIndexPair(&indexBuffer[(numShadowIndices + 0) & OUT_BUFFER_MASK], v1a ^ (0 & 1), v2a ^ (f1a & 1));
                    WriteIndexPair(&indexBuffer[(numShadowIndices + 2) & OUT_BUFFER_MASK], v2a ^ (f2a & 1), v1a ^ (f2a & 1));
                    WriteIndexPair(&indexBuffer[(numShadowIndices + 4) & OUT_BUFFER_MASK], v1a ^ (f1a & 1), v2a ^ (1 & 1));

                    numShadowIndices += ta;
                }

                {
                    const byte f1b = facing[sil1.p1];
                    const byte f2b = facing[sil1.p2];
                    const byte tb = (f1b ^ f2b) & 6;
                    const triIndex_t v1b = sil1.v1 << 1;
                    const triIndex_t v2b = sil1.v2 << 1;

                    WriteIndexPair(&indexBuffer[(numShadowIndices + 0) & OUT_BUFFER_MASK], v1b ^ (0 & 1), v2b ^ (f1b & 1));
                    WriteIndexPair(&indexBuffer[(numShadowIndices + 2) & OUT_BUFFER_MASK], v2b ^ (f2b & 1), v1b ^ (f2b & 1));
                    WriteIndexPair(&indexBuffer[(numShadowIndices + 4) & OUT_BUFFER_MASK], v1b ^ (f1b & 1), v2b ^ (1 & 1));

                    numShadowIndices += tb;
                }

                {
                    const byte f1c = facing[sil2.p1];
                    const byte f2c = facing[sil2.p2];
                    const byte tc = (f1c ^ f2c) & 6;
                    const triIndex_t v1c = sil2.v1 << 1;
                    const triIndex_t v2c = sil2.v2 << 1;

                    WriteIndexPair(&indexBuffer[(numShadowIndices + 0) & OUT_BUFFER_MASK], v1c ^ (0 & 1), v2c ^ (f1c & 1));
                    WriteIndexPair(&indexBuffer[(numShadowIndices + 2) & OUT_BUFFER_MASK], v2c ^ (f2c & 1), v1c ^ (f2c & 1));
                    WriteIndexPair(&indexBuffer[(numShadowIndices + 4) & OUT_BUFFER_MASK], v1c ^ (f1c & 1), v2c ^ (1 & 1));

                    numShadowIndices += tc;
                }

                {
                    const byte f1d = facing[sil3.p1];
                    const byte f2d = facing[sil3.p2];
                    const byte td = (f1d ^ f2d) & 6;
                    const triIndex_t v1d = sil3.v1 << 1;
                    const triIndex_t v2d = sil3.v2 << 1;

                    WriteIndexPair(&indexBuffer[(numShadowIndices + 0) & OUT_BUFFER_MASK], v1d ^ (0 & 1), v2d ^ (f1d & 1));
                    WriteIndexPair(&indexBuffer[(numShadowIndices + 2) & OUT_BUFFER_MASK], v2d ^ (f2d & 1), v1d ^ (f2d & 1));
                    WriteIndexPair(&indexBuffer[(numShadowIndices + 4) & OUT_BUFFER_MASK], v1d ^ (f1d & 1), v2d ^ (1 & 1));

                    numShadowIndices += td;
                }
            }
            for (; i + 1 <= nextNumSilEdges; i++) {
                const silEdge_t& sil = silEdgesODS[i];

                const byte f1 = facing[sil.p1];
                const byte f2 = facing[sil.p2];
                const byte t = (f1 ^ f2) & 6;
                const triIndex_t v1 = sil.v1 << 1;
                const triIndex_t v2 = sil.v2 << 1;

                WriteIndexPair(&indexBuffer[(numShadowIndices + 0) & OUT_BUFFER_MASK], v1 ^ (0 & 1), v2 ^ (f1 & 1));
                WriteIndexPair(&indexBuffer[(numShadowIndices + 2) & OUT_BUFFER_MASK], v2 ^ (f2 & 1), v1 ^ (f2 & 1));
                WriteIndexPair(&indexBuffer[(numShadowIndices + 4) & OUT_BUFFER_MASK], v1 ^ (f1 & 1), v2 ^ (1 & 1));

                numShadowIndices += t;
            }
        }
    }

    if (includeCaps) {
        idODSStreamedArray<triIndex_t, IN_BUFFER_SIZE, SBT_QUAD, 1> indexesODS(indexes, numIndexes);

        for (int i = 0, j = 0; i < numIndexes;) {

            const int nextNumIndexes = indexesODS.FetchNextBatch();

            // NOTE: we rely on FetchNextBatch() to wait for all previous DMAs to complete
            while (numShadowIndices - numStreamedIndices >= OUT_BUFFER_SIZE) {
                StreamOut(shadowIndices + numStreamedIndices, &indexBuffer[numStreamedIndices & OUT_BUFFER_MASK], OUT_BUFFER_SIZE * sizeof(triIndex_t));
                numStreamedIndices += OUT_BUFFER_SIZE;
            }

            for (; i + 4 * 3 <= nextNumIndexes; i += 4 * 3, j += 4) {
                const byte ta = ~facing[j + 0] & 6;
                const byte tb = ~facing[j + 1] & 6;
                const byte tc = ~facing[j + 2] & 6;
                const byte td = ~facing[j + 3] & 6;

                const triIndex_t i0a = indexesODS[i + 0 * 3 + 0] << 1;
                const triIndex_t i1a = indexesODS[i + 0 * 3 + 1] << 1;
                const triIndex_t i2a = indexesODS[i + 0 * 3 + 2] << 1;

                WriteIndexPair(&indexBuffer[(numShadowIndices + 0) & OUT_BUFFER_MASK], i2a + 0, i1a + 0);
                WriteIndexPair(&indexBuffer[(numShadowIndices + 2) & OUT_BUFFER_MASK], i0a + 0, i0a + 1);
                WriteIndexPair(&indexBuffer[(numShadowIndices + 4) & OUT_BUFFER_MASK], i1a + 1, i2a + 1);

                numShadowIndices += ta;

                const triIndex_t i0b = indexesODS[i + 1 * 3 + 0] << 1;
                const triIndex_t i1b = indexesODS[i + 1 * 3 + 1] << 1;
                const triIndex_t i2b = indexesODS[i + 1 * 3 + 2] << 1;

                WriteIndexPair(&indexBuffer[(numShadowIndices + 0) & OUT_BUFFER_MASK], i2b + 0, i1b + 0);
                WriteIndexPair(&indexBuffer[(numShadowIndices + 2) & OUT_BUFFER_MASK], i0b + 0, i0b + 1);
                WriteIndexPair(&indexBuffer[(numShadowIndices + 4) & OUT_BUFFER_MASK], i1b + 1, i2b + 1);

                numShadowIndices += tb;

                const triIndex_t i0c = indexesODS[i + 2 * 3 + 0] << 1;
                const triIndex_t i1c = indexesODS[i + 2 * 3 + 1] << 1;
                const triIndex_t i2c = indexesODS[i + 2 * 3 + 2] << 1;

                WriteIndexPair(&indexBuffer[(numShadowIndices + 0) & OUT_BUFFER_MASK], i2c + 0, i1c + 0);
                WriteIndexPair(&indexBuffer[(numShadowIndices + 2) & OUT_BUFFER_MASK], i0c + 0, i0c + 1);
                WriteIndexPair(&indexBuffer[(numShadowIndices + 4) & OUT_BUFFER_MASK], i1c + 1, i2c + 1);

                numShadowIndices += tc;

                const triIndex_t i0d = indexesODS[i + 3 * 3 + 0] << 1;
                const triIndex_t i1d = indexesODS[i + 3 * 3 + 1] << 1;
                const triIndex_t i2d = indexesODS[i + 3 * 3 + 2] << 1;

                WriteIndexPair(&indexBuffer[(numShadowIndices + 0) & OUT_BUFFER_MASK], i2d + 0, i1d + 0);
                WriteIndexPair(&indexBuffer[(numShadowIndices + 2) & OUT_BUFFER_MASK], i0d + 0, i0d + 1);
                WriteIndexPair(&indexBuffer[(numShadowIndices + 4) & OUT_BUFFER_MASK], i1d + 1, i2d + 1);

                numShadowIndices += td;
            }

            for (; i + 3 <= nextNumIndexes; i += 3, j++) {
                const byte t = ~facing[j] & 6;

                const triIndex_t i0 = indexesODS[i + 0] << 1;
                const triIndex_t i1 = indexesODS[i + 1] << 1;
                const triIndex_t i2 = indexesODS[i + 2] << 1;

                WriteIndexPair(&indexBuffer[(numShadowIndices + 0) & OUT_BUFFER_MASK], i2 + 0, i1 + 0);
                WriteIndexPair(&indexBuffer[(numShadowIndices + 2) & OUT_BUFFER_MASK], i0 + 0, i0 + 1);
                WriteIndexPair(&indexBuffer[(numShadowIndices + 4) & OUT_BUFFER_MASK], i1 + 1, i2 + 1);

                numShadowIndices += t;
            }
        }
    }

    while (numShadowIndices - numStreamedIndices >= OUT_BUFFER_SIZE) {
        StreamOut(shadowIndices + numStreamedIndices, &indexBuffer[numStreamedIndices & OUT_BUFFER_MASK], OUT_BUFFER_SIZE * sizeof(triIndex_t));
        numStreamedIndices += OUT_BUFFER_SIZE;
    }
    if (numShadowIndices > numStreamedIndices) {
        assert(numShadowIndices - numStreamedIndices < OUT_BUFFER_SIZE);
        StreamOut(shadowIndices + numStreamedIndices, &indexBuffer[numStreamedIndices & OUT_BUFFER_MASK], (numShadowIndices - numStreamedIndices) * sizeof(triIndex_t));
    }

    numShadowIndexesTotal = numShadowIndices;

#if defined(USE_INTRINSICS)
    _mm_sfence();
#endif

#else // NOTE: this code will not work on the SPU because it tries to write directly to the destination

    triIndex_t* shadowIndexPtr = shadowIndices;

    {
        idODSStreamedArray<silEdge_t, 128, SBT_DOUBLE, 1> silEdgesODS(silEdges, numSilEdges);

        for (int i = 0; i < numSilEdges;) {

            const int nextNumSilEdges = silEdgesODS.FetchNextBatch() - 1;

            for (; i <= nextNumSilEdges; i++) {
                const silEdge_t& sil = silEdgesODS[i];

                const byte f1 = facing[sil.p1] & 1;
                const byte f2 = facing[sil.p2] & 1;

                if ((f1 ^ f2) == 0) {
                    continue;
                }

                const triIndex_t v1 = sil.v1 << 1;
                const triIndex_t v2 = sil.v2 << 1;

                // set the two triangle winding orders based on facing
                // without using a poorly-predictable branch
#if 1
                // only write dwords to write combined memory
                WriteIndexPair(shadowIndexPtr + 0, v1 ^ 0, v2 ^ f1);
                WriteIndexPair(shadowIndexPtr + 2, v2 ^ f2, v1 ^ f2);
                WriteIndexPair(shadowIndexPtr + 4, v1 ^ f1, v2 ^ 1);
#else
                shadowIndexPtr[0] == v1;
                shadowIndexPtr[1] == v2 ^ f1;
                shadowIndexPtr[2] == v2 ^ f2;
                shadowIndexPtr[3] == v1 ^ f2;
                shadowIndexPtr[4] == v1 ^ f1;
                shadowIndexPtr[5] == v2 ^ 1;
#endif
                shadowIndexPtr += 6;
            }
        }
    }

    if (includeCaps) {
        idODSStreamedArray<triIndex_t, 256, SBT_QUAD, 1> indexesODS(indexes, numIndexes);

        for (int i = 0, j = 0; i < numIndexes;) {

            const int nextNumIndexes = indexesODS.FetchNextBatch() - 3;

            for (; i <= nextNumIndexes; i += 3, j++) {
                if (facing[j]) {
                    continue;
                }

                const triIndex_t i0 = indexesODS[i + 0] << 1;
                const triIndex_t i1 = indexesODS[i + 1] << 1;
                const triIndex_t i2 = indexesODS[i + 2] << 1;
#if 1
                // only write dwords to write combined memory
                WriteIndexPair(shadowIndexPtr + 0, i2 + 0, i1 + 0);
                WriteIndexPair(shadowIndexPtr + 2, i0 + 0, i0 + 1);
                WriteIndexPair(shadowIndexPtr + 4, i1 + 1, i2 + 1);
#else
                shadowIndexPtr[0] = i2;
                shadowIndexPtr[1] = i1;
                shadowIndexPtr[2] = i0;
                shadowIndexPtr[3] = i0 + 1;
                shadowIndexPtr[4] = i1 + 1;
                shadowIndexPtr[5] = i2 + 1;
#endif
                shadowIndexPtr += 6;
            }
        }
    }

    numShadowIndexesTotal = shadowIndexPtr - shadowIndices;

#endif
}

/*
=====================
R_CreateLightTriangles
=====================
*/
void R_CreateLightTriangles(triIndex_t* __restrict lightIndices, triIndex_t* __restrict indexBuffer, int& numLightIndicesTotal,
    const byte* __restrict culled, const triIndex_t* __restrict indexes, const int numIndexes)
{
    assert_spu_local_store(culled);
    assert_not_spu_local_store(lightIndices);
    assert_not_spu_local_store(indexes);

#if 1

    const int IN_BUFFER_SIZE = 256;
    const int OUT_BUFFER_SIZE = IN_BUFFER_SIZE * 2; // there are never more indices generated than the original indices
    const int OUT_BUFFER_DEPTH = 4;                 // quad buffer to allow overlapped output streaming
    const int OUT_BUFFER_MASK = (OUT_BUFFER_SIZE * OUT_BUFFER_DEPTH - 1);

    compile_time_assert(OUT_BUFFER_SIZE * OUT_BUFFER_DEPTH * sizeof(triIndex_t) == OUTPUT_INDEX_BUFFER_SIZE);
    assert_16_byte_aligned(indexBuffer);

    int numLightIndices = 0;
    int numStreamedIndices = 0;

    idODSStreamedArray<triIndex_t, IN_BUFFER_SIZE, SBT_QUAD, 1> indexesODS(indexes, numIndexes);

    for (int i = 0, j = 0; i < numIndexes;) {

        const int nextNumIndexes = indexesODS.FetchNextBatch();

        // NOTE: we rely on FetchNextBatch() to wait for all previous DMAs to complete
        while (numLightIndices - numStreamedIndices >= OUT_BUFFER_SIZE) {
            StreamOut(lightIndices + numStreamedIndices, &indexBuffer[numStreamedIndices & OUT_BUFFER_MASK], OUT_BUFFER_SIZE * sizeof(triIndex_t));
            numStreamedIndices += OUT_BUFFER_SIZE;
        }

        for (; i + 4 * 3 <= nextNumIndexes; i += 4 * 3, j += 4) {
            const byte ta = ~culled[j + 0] & 3;
            const byte tb = ~culled[j + 1] & 3;
            const byte tc = ~culled[j + 2] & 3;
            const byte td = ~culled[j + 3] & 3;

            indexBuffer[(numLightIndices + 0) & OUT_BUFFER_MASK] = indexesODS[i + 0 * 3 + 0];
            indexBuffer[(numLightIndices + 1) & OUT_BUFFER_MASK] = indexesODS[i + 0 * 3 + 1];
            indexBuffer[(numLightIndices + 2) & OUT_BUFFER_MASK] = indexesODS[i + 0 * 3 + 2];

            numLightIndices += ta;

            indexBuffer[(numLightIndices + 0) & OUT_BUFFER_MASK] = indexesODS[i + 1 * 3 + 0];
            indexBuffer[(numLightIndices + 1) & OUT_BUFFER_MASK] = indexesODS[i + 1 * 3 + 1];
            indexBuffer[(numLightIndices + 2) & OUT_BUFFER_MASK] = indexesODS[i + 1 * 3 + 2];

            numLightIndices += tb;

            indexBuffer[(numLightIndices + 0) & OUT_BUFFER_MASK] = indexesODS[i + 2 * 3 + 0];
            indexBuffer[(numLightIndices + 1) & OUT_BUFFER_MASK] = indexesODS[i + 2 * 3 + 1];
            indexBuffer[(numLightIndices + 2) & OUT_BUFFER_MASK] = indexesODS[i + 2 * 3 + 2];

            numLightIndices += tc;

            indexBuffer[(numLightIndices + 0) & OUT_BUFFER_MASK] = indexesODS[i + 3 * 3 + 0];
            indexBuffer[(numLightIndices + 1) & OUT_BUFFER_MASK] = indexesODS[i + 3 * 3 + 1];
            indexBuffer[(numLightIndices + 2) & OUT_BUFFER_MASK] = indexesODS[i + 3 * 3 + 2];

            numLightIndices += td;
        }

        for (; i + 3 <= nextNumIndexes; i += 3, j++) {
            const byte t = ~culled[j] & 3;

            indexBuffer[(numLightIndices + 0) & OUT_BUFFER_MASK] = indexesODS[i + 0];
            indexBuffer[(numLightIndices + 1) & OUT_BUFFER_MASK] = indexesODS[i + 1];
            indexBuffer[(numLightIndices + 2) & OUT_BUFFER_MASK] = indexesODS[i + 2];

            numLightIndices += t;
        }
    }

    while (numLightIndices - numStreamedIndices >= OUT_BUFFER_SIZE) {
        StreamOut(lightIndices + numStreamedIndices, &indexBuffer[numStreamedIndices & OUT_BUFFER_MASK], OUT_BUFFER_SIZE * sizeof(triIndex_t));
        numStreamedIndices += OUT_BUFFER_SIZE;
    }
    if (numLightIndices > numStreamedIndices) {
        assert(numLightIndices - numStreamedIndices < OUT_BUFFER_SIZE);
        StreamOut(lightIndices + numStreamedIndices, &indexBuffer[numStreamedIndices & OUT_BUFFER_MASK], (numLightIndices - numStreamedIndices) * sizeof(triIndex_t));
    }

    numLightIndicesTotal = numLightIndices;

#if defined(USE_INTRINSICS)
    _mm_sfence();
#endif

#else // NOTE: this code will not work on the SPU because it tries to write directly to the destination

    int numLightIndices = 0;

    idODSStreamedArray<triIndex_t, 256, SBT_QUAD, 1> indexesODS(indexes, numIndexes);

    for (int i = 0, j = 0; i < numIndexes;) {

        const int nextNumIndexes = indexesODS.FetchNextBatch() - 3;

        for (; i <= nextNumIndexes; i += 3, j++) {
            if (culled[j]) {
                continue;
            }

            lightIndices[numLightIndices + 0] = indexesODS[i + 0];
            lightIndices[numLightIndices + 1] = indexesODS[i + 1];
            lightIndices[numLightIndices + 2] = indexesODS[i + 2];

            numLightIndices += 3;
        }
    }

    numLightIndicesTotal = numLightIndices;

#endif
}

/*
=====================
DynamicShadowVolumeJob

Creates shadow volume indices for a surface that intersects a light.
Optionally also creates new surface indices with just the triangles
inside the light volume. These indices will be unique for a given
light / surface combination.

The shadow volume indices are created using the original surface vertices.
However, the indices are setup to be used with a shadow volume vertex buffer
with all vertices duplicated where the even vertices have the same positions
as the surface vertices (at the near cap) and each odd vertex has the
same position as the previous even vertex but is projected to infinity
(the far cap) in the vertex program.
=====================
*/
#if defined(__vita__)
int vita_slowShadowJobReports = 0; // the benchmark clears it for each segment
#endif

#if !defined(__vita__) && defined(ID_VITA_PHYSCHECK)
uint64 Sys_Microseconds();
void Vita_ShadowCalibLog(const char* model, int us, int tris, int verts, int skinned, int precise); // VitaBench.cpp
static volatile int pcShadowJobLogs = 0;
#endif

void DynamicShadowVolumeJob(const dynamicShadowVolumeParms_t* parms)
{
#if defined(__vita__)
    const uint64 vitaJobStart = Sys_Microseconds();
#endif
#if !defined(__vita__) && defined(ID_VITA_PHYSCHECK)
    const uint64 pcJobStart = Sys_Microseconds();
#endif
    if (parms->tempFacing == NULL) {
        *const_cast<byte**>(&parms->tempFacing) = (byte*)_alloca16(TEMP_FACING(parms->numIndexes));
    }
    if (parms->tempCulled == NULL) {
        *const_cast<byte**>(&parms->tempCulled) = (byte*)_alloca16(TEMP_CULL(parms->numIndexes));
    }
    if (parms->tempVerts == NULL && parms->joints != NULL) {
        *const_cast<idVec4**>(&parms->tempVerts) = (idVec4*)_alloca16(TEMP_VERTS(parms->numVerts));
    }
    if (parms->indexBuffer == NULL) {
        *const_cast<triIndex_t**>(&parms->indexBuffer) = (triIndex_t*)_alloca16(OUTPUT_INDEX_BUFFER_SIZE);
    }

    assert(parms->joints == NULL || parms->numJoints > 0);

    // Calculate the shadow depth bounds.
    float shadowZMin = parms->lightZMin;
    float shadowZMax = parms->lightZMax;
    if (parms->useShadowDepthBounds) {
        // counted, not timed: ~1 us since the Vita fast path (RenderMatrix.cpp)
        VITA_ZONE_COUNT("shadow job: depth bounds");
        idRenderMatrix::DepthBoundsForShadowBounds(shadowZMin, shadowZMax, parms->triangleMVP, parms->triangleBounds, parms->localLightOrigin, true);
        shadowZMin = Max(shadowZMin, parms->lightZMin);
        shadowZMax = Min(shadowZMax, parms->lightZMax);
    }

    bool renderZFail = false;
    int numShadowIndices = 0;
    int numLightIndices = 0;
#if defined(__vita__)
    bool vitaPotentiallyInside = false;
#endif

    // The shadow volume may be depth culled if either the shadow volume was culled to the view frustum or if the
    // depth range of the visible part of the shadow volume is outside the depth range of the light volume.
    if (shadowZMin < shadowZMax) {

        // Check if we need to render the shadow volume with Z-fail.
        bool* preciseInsideShadowVolume = NULL;
        // If the view is potentially inside the shadow volume bounds we may need to render with Z-fail.
        if (R_ViewPotentiallyInsideInfiniteShadowVolume(parms->triangleBounds, parms->localLightOrigin, parms->localViewOrigin, parms->zNear * INSIDE_SHADOW_VOLUME_EXTRA_STRETCH)) {
#if defined(__vita__)
            vitaPotentiallyInside = true;
#endif
            // Optionally perform a more precise test to see whether or not the view is inside the shadow volume.
            if (parms->useShadowPreciseInsideTest) {
                preciseInsideShadowVolume = &renderZFail;
            } else {
                renderZFail = true;
            }
        }
        if (R_ForceZFailStencilShadows()) {
            // the z-fail check: Z-fail and caps for every volume (no precise
            // test to overwrite it)
            preciseInsideShadowVolume = NULL;
            renderZFail = true;
        }

        // Calculate the facing of each triangle and cull each triangle to the light volume.
        // Optionally also calculate more precisely whether or not the view is inside the shadow volume.
        int numFrontFacing = 0;
        if (parms->joints != NULL) {
            numFrontFacing = CalculateTriangleFacingCulledSkinned(parms->tempFacing, parms->tempCulled, parms->tempVerts, parms->indexes, parms->numIndexes,
                parms->verts, parms->numVerts, parms->joints,
                parms->localLightOrigin, parms->localViewOrigin,
                parms->cullShadowTrianglesToLight, parms->localLightProject,
                preciseInsideShadowVolume, parms->zNear * INSIDE_SHADOW_VOLUME_EXTRA_STRETCH);
        } else {
            numFrontFacing = CalculateTriangleFacingCulledStatic(parms->tempFacing, parms->tempCulled, parms->indexes, parms->numIndexes,
                parms->verts, parms->numVerts,
                parms->localLightOrigin, parms->localViewOrigin,
                parms->cullShadowTrianglesToLight, parms->localLightProject,
                preciseInsideShadowVolume, parms->zNear * INSIDE_SHADOW_VOLUME_EXTRA_STRETCH);
        }

        // Create shadow volume indices.
        if (parms->shadowIndices != NULL) {
            const int numTriangles = parms->numIndexes / 3;

            // If there are any triangles facing away from the light.
            if (numTriangles - numFrontFacing > 0) {
                // Set the "fake triangle" used by dangling edges to facing so a dangling edge will
                // make a silhouette if the triangle that uses the dangling edges is not facing.
                // Note that dangling edges outside the light frustum do not make silhouettes because
                // a triangle outside the light frustum is also set to facing just like the "fake triangle"
                // used by a dangling edge.
                parms->tempFacing[numTriangles] = 255;

                // Check if we can avoid rendering the shadow volume caps.
                bool renderShadowCaps = parms->forceShadowCaps || renderZFail;

                // Create new triangles along the silhouette planes and optionally add end-cap triangles on the model and on the distant projection.
                VITA_ZONE("shadow job: silhouette + cap indices");
                R_CreateShadowVolumeTriangles(parms->shadowIndices, parms->indexBuffer, numShadowIndices, parms->tempFacing,
                    parms->silEdges, parms->numSilEdges, parms->indexes, parms->numIndexes, renderShadowCaps);

                assert(numShadowIndices <= parms->maxShadowIndices);
            }
        }

        // Create new indices with only the triangles that are inside the light volume.
        if (parms->lightIndices != NULL) {
            VITA_ZONE("shadow job: light indices");
            R_CreateLightTriangles(parms->lightIndices, parms->indexBuffer, numLightIndices, parms->tempCulled, parms->indexes, parms->numIndexes);

            assert(numLightIndices <= parms->maxLightIndices);
        }
    }

    // write out the number of shadow indices
    if (parms->numShadowIndices != NULL) {
        *parms->numShadowIndices = numShadowIndices;
    }
    // write out the number of light indices
    if (parms->numLightIndices != NULL) {
        *parms->numLightIndices = numLightIndices;
    }
    // write out whether or not the shadow volume needs to be rendered with Z-Fail
    if (parms->renderZFail != NULL) {
        *parms->renderZFail = renderZFail;
    }
    // write out the shadow depth bounds
    if (parms->shadowZMin != NULL) {
        *parms->shadowZMin = shadowZMin;
    }
    if (parms->shadowZMax != NULL) {
        *parms->shadowZMax = shadowZMax;
    }
    // write out the shadow volume state
    if (parms->shadowVolumeState != NULL) {
        *parms->shadowVolumeState = SHADOWVOLUME_DONE;
    }
#if !defined(__vita__) && defined(ID_VITA_PHYSCHECK)
    {
        // PC calibration of the Vita's slow shadow jobs (temporary)
        const uint64 us = Sys_Microseconds() - pcJobStart;
        if (parms->numIndexes / 3 > 200 && pcShadowJobLogs < 80) {
            pcShadowJobLogs++;
            Vita_ShadowCalibLog(parms->debugModel ? parms->debugModel : "?", (int)us, parms->numIndexes / 3, parms->numVerts, parms->joints != NULL, parms->useShadowPreciseInsideTest ? 1 : 0);
        }
    }
#endif
#if defined(__vita__)
    const uint64 vitaJobUs = Sys_Microseconds() - vitaJobStart;
    if (vitaJobUs > 3000 && vita_slowShadowJobReports < 24) {
        vita_slowShadowJobReports++;
        // the memory each array lives in: the CPU reads uncached memory
        // several times slower, and writes to it go through the write buffer
        Vita_Checkpoint("shadow volume job %.1f ms: model '%s', light %d: %d triangles, %d verts, %s, %d shadow + %d light indices, z-fail %d, precise inside test %d (view potentially inside: %d) | memory: verts %s, indexes %s, silhouette edges %s, shadow index output %s, light index output %s, temp buffers %s",
            vitaJobUs / 1000.0f, parms->debugModel != NULL ? parms->debugModel : "?", parms->debugLight,
            parms->numIndexes / 3, parms->numVerts, parms->joints != NULL ? "skinned" : "static (CPU-side verts)",
            numShadowIndices, numLightIndices, renderZFail ? 1 : 0, parms->useShadowPreciseInsideTest ? 1 : 0, vitaPotentiallyInside ? 1 : 0,
            Vita_MemKind(parms->verts), Vita_MemKind(parms->indexes), Vita_MemKind(parms->silEdges),
            parms->shadowIndices != NULL ? Vita_MemKind(parms->shadowIndices) : "-", parms->lightIndices != NULL ? Vita_MemKind(parms->lightIndices) : "-",
            Vita_MemKind(parms->tempFacing));
    }
#endif
}

REGISTER_PARALLEL_JOB(DynamicShadowVolumeJob, "DynamicShadowVolumeJob");
