/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2014 Robert Beckebans

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

#pragma hdrstop
#include "precompiled.h"

#include "tr_local.h"
#include "Model_local.h"
#include "../sys/vita/vita_zones.h" // VITA_ZONE_ADD: the model trace counts in the perf log

#include "../idlib/geometry/DrawVert_intrinsics.h"

/*
====================
R_TracePointCullStatic
====================
*/
static void R_TracePointCullStatic(byte* cullBits, byte& totalOr, const float radius, const idPlane* planes, const idDrawVert* verts, const int numVerts)
{
    assert_16_byte_aligned(cullBits);
    assert_16_byte_aligned(verts);

#if defined(USE_INTRINSICS)
    idODSStreamedArray<idDrawVert, 16, SBT_DOUBLE, 4> vertsODS(verts, numVerts);

    const __m128 vector_float_radius = _mm_splat_ps(_mm_load_ss(&radius), 0);
    const __m128 vector_float_zero = { 0.0f, 0.0f, 0.0f, 0.0f };
    const __m128i vector_int_mask0 = _mm_set1_epi32(1 << 0);
    const __m128i vector_int_mask1 = _mm_set1_epi32(1 << 1);
    const __m128i vector_int_mask2 = _mm_set1_epi32(1 << 2);
    const __m128i vector_int_mask3 = _mm_set1_epi32(1 << 3);
    const __m128i vector_int_mask4 = _mm_set1_epi32(1 << 4);
    const __m128i vector_int_mask5 = _mm_set1_epi32(1 << 5);
    const __m128i vector_int_mask6 = _mm_set1_epi32(1 << 6);
    const __m128i vector_int_mask7 = _mm_set1_epi32(1 << 7);

    const __m128 p0 = _mm_loadu_ps(planes[0].ToFloatPtr());
    const __m128 p1 = _mm_loadu_ps(planes[1].ToFloatPtr());
    const __m128 p2 = _mm_loadu_ps(planes[2].ToFloatPtr());
    const __m128 p3 = _mm_loadu_ps(planes[3].ToFloatPtr());

    const __m128 p0X = _mm_splat_ps(p0, 0);
    const __m128 p0Y = _mm_splat_ps(p0, 1);
    const __m128 p0Z = _mm_splat_ps(p0, 2);
    const __m128 p0W = _mm_splat_ps(p0, 3);

    const __m128 p1X = _mm_splat_ps(p1, 0);
    const __m128 p1Y = _mm_splat_ps(p1, 1);
    const __m128 p1Z = _mm_splat_ps(p1, 2);
    const __m128 p1W = _mm_splat_ps(p1, 3);

    const __m128 p2X = _mm_splat_ps(p2, 0);
    const __m128 p2Y = _mm_splat_ps(p2, 1);
    const __m128 p2Z = _mm_splat_ps(p2, 2);
    const __m128 p2W = _mm_splat_ps(p2, 3);

    const __m128 p3X = _mm_splat_ps(p3, 0);
    const __m128 p3Y = _mm_splat_ps(p3, 1);
    const __m128 p3Z = _mm_splat_ps(p3, 2);
    const __m128 p3W = _mm_splat_ps(p3, 3);

    __m128i vecTotalOrInt = _mm_set_epi32(0, 0, 0, 0);

    for (int i = 0; i < numVerts;) {

        const int nextNumVerts = vertsODS.FetchNextBatch() - 4;

        for (; i <= nextNumVerts; i += 4) {
            const __m128 v0 = _mm_load_ps(vertsODS[i + 0].xyz.ToFloatPtr());
            const __m128 v1 = _mm_load_ps(vertsODS[i + 1].xyz.ToFloatPtr());
            const __m128 v2 = _mm_load_ps(vertsODS[i + 2].xyz.ToFloatPtr());
            const __m128 v3 = _mm_load_ps(vertsODS[i + 3].xyz.ToFloatPtr());

            const __m128 r0 = _mm_unpacklo_ps(v0, v2); // v0.x, v2.x, v0.z, v2.z
            const __m128 r1 = _mm_unpackhi_ps(v0, v2); // v0.y, v2.y, v0.w, v2.w
            const __m128 r2 = _mm_unpacklo_ps(v1, v3); // v1.x, v3.x, v1.z, v3.z
            const __m128 r3 = _mm_unpackhi_ps(v1, v3); // v1.y, v3.y, v1.w, v3.w

            const __m128 vX = _mm_unpacklo_ps(r0, r2); // v0.x, v1.x, v2.x, v3.x
            const __m128 vY = _mm_unpackhi_ps(r0, r2); // v0.y, v1.y, v2.y, v3.y
            const __m128 vZ = _mm_unpacklo_ps(r1, r3); // v0.z, v1.z, v2.z, v3.z

            const __m128 d0 = _mm_madd_ps(vX, p0X, _mm_madd_ps(vY, p0Y, _mm_madd_ps(vZ, p0Z, p0W)));
            const __m128 d1 = _mm_madd_ps(vX, p1X, _mm_madd_ps(vY, p1Y, _mm_madd_ps(vZ, p1Z, p1W)));
            const __m128 d2 = _mm_madd_ps(vX, p2X, _mm_madd_ps(vY, p2Y, _mm_madd_ps(vZ, p2Z, p2W)));
            const __m128 d3 = _mm_madd_ps(vX, p3X, _mm_madd_ps(vY, p3Y, _mm_madd_ps(vZ, p3Z, p3W)));

            const __m128 t0 = _mm_add_ps(d0, vector_float_radius);
            const __m128 t1 = _mm_add_ps(d1, vector_float_radius);
            const __m128 t2 = _mm_add_ps(d2, vector_float_radius);
            const __m128 t3 = _mm_add_ps(d3, vector_float_radius);

            const __m128 t4 = _mm_sub_ps(d0, vector_float_radius);
            const __m128 t5 = _mm_sub_ps(d1, vector_float_radius);
            const __m128 t6 = _mm_sub_ps(d2, vector_float_radius);
            const __m128 t7 = _mm_sub_ps(d3, vector_float_radius);

            __m128i c0 = __m128c(_mm_cmpgt_ps(t0, vector_float_zero));
            __m128i c1 = __m128c(_mm_cmpgt_ps(t1, vector_float_zero));
            __m128i c2 = __m128c(_mm_cmpgt_ps(t2, vector_float_zero));
            __m128i c3 = __m128c(_mm_cmpgt_ps(t3, vector_float_zero));

            __m128i c4 = __m128c(_mm_cmplt_ps(t4, vector_float_zero));
            __m128i c5 = __m128c(_mm_cmplt_ps(t5, vector_float_zero));
            __m128i c6 = __m128c(_mm_cmplt_ps(t6, vector_float_zero));
            __m128i c7 = __m128c(_mm_cmplt_ps(t7, vector_float_zero));

            c0 = _mm_and_si128(c0, vector_int_mask0);
            c1 = _mm_and_si128(c1, vector_int_mask1);
            c2 = _mm_and_si128(c2, vector_int_mask2);
            c3 = _mm_and_si128(c3, vector_int_mask3);

            c4 = _mm_and_si128(c4, vector_int_mask4);
            c5 = _mm_and_si128(c5, vector_int_mask5);
            c6 = _mm_and_si128(c6, vector_int_mask6);
            c7 = _mm_and_si128(c7, vector_int_mask7);

            c0 = _mm_or_si128(c0, c1);
            c2 = _mm_or_si128(c2, c3);
            c4 = _mm_or_si128(c4, c5);
            c6 = _mm_or_si128(c6, c7);

            c0 = _mm_or_si128(c0, c2);
            c4 = _mm_or_si128(c4, c6);
            c0 = _mm_or_si128(c0, c4);

            vecTotalOrInt = _mm_or_si128(vecTotalOrInt, c0);

            __m128i s0 = _mm_packs_epi32(c0, c0);
            __m128i b0 = _mm_packus_epi16(s0, s0);

            *(unsigned int*)&cullBits[i] = _mm_cvtsi128_si32(b0);
        }
    }

    vecTotalOrInt = _mm_or_si128(vecTotalOrInt, _mm_shuffle_epi32(vecTotalOrInt, _MM_SHUFFLE(1, 0, 3, 2)));
    vecTotalOrInt = _mm_or_si128(vecTotalOrInt, _mm_shuffle_epi32(vecTotalOrInt, _MM_SHUFFLE(2, 3, 0, 1)));

    __m128i vecTotalOrShort = _mm_packs_epi32(vecTotalOrInt, vecTotalOrInt);
    __m128i vecTotalOrByte = _mm_packus_epi16(vecTotalOrShort, vecTotalOrShort);

    totalOr = (byte)_mm_cvtsi128_si32(vecTotalOrByte);

#else

    idODSStreamedArray<idDrawVert, 16, SBT_DOUBLE, 1> vertsODS(verts, numVerts);

    byte tOr = 0;
    for (int i = 0; i < numVerts;) {

        const int nextNumVerts = vertsODS.FetchNextBatch() - 1;

        for (; i <= nextNumVerts; i++) {
            const idVec3& v = vertsODS[i].xyz;

            const float d0 = planes[0].Distance(v);
            const float d1 = planes[1].Distance(v);
            const float d2 = planes[2].Distance(v);
            const float d3 = planes[3].Distance(v);

            const float t0 = d0 + radius;
            const float t1 = d1 + radius;
            const float t2 = d2 + radius;
            const float t3 = d3 + radius;

            const float s0 = d0 - radius;
            const float s1 = d1 - radius;
            const float s2 = d2 - radius;
            const float s3 = d3 - radius;

            byte bits;
            bits = IEEE_FLT_SIGNBITSET(t0) << 0;
            bits |= IEEE_FLT_SIGNBITSET(t1) << 1;
            bits |= IEEE_FLT_SIGNBITSET(t2) << 2;
            bits |= IEEE_FLT_SIGNBITSET(t3) << 3;

            bits |= IEEE_FLT_SIGNBITSET(s0) << 4;
            bits |= IEEE_FLT_SIGNBITSET(s1) << 5;
            bits |= IEEE_FLT_SIGNBITSET(s2) << 6;
            bits |= IEEE_FLT_SIGNBITSET(s3) << 7;

            bits ^= 0x0F; // flip lower four bits

            tOr |= bits;
            cullBits[i] = bits;
        }
    }

    totalOr = tOr;

#endif
}

/*
====================
R_TracePointCullSkinned
====================
*/
static void R_TracePointCullSkinned(byte* cullBits, byte& totalOr, const float radius, const idPlane* planes, const idDrawVert* verts, const int numVerts, const idJointMat* joints)
{
    assert_16_byte_aligned(cullBits);
    assert_16_byte_aligned(verts);

#if defined(USE_INTRINSICS)
    idODSStreamedArray<idDrawVert, 16, SBT_DOUBLE, 4> vertsODS(verts, numVerts);

    const __m128 vector_float_radius = _mm_splat_ps(_mm_load_ss(&radius), 0);
    const __m128 vector_float_zero = { 0.0f, 0.0f, 0.0f, 0.0f };
    const __m128i vector_int_mask0 = _mm_set1_epi32(1 << 0);
    const __m128i vector_int_mask1 = _mm_set1_epi32(1 << 1);
    const __m128i vector_int_mask2 = _mm_set1_epi32(1 << 2);
    const __m128i vector_int_mask3 = _mm_set1_epi32(1 << 3);
    const __m128i vector_int_mask4 = _mm_set1_epi32(1 << 4);
    const __m128i vector_int_mask5 = _mm_set1_epi32(1 << 5);
    const __m128i vector_int_mask6 = _mm_set1_epi32(1 << 6);
    const __m128i vector_int_mask7 = _mm_set1_epi32(1 << 7);

    const __m128 p0 = _mm_loadu_ps(planes[0].ToFloatPtr());
    const __m128 p1 = _mm_loadu_ps(planes[1].ToFloatPtr());
    const __m128 p2 = _mm_loadu_ps(planes[2].ToFloatPtr());
    const __m128 p3 = _mm_loadu_ps(planes[3].ToFloatPtr());

    const __m128 p0X = _mm_splat_ps(p0, 0);
    const __m128 p0Y = _mm_splat_ps(p0, 1);
    const __m128 p0Z = _mm_splat_ps(p0, 2);
    const __m128 p0W = _mm_splat_ps(p0, 3);

    const __m128 p1X = _mm_splat_ps(p1, 0);
    const __m128 p1Y = _mm_splat_ps(p1, 1);
    const __m128 p1Z = _mm_splat_ps(p1, 2);
    const __m128 p1W = _mm_splat_ps(p1, 3);

    const __m128 p2X = _mm_splat_ps(p2, 0);
    const __m128 p2Y = _mm_splat_ps(p2, 1);
    const __m128 p2Z = _mm_splat_ps(p2, 2);
    const __m128 p2W = _mm_splat_ps(p2, 3);

    const __m128 p3X = _mm_splat_ps(p3, 0);
    const __m128 p3Y = _mm_splat_ps(p3, 1);
    const __m128 p3Z = _mm_splat_ps(p3, 2);
    const __m128 p3W = _mm_splat_ps(p3, 3);

    __m128i vecTotalOrInt = _mm_set_epi32(0, 0, 0, 0);

    for (int i = 0; i < numVerts;) {

        const int nextNumVerts = vertsODS.FetchNextBatch() - 4;

        for (; i <= nextNumVerts; i += 4) {
            const __m128 v0 = LoadSkinnedDrawVertPosition(vertsODS[i + 0], joints);
            const __m128 v1 = LoadSkinnedDrawVertPosition(vertsODS[i + 1], joints);
            const __m128 v2 = LoadSkinnedDrawVertPosition(vertsODS[i + 2], joints);
            const __m128 v3 = LoadSkinnedDrawVertPosition(vertsODS[i + 3], joints);

            const __m128 r0 = _mm_unpacklo_ps(v0, v2); // v0.x, v2.x, v0.z, v2.z
            const __m128 r1 = _mm_unpackhi_ps(v0, v2); // v0.y, v2.y, v0.w, v2.w
            const __m128 r2 = _mm_unpacklo_ps(v1, v3); // v1.x, v3.x, v1.z, v3.z
            const __m128 r3 = _mm_unpackhi_ps(v1, v3); // v1.y, v3.y, v1.w, v3.w

            const __m128 vX = _mm_unpacklo_ps(r0, r2); // v0.x, v1.x, v2.x, v3.x
            const __m128 vY = _mm_unpackhi_ps(r0, r2); // v0.y, v1.y, v2.y, v3.y
            const __m128 vZ = _mm_unpacklo_ps(r1, r3); // v0.z, v1.z, v2.z, v3.z

            const __m128 d0 = _mm_madd_ps(vX, p0X, _mm_madd_ps(vY, p0Y, _mm_madd_ps(vZ, p0Z, p0W)));
            const __m128 d1 = _mm_madd_ps(vX, p1X, _mm_madd_ps(vY, p1Y, _mm_madd_ps(vZ, p1Z, p1W)));
            const __m128 d2 = _mm_madd_ps(vX, p2X, _mm_madd_ps(vY, p2Y, _mm_madd_ps(vZ, p2Z, p2W)));
            const __m128 d3 = _mm_madd_ps(vX, p3X, _mm_madd_ps(vY, p3Y, _mm_madd_ps(vZ, p3Z, p3W)));

            const __m128 t0 = _mm_add_ps(d0, vector_float_radius);
            const __m128 t1 = _mm_add_ps(d1, vector_float_radius);
            const __m128 t2 = _mm_add_ps(d2, vector_float_radius);
            const __m128 t3 = _mm_add_ps(d3, vector_float_radius);

            const __m128 t4 = _mm_sub_ps(d0, vector_float_radius);
            const __m128 t5 = _mm_sub_ps(d1, vector_float_radius);
            const __m128 t6 = _mm_sub_ps(d2, vector_float_radius);
            const __m128 t7 = _mm_sub_ps(d3, vector_float_radius);

            __m128i c0 = __m128c(_mm_cmpgt_ps(t0, vector_float_zero));
            __m128i c1 = __m128c(_mm_cmpgt_ps(t1, vector_float_zero));
            __m128i c2 = __m128c(_mm_cmpgt_ps(t2, vector_float_zero));
            __m128i c3 = __m128c(_mm_cmpgt_ps(t3, vector_float_zero));

            __m128i c4 = __m128c(_mm_cmplt_ps(t4, vector_float_zero));
            __m128i c5 = __m128c(_mm_cmplt_ps(t5, vector_float_zero));
            __m128i c6 = __m128c(_mm_cmplt_ps(t6, vector_float_zero));
            __m128i c7 = __m128c(_mm_cmplt_ps(t7, vector_float_zero));

            c0 = _mm_and_si128(c0, vector_int_mask0);
            c1 = _mm_and_si128(c1, vector_int_mask1);
            c2 = _mm_and_si128(c2, vector_int_mask2);
            c3 = _mm_and_si128(c3, vector_int_mask3);

            c4 = _mm_and_si128(c4, vector_int_mask4);
            c5 = _mm_and_si128(c5, vector_int_mask5);
            c6 = _mm_and_si128(c6, vector_int_mask6);
            c7 = _mm_and_si128(c7, vector_int_mask7);

            c0 = _mm_or_si128(c0, c1);
            c2 = _mm_or_si128(c2, c3);
            c4 = _mm_or_si128(c4, c5);
            c6 = _mm_or_si128(c6, c7);

            c0 = _mm_or_si128(c0, c2);
            c4 = _mm_or_si128(c4, c6);
            c0 = _mm_or_si128(c0, c4);

            vecTotalOrInt = _mm_or_si128(vecTotalOrInt, c0);

            __m128i s0 = _mm_packs_epi32(c0, c0);
            __m128i b0 = _mm_packus_epi16(s0, s0);

            *(unsigned int*)&cullBits[i] = _mm_cvtsi128_si32(b0);
        }
    }

    vecTotalOrInt = _mm_or_si128(vecTotalOrInt, _mm_shuffle_epi32(vecTotalOrInt, _MM_SHUFFLE(1, 0, 3, 2)));
    vecTotalOrInt = _mm_or_si128(vecTotalOrInt, _mm_shuffle_epi32(vecTotalOrInt, _MM_SHUFFLE(2, 3, 0, 1)));

    __m128i vecTotalOrShort = _mm_packs_epi32(vecTotalOrInt, vecTotalOrInt);
    __m128i vecTotalOrByte = _mm_packus_epi16(vecTotalOrShort, vecTotalOrShort);

    totalOr = (byte)_mm_cvtsi128_si32(vecTotalOrByte);

#else

    idODSStreamedArray<idDrawVert, 16, SBT_DOUBLE, 1> vertsODS(verts, numVerts);

    byte tOr = 0;
    for (int i = 0; i < numVerts;) {

        const int nextNumVerts = vertsODS.FetchNextBatch() - 1;

        for (; i <= nextNumVerts; i++) {
            const idVec3 v = Scalar_LoadSkinnedDrawVertPosition(vertsODS[i], joints);

            const float d0 = planes[0].Distance(v);
            const float d1 = planes[1].Distance(v);
            const float d2 = planes[2].Distance(v);
            const float d3 = planes[3].Distance(v);

            const float t0 = d0 + radius;
            const float t1 = d1 + radius;
            const float t2 = d2 + radius;
            const float t3 = d3 + radius;

            const float s0 = d0 - radius;
            const float s1 = d1 - radius;
            const float s2 = d2 - radius;
            const float s3 = d3 - radius;

            byte bits;
            bits = IEEE_FLT_SIGNBITSET(t0) << 0;
            bits |= IEEE_FLT_SIGNBITSET(t1) << 1;
            bits |= IEEE_FLT_SIGNBITSET(t2) << 2;
            bits |= IEEE_FLT_SIGNBITSET(t3) << 3;

            bits |= IEEE_FLT_SIGNBITSET(s0) << 4;
            bits |= IEEE_FLT_SIGNBITSET(s1) << 5;
            bits |= IEEE_FLT_SIGNBITSET(s2) << 6;
            bits |= IEEE_FLT_SIGNBITSET(s3) << 7;

            bits ^= 0x0F; // flip lower four bits

            tOr |= bits;
            cullBits[i] = bits;
        }
    }

    totalOr = tOr;

#endif
}

/*
====================
R_LineIntersectsTriangleExpandedWithCircle

The triangle is expanded in the plane with a circle of the given radius.
====================
*/
static bool R_LineIntersectsTriangleExpandedWithCircle(localTrace_t& hit, const idVec3& start, const idVec3& end, const float circleRadius, const idVec3& triVert0, const idVec3& triVert1, const idVec3& triVert2)
{
    const idPlane plane(triVert0, triVert1, triVert2);

    const float planeDistStart = plane.Distance(start);
    const float planeDistEnd = plane.Distance(end);

    if (planeDistStart < 0.0f) {
        return false; // starts past the triangle
    }

    if (planeDistEnd > 0.0f) {
        return false; // finishes in front of the triangle
    }

    const float planeDelta = planeDistStart - planeDistEnd;

    if (planeDelta < idMath::FLT_SMALLEST_NON_DENORMAL) {
        return false; // coming at the triangle from behind or parallel
    }

    const float fraction = planeDistStart / planeDelta;

    if (fraction < 0.0f) {
        return false; // shouldn't happen
    }

    if (fraction >= hit.fraction) {
        return false; // have already hit something closer
    }

    // find the exact point of impact with the plane
    const idVec3 point = start + fraction * (end - start);

    // see if the point is within the three edges
    // if radius > 0 the triangle is expanded with a circle in the triangle plane

    const float radiusSqr = circleRadius * circleRadius;

    const idVec3 dir0 = triVert0 - point;
    const idVec3 dir1 = triVert1 - point;

    const idVec3 cross0 = dir0.Cross(dir1);
    float d0 = plane.Normal() * cross0;
    if (d0 > 0.0f) {
        if (radiusSqr <= 0.0f) {
            return false;
        }
        idVec3 edge = triVert0 - triVert1;
        const float edgeLengthSqr = edge.LengthSqr();
        if (cross0.LengthSqr() > edgeLengthSqr * radiusSqr) {
            return false;
        }
        d0 = edge * dir0;
        if (d0 < 0.0f) {
            edge = triVert0 - triVert2;
            d0 = edge * dir0;
            if (d0 < 0.0f) {
                if (dir0.LengthSqr() > radiusSqr) {
                    return false;
                }
            }
        } else if (d0 > edgeLengthSqr) {
            edge = triVert1 - triVert2;
            d0 = edge * dir1;
            if (d0 < 0.0f) {
                if (dir1.LengthSqr() > radiusSqr) {
                    return false;
                }
            }
        }
    }

    const idVec3 dir2 = triVert2 - point;

    const idVec3 cross1 = dir1.Cross(dir2);
    float d1 = plane.Normal() * cross1;
    if (d1 > 0.0f) {
        if (radiusSqr <= 0.0f) {
            return false;
        }
        idVec3 edge = triVert1 - triVert2;
        const float edgeLengthSqr = edge.LengthSqr();
        if (cross1.LengthSqr() > edgeLengthSqr * radiusSqr) {
            return false;
        }
        d1 = edge * dir1;
        if (d1 < 0.0f) {
            edge = triVert1 - triVert0;
            d1 = edge * dir1;
            if (d1 < 0.0f) {
                if (dir1.LengthSqr() > radiusSqr) {
                    return false;
                }
            }
        } else if (d1 > edgeLengthSqr) {
            edge = triVert2 - triVert0;
            d1 = edge * dir2;
            if (d1 < 0.0f) {
                if (dir2.LengthSqr() > radiusSqr) {
                    return false;
                }
            }
        }
    }

    const idVec3 cross2 = dir2.Cross(dir0);
    float d2 = plane.Normal() * cross2;
    if (d2 > 0.0f) {
        if (radiusSqr <= 0.0f) {
            return false;
        }
        idVec3 edge = triVert2 - triVert0;
        const float edgeLengthSqr = edge.LengthSqr();
        if (cross2.LengthSqr() > edgeLengthSqr * radiusSqr) {
            return false;
        }
        d2 = edge * dir2;
        if (d2 < 0.0f) {
            edge = triVert2 - triVert1;
            d2 = edge * dir2;
            if (d2 < 0.0f) {
                if (dir2.LengthSqr() > radiusSqr) {
                    return false;
                }
            }
        } else if (d2 > edgeLengthSqr) {
            edge = triVert0 - triVert1;
            d2 = edge * dir0;
            if (d2 < 0.0f) {
                if (dir0.LengthSqr() > radiusSqr) {
                    return false;
                }
            }
        }
    }

    // we hit this triangle
    hit.fraction = fraction;
    hit.normal = plane.Normal();
    hit.point = point;

    return true;
}

/*
====================
R_LocalTrace
====================
*/
localTrace_t R_LocalTrace(const idVec3& start, const idVec3& end, const float radius, const srfTriangles_t* tri)
{
    localTrace_t hit;
    hit.fraction = 1.0f;

    ALIGNTYPE16 idPlane planes[4];
    // create two planes orthogonal to each other that intersect along the trace
    idVec3 startDir = end - start;
    startDir.Normalize();
    startDir.NormalVectors(planes[0].Normal(), planes[1].Normal());
    planes[0][3] = -start * planes[0].Normal();
    planes[1][3] = -start * planes[1].Normal();
    // create front and end planes so the trace is on the positive sides of both
    planes[2] = startDir;
    planes[2][3] = -start * planes[2].Normal();
    planes[3] = -startDir;
    planes[3][3] = -end * planes[3].Normal();

    // catagorize each point against the four planes
    byte* cullBits = (byte*)_alloca16(ALIGN(tri->numVerts, 4)); // round up to a multiple of 4 for SIMD
    byte totalOr = 0;

    // RB: added check wether GPU skinning is available at all
    const idJointMat* joints = (tri->staticModelWithJoints != NULL && r_useGPUSkinning.GetBool() && glConfig.gpuSkinningAvailable) ? tri->staticModelWithJoints->jointsInverted : NULL;
    // RB end

    if (joints != NULL) {
        R_TracePointCullSkinned(cullBits, totalOr, radius, planes, tri->verts, tri->numVerts, joints);
    } else {
        R_TracePointCullStatic(cullBits, totalOr, radius, planes, tri->verts, tri->numVerts);
    }

    // if we don't have points on both sides of both the ray planes, no intersection
    if ((totalOr ^ (totalOr >> 4)) & 3) {
        return hit;
    }

    // if we don't have any points between front and end, no intersection
    if ((totalOr ^ (totalOr >> 1)) & 4) {
        return hit;
    }

    // start streaming the indexes
    idODSStreamedArray<triIndex_t, 256, SBT_QUAD, 3> indexesODS(tri->indexes, tri->numIndexes);

    for (int i = 0; i < tri->numIndexes;) {

        const int nextNumIndexes = indexesODS.FetchNextBatch() - 3;

        for (; i <= nextNumIndexes; i += 3) {
            const int i0 = indexesODS[i + 0];
            const int i1 = indexesODS[i + 1];
            const int i2 = indexesODS[i + 2];

            // get sidedness info for the triangle
            const byte triOr = cullBits[i0] | cullBits[i1] | cullBits[i2];

            // if we don't have points on both sides of both the ray planes, no intersection
            if (likely((triOr ^ (triOr >> 4)) & 3)) {
                continue;
            }

            // if we don't have any points between front and end, no intersection
            if (unlikely((triOr ^ (triOr >> 1)) & 4)) {
                continue;
            }

            const idVec3 triVert0 = idDrawVert::GetSkinnedDrawVertPosition(idODSObject<idDrawVert>(&tri->verts[i0]), joints);
            const idVec3 triVert1 = idDrawVert::GetSkinnedDrawVertPosition(idODSObject<idDrawVert>(&tri->verts[i1]), joints);
            const idVec3 triVert2 = idDrawVert::GetSkinnedDrawVertPosition(idODSObject<idDrawVert>(&tri->verts[i2]), joints);

            if (R_LineIntersectsTriangleExpandedWithCircle(hit, start, end, radius, triVert0, triVert1, triVert2)) {
                hit.indexes[0] = i0;
                hit.indexes[1] = i1;
                hit.indexes[2] = i2;
            }
        }
    }

    return hit;
}

#if defined(__vita__) && defined(__ARM_NEON__)
#include <arm_neon.h>
/*
====================
Vita_TraceCullBitsPositions_NEON

R_LocalTracePositions' per-vertex sidedness bits, four vertices at a time:
the same distances (a*x + b*y + c*z + d, in that order), the same sign bits,
so the same bytes (tools/vita/neontest).
====================
*/
static void Vita_TraceCullBitsPositions_NEON(byte* cullBits, byte& totalOr, const float radius, const float* planes, const float* xyz, const int numVerts)
{
    const float32x4_t r = vdupq_n_f32(radius);
    // the four planes loaded once and multiplied by lane (as float scalars GCC
    // re-materialized them inside the loop); the same multiply-adds
    const float32x4_t pl[4] = { vld1q_f32(planes + 0), vld1q_f32(planes + 4), vld1q_f32(planes + 8), vld1q_f32(planes + 12) };
    uint32x4_t total = vdupq_n_u32(0);
    int i = 0;
    for (; i + 4 <= numVerts; i += 4) {
        const float32x4x3_t v = vld3q_f32(xyz + i * 3); // x, y, z of four vertices
        uint32x4_t bits = vdupq_n_u32(0);
        for (int k = 0; k < 4; k++) {
            const float32x2_t p01 = vget_low_f32(pl[k]);
            const float32x2_t p23 = vget_high_f32(pl[k]);
            const float32x4_t d = vaddq_f32(vmlaq_lane_f32(vmlaq_lane_f32(vmulq_lane_f32(v.val[0], p01, 0), v.val[1], p01, 1), v.val[2], p23, 0), vdupq_lane_f32(p23, 1));
            const uint32x4_t tSign = vshrq_n_u32(vreinterpretq_u32_f32(vaddq_f32(d, r)), 31);
            const uint32x4_t sSign = vshrq_n_u32(vreinterpretq_u32_f32(vsubq_f32(d, r)), 31);
            bits = vorrq_u32(bits, vshlq_u32(tSign, vdupq_n_s32(k)));
            bits = vorrq_u32(bits, vshlq_u32(sSign, vdupq_n_s32(k + 4)));
        }
        bits = veorq_u32(bits, vdupq_n_u32(0x0F)); // flip lower four bits
        total = vorrq_u32(total, bits);
        const uint16x4_t b16 = vmovn_u32(bits);
        const uint8x8_t b8 = vmovn_u16(vcombine_u16(b16, b16));
        vst1_lane_u32((uint32_t*)(cullBits + i), vreinterpret_u32_u8(b8), 0);
    }
    const uint32x2_t t2 = vorr_u32(vget_low_u32(total), vget_high_u32(total));
    byte tOr = (byte)(vget_lane_u32(t2, 0) | vget_lane_u32(t2, 1));
    for (; i < numVerts; i++) {
        const float* vp = xyz + i * 3;
        byte bits = 0;
        for (int k = 0; k < 4; k++) {
            const float* p = planes + k * 4;
            const float d = p[0] * vp[0] + p[1] * vp[1] + p[2] * vp[2] + p[3];
            const float tt = d + radius;
            const float s = d - radius;
            bits |= (byte)(IEEE_FLT_SIGNBITSET(tt) << k);
            bits |= (byte)(IEEE_FLT_SIGNBITSET(s) << (k + 4));
        }
        bits ^= 0x0F;
        tOr |= bits;
        cullBits[i] = bits;
    }
    totalOr = tOr;
}
#endif

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
/*
====================
R_LocalTracePositions

R_LocalTrace for a surface given as bare positions - the same planes, the same
per-vertex sidedness bits (R_TracePointCullStatic's generic code) and the same
triangle tests, so the same result as R_LocalTrace on draw verts holding those
positions.
====================
*/
localTrace_t R_LocalTracePositions(const idVec3& start, const idVec3& end, const float radius, const idVec3* xyz, const int numVerts, const triIndex_t* indexes, const int numIndexes)
{
    localTrace_t hit;
    hit.fraction = 1.0f;

    ALIGNTYPE16 idPlane planes[4];
    // create two planes orthogonal to each other that intersect along the trace
    idVec3 startDir = end - start;
    startDir.Normalize();
    startDir.NormalVectors(planes[0].Normal(), planes[1].Normal());
    planes[0][3] = -start * planes[0].Normal();
    planes[1][3] = -start * planes[1].Normal();
    // create front and end planes so the trace is on the positive sides of both
    planes[2] = startDir;
    planes[2][3] = -start * planes[2].Normal();
    planes[3] = -startDir;
    planes[3][3] = -end * planes[3].Normal();

    // catagorize each point against the four planes
    byte* cullBits = (byte*)_alloca16(ALIGN(numVerts, 4));
    byte totalOr = 0;
    VITA_ZONE_ADD("model trace: vertices given sidedness bits", numVerts);
#if defined(__vita__) && defined(__ARM_NEON__)
    Vita_TraceCullBitsPositions_NEON(cullBits, totalOr, radius, planes[0].ToFloatPtr(), xyz[0].ToFloatPtr(), numVerts);
#else
    for (int i = 0; i < numVerts; i++) {
        const idVec3& v = xyz[i];

        const float d0 = planes[0].Distance(v);
        const float d1 = planes[1].Distance(v);
        const float d2 = planes[2].Distance(v);
        const float d3 = planes[3].Distance(v);

        const float t0 = d0 + radius;
        const float t1 = d1 + radius;
        const float t2 = d2 + radius;
        const float t3 = d3 + radius;

        const float s0 = d0 - radius;
        const float s1 = d1 - radius;
        const float s2 = d2 - radius;
        const float s3 = d3 - radius;

        byte bits;
        bits = IEEE_FLT_SIGNBITSET(t0) << 0;
        bits |= IEEE_FLT_SIGNBITSET(t1) << 1;
        bits |= IEEE_FLT_SIGNBITSET(t2) << 2;
        bits |= IEEE_FLT_SIGNBITSET(t3) << 3;

        bits |= IEEE_FLT_SIGNBITSET(s0) << 4;
        bits |= IEEE_FLT_SIGNBITSET(s1) << 5;
        bits |= IEEE_FLT_SIGNBITSET(s2) << 6;
        bits |= IEEE_FLT_SIGNBITSET(s3) << 7;

        bits ^= 0x0F; // flip lower four bits

        totalOr |= bits;
        cullBits[i] = bits;
    }
#endif

    // if we don't have points on both sides of both the ray planes, no intersection
    if ((totalOr ^ (totalOr >> 4)) & 3) {
        return hit;
    }

    // if we don't have any points between front and end, no intersection
    if ((totalOr ^ (totalOr >> 1)) & 4) {
        return hit;
    }

    VITA_ZONE_ADD("model trace: triangles culled by sidedness", numIndexes / 3);
    int exactTests = 0;
    for (int i = 0; i + 3 <= numIndexes; i += 3) {
        const int i0 = indexes[i + 0];
        const int i1 = indexes[i + 1];
        const int i2 = indexes[i + 2];

        // get sidedness info for the triangle
        const byte triOr = cullBits[i0] | cullBits[i1] | cullBits[i2];

        // if we don't have points on both sides of both the ray planes, no intersection
        if (likely((triOr ^ (triOr >> 4)) & 3)) {
            continue;
        }

        // if we don't have any points between front and end, no intersection
        if (unlikely((triOr ^ (triOr >> 1)) & 4)) {
            continue;
        }

        exactTests++;
        if (R_LineIntersectsTriangleExpandedWithCircle(hit, start, end, radius, xyz[i0], xyz[i1], xyz[i2])) {
            hit.indexes[0] = i0;
            hit.indexes[1] = i1;
            hit.indexes[2] = i2;
        }
    }
    VITA_ZONE_ADD("model trace: triangles given the exact (radius) test", exactTests);
    if (radius > 0.0f) {
        VITA_ZONE_COUNT("model trace: traces with a radius");
    }

    return hit;
}

/*
====================
R_LocalTracePositionsSubsets

R_LocalTracePositions on some of a surface's triangles: the same planes, the
same sidedness bits for the vertices those triangles use, the same triangle
tests. The whole-surface loop keeps the closest hit and, on a tie, the
triangle first in the surface (a later one must be strictly closer); here the
triangles come in another order, so a tie goes to the lower place in the
surface explicitly - the same hit as the whole loop's when the triangles left
out cannot be hit. (Its early outs over all vertices only skip a loop that
would hit nothing, so leaving them out changes no result.)

Game/draw thread only, like the traces (one scratch buffer).
====================
*/
localTrace_t R_LocalTracePositionsSubsets(const idVec3& start, const idVec3& end, const float radius, const idVec3* xyz, const int numVerts,
    const vitaTraceSubset_t* subsets, const int numSubsets)
{
    localTrace_t hit;
    hit.fraction = 1.0f;

    ALIGNTYPE16 idPlane planes[4];
    // create two planes orthogonal to each other that intersect along the trace
    idVec3 startDir = end - start;
    startDir.Normalize();
    startDir.NormalVectors(planes[0].Normal(), planes[1].Normal());
    planes[0][3] = -start * planes[0].Normal();
    planes[1][3] = -start * planes[1].Normal();
    // create front and end planes so the trace is on the positive sides of both
    planes[2] = startDir;
    planes[2][3] = -start * planes[2].Normal();
    planes[3] = -startDir;
    planes[3][3] = -end * planes[3].Normal();

    // the sidedness bits of the vertices the subsets use: gathered, then
    // computed exactly as for a whole surface, then put back in place
    static idVec3* gathered = NULL;
    static byte* gatheredBits = NULL;
    static byte* cullBits = NULL;
    static int capacity = 0;
    if (numVerts + 4 > capacity) {
        Mem_Free16(gathered);
        Mem_Free16(gatheredBits);
        Mem_Free16(cullBits);
        capacity = ALIGN(numVerts + 4, 256);
        gathered = (idVec3*)Mem_Alloc16(capacity * sizeof(idVec3), TAG_RENDER);
        gatheredBits = (byte*)Mem_Alloc16(capacity, TAG_RENDER);
        cullBits = (byte*)Mem_Alloc16(capacity, TAG_RENDER);
    }
    int vertsBits = 0;
    for (int s = 0; s < numSubsets; s++) {
        const vitaTraceSubset_t& sub = subsets[s];
        for (int v = 0; v < sub.numVerts; v++) {
            gathered[v] = xyz[sub.verts[v]];
        }
        byte totalOr = 0;
#if defined(__vita__) && defined(__ARM_NEON__)
        Vita_TraceCullBitsPositions_NEON(gatheredBits, totalOr, radius, planes[0].ToFloatPtr(), gathered[0].ToFloatPtr(), sub.numVerts);
#else
        for (int i = 0; i < sub.numVerts; i++) {
            const idVec3& v = gathered[i];

            const float d0 = planes[0].Distance(v);
            const float d1 = planes[1].Distance(v);
            const float d2 = planes[2].Distance(v);
            const float d3 = planes[3].Distance(v);

            const float t0 = d0 + radius;
            const float t1 = d1 + radius;
            const float t2 = d2 + radius;
            const float t3 = d3 + radius;

            const float s0 = d0 - radius;
            const float s1 = d1 - radius;
            const float s2 = d2 - radius;
            const float s3 = d3 - radius;

            byte bits;
            bits = IEEE_FLT_SIGNBITSET(t0) << 0;
            bits |= IEEE_FLT_SIGNBITSET(t1) << 1;
            bits |= IEEE_FLT_SIGNBITSET(t2) << 2;
            bits |= IEEE_FLT_SIGNBITSET(t3) << 3;

            bits |= IEEE_FLT_SIGNBITSET(s0) << 4;
            bits |= IEEE_FLT_SIGNBITSET(s1) << 5;
            bits |= IEEE_FLT_SIGNBITSET(s2) << 6;
            bits |= IEEE_FLT_SIGNBITSET(s3) << 7;

            bits ^= 0x0F; // flip lower four bits

            totalOr |= bits;
            gatheredBits[i] = bits;
        }
#endif
        for (int v = 0; v < sub.numVerts; v++) {
            cullBits[sub.verts[v]] = gatheredBits[v];
        }
        vertsBits += sub.numVerts;
    }
    VITA_ZONE_ADD("model trace: vertices given sidedness bits", vertsBits);

    int tris = 0;
    int exactTests = 0;
    int bestPos = INT_MAX;
    for (int s = 0; s < numSubsets; s++) {
        const vitaTraceSubset_t& sub = subsets[s];
        tris += sub.numTris;
        for (int t = 0; t < sub.numTris; t++) {
            const int i0 = sub.indexes[t * 3 + 0];
            const int i1 = sub.indexes[t * 3 + 1];
            const int i2 = sub.indexes[t * 3 + 2];

            // get sidedness info for the triangle
            const byte triOr = cullBits[i0] | cullBits[i1] | cullBits[i2];

            // if we don't have points on both sides of both the ray planes, no intersection
            if (likely((triOr ^ (triOr >> 4)) & 3)) {
                continue;
            }

            // if we don't have any points between front and end, no intersection
            if (unlikely((triOr ^ (triOr >> 1)) & 4)) {
                continue;
            }

            exactTests++;
            // an earlier triangle wins a tie: it may equal the best so far
            const int pos = sub.triPos[t];
            localTrace_t tri;
            tri.fraction = (bestPos != INT_MAX && pos < bestPos) ? nextafterf(hit.fraction, 2.0f) : hit.fraction;
            if (R_LineIntersectsTriangleExpandedWithCircle(tri, start, end, radius, xyz[i0], xyz[i1], xyz[i2])) {
                hit.fraction = tri.fraction;
                hit.point = tri.point;
                hit.normal = tri.normal;
                hit.indexes[0] = i0;
                hit.indexes[1] = i1;
                hit.indexes[2] = i2;
                bestPos = pos;
            }
        }
    }
    VITA_ZONE_ADD("model trace: triangles culled by sidedness", tris);
    VITA_ZONE_ADD("model trace: triangles given the exact (radius) test", exactTests);
    if (radius > 0.0f) {
        VITA_ZONE_COUNT("model trace: traces with a radius");
    }

    return hit;
}
#endif
