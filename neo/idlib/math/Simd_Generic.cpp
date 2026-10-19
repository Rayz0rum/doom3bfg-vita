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

#pragma hdrstop
#include "precompiled.h"
#include "Simd_Generic.h"
#if defined(__vita__) && defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

//===============================================================
//
//	Generic implementation of idSIMDProcessor
//
//===============================================================

#define UNROLL1(Y)                          \
    {                                       \
        int _IX;                            \
        for (_IX = 0; _IX < count; _IX++) { \
            Y(_IX);                         \
        }                                   \
    }
#define UNROLL2(Y)                           \
    {                                        \
        int _IX, _NM = count & 0xfffffffe;   \
        for (_IX = 0; _IX < _NM; _IX += 2) { \
            Y(_IX + 0);                      \
            Y(_IX + 1);                      \
        }                                    \
        if (_IX < count) {                   \
            Y(_IX);                          \
        }                                    \
    }
#define UNROLL4(Y)                           \
    {                                        \
        int _IX, _NM = count & 0xfffffffc;   \
        for (_IX = 0; _IX < _NM; _IX += 4) { \
            Y(_IX + 0);                      \
            Y(_IX + 1);                      \
            Y(_IX + 2);                      \
            Y(_IX + 3);                      \
        }                                    \
        for (; _IX < count; _IX++) {         \
            Y(_IX);                          \
        }                                    \
    }
#define UNROLL8(Y)                           \
    {                                        \
        int _IX, _NM = count & 0xfffffff8;   \
        for (_IX = 0; _IX < _NM; _IX += 8) { \
            Y(_IX + 0);                      \
            Y(_IX + 1);                      \
            Y(_IX + 2);                      \
            Y(_IX + 3);                      \
            Y(_IX + 4);                      \
            Y(_IX + 5);                      \
            Y(_IX + 6);                      \
            Y(_IX + 7);                      \
        }                                    \
        _NM = count & 0xfffffffe;            \
        for (; _IX < _NM; _IX += 2) {        \
            Y(_IX);                          \
            Y(_IX + 1);                      \
        }                                    \
        if (_IX < count) {                   \
            Y(_IX);                          \
        }                                    \
    }

#ifdef _DEBUG
#define NODEFAULT \
    default:      \
        assert(0)
#else
#define NODEFAULT \
    default:      \
        __assume(0)
#endif

/*
============
idSIMD_Generic::GetName
============
*/
const char* idSIMD_Generic::GetName() const
{
    return "generic code";
}

/*
============
idSIMD_Generic::MinMax
============
*/
void VPCALL idSIMD_Generic::MinMax(float& min, float& max, const float* src, const int count)
{
    min = idMath::INFINITY;
    max = -idMath::INFINITY;
#define OPER(X)           \
    if (src[(X)] < min) { \
        min = src[(X)];   \
    }                     \
    if (src[(X)] > max) { \
        max = src[(X)];   \
    }
    UNROLL1(OPER)
#undef OPER
}

/*
============
idSIMD_Generic::MinMax
============
*/
void VPCALL idSIMD_Generic::MinMax(idVec2& min, idVec2& max, const idVec2* src, const int count)
{
    min[0] = min[1] = idMath::INFINITY;
    max[0] = max[1] = -idMath::INFINITY;
#define OPER(X)                 \
    const idVec2& v = src[(X)]; \
    if (v[0] < min[0]) {        \
        min[0] = v[0];          \
    }                           \
    if (v[0] > max[0]) {        \
        max[0] = v[0];          \
    }                           \
    if (v[1] < min[1]) {        \
        min[1] = v[1];          \
    }                           \
    if (v[1] > max[1]) {        \
        max[1] = v[1];          \
    }
    UNROLL1(OPER)
#undef OPER
}

/*
============
idSIMD_Generic::MinMax
============
*/
void VPCALL idSIMD_Generic::MinMax(idVec3& min, idVec3& max, const idVec3* src, const int count)
{
    min[0] = min[1] = min[2] = idMath::INFINITY;
    max[0] = max[1] = max[2] = -idMath::INFINITY;
#define OPER(X)                 \
    const idVec3& v = src[(X)]; \
    if (v[0] < min[0]) {        \
        min[0] = v[0];          \
    }                           \
    if (v[0] > max[0]) {        \
        max[0] = v[0];          \
    }                           \
    if (v[1] < min[1]) {        \
        min[1] = v[1];          \
    }                           \
    if (v[1] > max[1]) {        \
        max[1] = v[1];          \
    }                           \
    if (v[2] < min[2]) {        \
        min[2] = v[2];          \
    }                           \
    if (v[2] > max[2]) {        \
        max[2] = v[2];          \
    }
    UNROLL1(OPER)
#undef OPER
}

/*
============
idSIMD_Generic::MinMax
============
*/
void VPCALL idSIMD_Generic::MinMax(idVec3& min, idVec3& max, const idDrawVert* src, const int count)
{
    min[0] = min[1] = min[2] = idMath::INFINITY;
    max[0] = max[1] = max[2] = -idMath::INFINITY;
#define OPER(X)                     \
    const idVec3& v = src[(X)].xyz; \
    if (v[0] < min[0]) {            \
        min[0] = v[0];              \
    }                               \
    if (v[0] > max[0]) {            \
        max[0] = v[0];              \
    }                               \
    if (v[1] < min[1]) {            \
        min[1] = v[1];              \
    }                               \
    if (v[1] > max[1]) {            \
        max[1] = v[1];              \
    }                               \
    if (v[2] < min[2]) {            \
        min[2] = v[2];              \
    }                               \
    if (v[2] > max[2]) {            \
        max[2] = v[2];              \
    }
    UNROLL1(OPER)
#undef OPER
}

/*
============
idSIMD_Generic::MinMax
============
*/
void VPCALL idSIMD_Generic::MinMax(idVec3& min, idVec3& max, const idDrawVert* src, const triIndex_t* indexes, const int count)
{
    min[0] = min[1] = min[2] = idMath::INFINITY;
    max[0] = max[1] = max[2] = -idMath::INFINITY;
#define OPER(X)                              \
    const idVec3& v = src[indexes[(X)]].xyz; \
    if (v[0] < min[0]) {                     \
        min[0] = v[0];                       \
    }                                        \
    if (v[0] > max[0]) {                     \
        max[0] = v[0];                       \
    }                                        \
    if (v[1] < min[1]) {                     \
        min[1] = v[1];                       \
    }                                        \
    if (v[1] > max[1]) {                     \
        max[1] = v[1];                       \
    }                                        \
    if (v[2] < min[2]) {                     \
        min[2] = v[2];                       \
    }                                        \
    if (v[2] > max[2]) {                     \
        max[2] = v[2];                       \
    }
    UNROLL1(OPER)
#undef OPER
}

/*
================
idSIMD_Generic::Memcpy
================
*/
void VPCALL idSIMD_Generic::Memcpy(void* dst, const void* src, const int count)
{
    memcpy(dst, src, count);
}

/*
================
idSIMD_Generic::Memset
================
*/
void VPCALL idSIMD_Generic::Memset(void* dst, const int val, const int count)
{
    memset(dst, val, count);
}

/*
============
idSIMD_Generic::BlendJoints
============
*/
void VPCALL idSIMD_Generic::BlendJoints(idJointQuat* joints, const idJointQuat* blendJoints, const float lerp, const int* index, const int numJoints)
{
#if defined(__vita__)
    // idQuat::Slerp calls libm's acos and sin three times per joint - every
    // animation blend of every animated entity, every game tic (5 ms a frame
    // in a fight). The same slerp as the PC's SSE path instead (its scalar
    // tail, idSIMD_SSE::BlendJoints): the polynomial ATan16 / Sin16.
    if (lerp <= 0.0f) {
        return;
    } else if (lerp >= 1.0f) {
        for (int i = 0; i < numJoints; i++) {
            const int j = index[i];
            joints[j] = blendJoints[j];
        }
        return;
    }
    for (int i = 0; i < numJoints; i++) {
        const int n = index[i];

        idVec3& jointVert = joints[n].t;
        const idVec3& blendVert = blendJoints[n].t;
        jointVert[0] += lerp * (blendVert[0] - jointVert[0]);
        jointVert[1] += lerp * (blendVert[1] - jointVert[1]);
        jointVert[2] += lerp * (blendVert[2] - jointVert[2]);
        joints[n].w = 0.0f;

        idQuat& jointQuat = joints[n].q;
        const idQuat& blendQuat = blendJoints[n].q;

        float cosom = jointQuat.x * blendQuat.x + jointQuat.y * blendQuat.y + jointQuat.z * blendQuat.z + jointQuat.w * blendQuat.w;
        const bool negative = cosom < 0.0f;
        if (negative) {
            cosom = -cosom;
        }
        float scale0 = 1.0f - cosom * cosom;
        scale0 = (scale0 <= 0.0f) ? 1e-10f : scale0;
        const float sinom = idMath::InvSqrt(scale0);
        const float omega = idMath::ATan16(scale0 * sinom, cosom);
        scale0 = idMath::Sin16((1.0f - lerp) * omega) * sinom;
        float scale1 = idMath::Sin16(lerp * omega) * sinom;
        if (negative) {
            scale1 = -scale1;
        }

        jointQuat.x = scale0 * jointQuat.x + scale1 * blendQuat.x;
        jointQuat.y = scale0 * jointQuat.y + scale1 * blendQuat.y;
        jointQuat.z = scale0 * jointQuat.z + scale1 * blendQuat.z;
        jointQuat.w = scale0 * jointQuat.w + scale1 * blendQuat.w;
    }
#else
    for (int i = 0; i < numJoints; i++) {
        int j = index[i];
        joints[j].q.Slerp(joints[j].q, blendJoints[j].q, lerp);
        joints[j].t.Lerp(joints[j].t, blendJoints[j].t, lerp);
        joints[j].w = 0.0f;
    }
#endif
}

/*
============
idSIMD_Generic::BlendJointsFast
============
*/
void VPCALL idSIMD_Generic::BlendJointsFast(idJointQuat* joints, const idJointQuat* blendJoints, const float lerp, const int* index, const int numJoints)
{
    for (int i = 0; i < numJoints; i++) {
        int j = index[i];
        joints[j].q.Lerp(joints[j].q, blendJoints[j].q, lerp);
        joints[j].t.Lerp(joints[j].t, blendJoints[j].t, lerp);
        joints[j].w = 0.0f;
    }
}

/*
============
idSIMD_Generic::ConvertJointQuatsToJointMats
============
*/
void VPCALL idSIMD_Generic::ConvertJointQuatsToJointMats(idJointMat* jointMats, const idJointQuat* jointQuats, const int numJoints)
{
    for (int i = 0; i < numJoints; i++) {
        jointMats[i].SetRotation(jointQuats[i].q.ToMat3());
        jointMats[i].SetTranslation(jointQuats[i].t);
    }
}

/*
============
idSIMD_Generic::ConvertJointMatsToJointQuats
============
*/
void VPCALL idSIMD_Generic::ConvertJointMatsToJointQuats(idJointQuat* jointQuats, const idJointMat* jointMats, const int numJoints)
{
    for (int i = 0; i < numJoints; i++) {
        jointQuats[i] = jointMats[i].ToJointQuat();
    }
}

/*
============
idSIMD_Generic::TransformJoints
============
*/
void VPCALL idSIMD_Generic::TransformJoints(idJointMat* jointMats, const int* parents, const int firstJoint, const int lastJoint)
{
#if defined(__vita__) && defined(__ARM_NEON__)
    // Every animated entity's skeleton, every frame it is built (idAnimator::
    // CreateFrame, ~140 times a frame in a fight). idJointMat::operator*= in
    // NEON, a row at a time: lane c of new row r is m[0][c] * a[r][0] +
    // m[1][c] * a[r][1] + m[2][c] * a[r][2], in that order, as the scalar
    // code sums it (VMLA rounds the product before the add, as it does), and
    // the translation lane then gets a[r][3]; the other lanes get -0, which
    // changes nothing, signed zeros included (tools/vita/neontest).
    for (int i = firstJoint; i <= lastJoint; i++) {
        assert(parents[i] < i);
        float* m = jointMats[i].ToFloatPtr();
        const float* a = jointMats[parents[i]].ToFloatPtr();
        const float32x4_t m0 = vld1q_f32(m + 0);
        const float32x4_t m1 = vld1q_f32(m + 4);
        const float32x4_t m2 = vld1q_f32(m + 8);
        const float32x4_t negZero = vdupq_n_f32(-0.0f);
        // the parent's rows loaded as vectors and multiplied by lane (as float
        // scalars, GCC built them through the stack every joint); the
        // translation is the parent row's lane 3 over -0 elsewhere, selected
        // bit for bit. The same arithmetic.
        const uint32x4_t lane3 = vcombine_u32(vdup_n_u32(0), vcreate_u32(0xFFFFFFFF00000000ULL));
        for (int r = 0; r < 3; r++) {
            const float32x4_t ar = vld1q_f32(a + r * 4);
            const float32x2_t a01 = vget_low_f32(ar);
            const float32x2_t a23 = vget_high_f32(ar);
            float32x4_t row = vmulq_lane_f32(m0, a01, 0);
            row = vmlaq_lane_f32(row, m1, a01, 1);
            row = vmlaq_lane_f32(row, m2, a23, 0);
            row = vaddq_f32(row, vbslq_f32(lane3, ar, negZero));
            vst1q_f32(m + r * 4, row);
        }
    }
    return;
#endif
    for (int i = firstJoint; i <= lastJoint; i++) {
        assert(parents[i] < i);
        jointMats[i] *= jointMats[parents[i]];
    }
}

/*
============
idSIMD_Generic::UntransformJoints
============
*/
void VPCALL idSIMD_Generic::UntransformJoints(idJointMat* jointMats, const int* parents, const int firstJoint, const int lastJoint)
{
    for (int i = lastJoint; i >= firstJoint; i--) {
        assert(parents[i] < i);
        jointMats[i] /= jointMats[parents[i]];
    }
}
