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
#include "../sys/vita/vita_zones.h" // VITA_ZONE_COUNT / VITA_ZONE_ADD: the model trace counts in the perf log

#if defined(USE_INTRINSICS)
static const __m128 vector_float_posInfinity = { idMath::INFINITY, idMath::INFINITY, idMath::INFINITY, idMath::INFINITY };
static const __m128 vector_float_negInfinity = { -idMath::INFINITY, -idMath::INFINITY, -idMath::INFINITY, -idMath::INFINITY };
#endif

static const char* MD5_SnapshotName = "_MD5_Snapshot_";

static const byte MD5B_VERSION = 106;
static const unsigned int MD5B_MAGIC = ('5' << 24) | ('D' << 16) | ('M' << 8) | MD5B_VERSION;

idCVar r_useGPUSkinning("r_useGPUSkinning", "1", CVAR_INTEGER, "animate normals and tangents instead of deriving");

/***********************************************************************

        idMD5Mesh

***********************************************************************/

static int c_numVerts = 0;
static int c_numWeights = 0;
static int c_numWeightJoints = 0;

struct vertexWeight_t {
    int joint;
    idVec3 offset;
    float jointWeight;
};

/*
====================
idMD5Mesh::idMD5Mesh
====================
*/
idMD5Mesh::idMD5Mesh()
{
    shader = NULL;
    numVerts = 0;
    numTris = 0;
    meshJoints = NULL;
    numMeshJoints = 0;
    maxJointVertDist = 0.0f;
    deformInfo = NULL;
    surfaceNum = 0;
#if defined(__vita__)
    vitaUnitNT = NULL;
#endif
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    vitaWeightSumMax = -1.0f;
    vitaGroupsOf = NULL;
    vitaNumGroups = 0;
    vitaGroups = NULL;
    vitaGroupJoints = NULL;
    vitaGroupVerts = NULL;
    vitaGroupIndexes = NULL;
    vitaGroupTriPos = NULL;
#endif
}

/*
====================
idMD5Mesh::~idMD5Mesh
====================
*/
idMD5Mesh::~idMD5Mesh()
{
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    VitaFreeTraceGroups();
#endif
#if defined(__vita__)
    if (vitaUnitNT != NULL) {
        Mem_Free16(vitaUnitNT);
        vitaUnitNT = NULL;
    }
#endif
    if (meshJoints != NULL) {
        Mem_Free(meshJoints);
        meshJoints = NULL;
    }
    if (deformInfo != NULL) {
        R_FreeDeformInfo(deformInfo);
        deformInfo = NULL;
    }
}

/*
====================
idMD5Mesh::ParseMesh
====================
*/
void idMD5Mesh::ParseMesh(idLexer& parser, int numJoints, const idJointMat* joints)
{
    idToken token;
    idToken name;

    parser.ExpectTokenString("{");

    //
    // parse name
    //
    if (parser.CheckTokenString("name")) {
        parser.ReadToken(&name);
    }

    //
    // parse shader
    //
    parser.ExpectTokenString("shader");

    parser.ReadToken(&token);
    idStr shaderName = token;

    shader = declManager->FindMaterial(shaderName);

    //
    // parse texture coordinates
    //
    parser.ExpectTokenString("numverts");
    int count = parser.ParseInt();
    if (count < 0) {
        parser.Error("Invalid size: %s", token.c_str());
    }

    this->numVerts = count;

    idList<idVec2> texCoords;
    idList<int> firstWeightForVertex;
    idList<int> numWeightsForVertex;

    texCoords.SetNum(count);
    firstWeightForVertex.SetNum(count);
    numWeightsForVertex.SetNum(count);

    int numWeights = 0;
    int maxweight = 0;
    for (int i = 0; i < texCoords.Num(); i++) {
        parser.ExpectTokenString("vert");
        parser.ParseInt();

        parser.Parse1DMatrix(2, texCoords[i].ToFloatPtr());

        firstWeightForVertex[i] = parser.ParseInt();
        numWeightsForVertex[i] = parser.ParseInt();

        if (!numWeightsForVertex[i]) {
            parser.Error("Vertex without any joint weights.");
        }

        numWeights += numWeightsForVertex[i];
        if (numWeightsForVertex[i] + firstWeightForVertex[i] > maxweight) {
            maxweight = numWeightsForVertex[i] + firstWeightForVertex[i];
        }
    }

    //
    // parse tris
    //
    parser.ExpectTokenString("numtris");
    count = parser.ParseInt();
    if (count < 0) {
        parser.Error("Invalid size: %d", count);
    }

    idList<int> tris;
    tris.SetNum(count * 3);
    numTris = count;
    for (int i = 0; i < count; i++) {
        parser.ExpectTokenString("tri");
        parser.ParseInt();

        tris[i * 3 + 0] = parser.ParseInt();
        tris[i * 3 + 1] = parser.ParseInt();
        tris[i * 3 + 2] = parser.ParseInt();
    }

    //
    // parse weights
    //
    parser.ExpectTokenString("numweights");
    count = parser.ParseInt();
    if (count < 0) {
        parser.Error("Invalid size: %d", count);
    }

    if (maxweight > count) {
        parser.Warning("Vertices reference out of range weights in model (%d of %d weights).", maxweight, count);
    }

    idList<vertexWeight_t> tempWeights;
    tempWeights.SetNum(count);
    assert(numJoints < 256); // so we can pack into bytes

    for (int i = 0; i < count; i++) {
        parser.ExpectTokenString("weight");
        parser.ParseInt();

        int jointnum = parser.ParseInt();
        if ((jointnum < 0) || (jointnum >= numJoints)) {
            parser.Error("Joint Index out of range(%d): %d", numJoints, jointnum);
        }

        tempWeights[i].joint = jointnum;
        tempWeights[i].jointWeight = parser.ParseFloat();

        parser.Parse1DMatrix(3, tempWeights[i].offset.ToFloatPtr());
    }

    // create pre-scaled weights and an index for the vertex/joint lookup
    idVec4* scaledWeights = (idVec4*)Mem_Alloc16(numWeights * sizeof(scaledWeights[0]), TAG_MD5_WEIGHT);
    int* weightIndex = (int*)Mem_Alloc16(numWeights * 2 * sizeof(weightIndex[0]), TAG_MD5_INDEX);
    memset(weightIndex, 0, numWeights * 2 * sizeof(weightIndex[0]));

    count = 0;
    for (int i = 0; i < texCoords.Num(); i++) {
        int num = firstWeightForVertex[i];
        for (int j = 0; j < numWeightsForVertex[i]; j++, num++, count++) {
            scaledWeights[count].ToVec3() = tempWeights[num].offset * tempWeights[num].jointWeight;
            scaledWeights[count].w = tempWeights[num].jointWeight;
            weightIndex[count * 2 + 0] = tempWeights[num].joint * sizeof(idJointMat);
        }
        weightIndex[count * 2 - 1] = 1;
    }

    parser.ExpectTokenString("}");

    // update counters
    c_numVerts += texCoords.Num();
    c_numWeights += numWeights;
    c_numWeightJoints++;
    for (int i = 0; i < numWeights; i++) {
        c_numWeightJoints += weightIndex[i * 2 + 1];
    }

    //
    // build a base pose that can be used for skinning
    //
    idDrawVert* basePose = (idDrawVert*)Mem_ClearedAlloc(texCoords.Num() * sizeof(*basePose), TAG_MD5_BASE);
    for (int j = 0, i = 0; i < texCoords.Num(); i++) {
        idVec3 v = (*(idJointMat*)((byte*)joints + weightIndex[j * 2 + 0])) * scaledWeights[j];
        while (weightIndex[j * 2 + 1] == 0) {
            j++;
            v += (*(idJointMat*)((byte*)joints + weightIndex[j * 2 + 0])) * scaledWeights[j];
        }
        j++;

        basePose[i].Clear();
        basePose[i].xyz = v;
        basePose[i].SetTexCoord(texCoords[i]);
    }

    // build the weights and bone indexes into the verts, so they will be duplicated
    // as necessary at mirror seems

    static int maxWeightsPerVert;
    static float maxResidualWeight;

    const int MAX_VERTEX_WEIGHTS = 4;

    idList<bool> jointIsUsed;
    jointIsUsed.SetNum(numJoints);
    for (int i = 0; i < jointIsUsed.Num(); i++) {
        jointIsUsed[i] = false;
    }

    numMeshJoints = 0;
    maxJointVertDist = 0.0f;

    //-----------------------------------------
    // new-style setup for fixed four weights and normal / tangent deformation
    //
    // Several important models have >25% residual weight in joints after the
    // first four, which is worrisome for using a fixed four joint deformation.
    //-----------------------------------------
    for (int i = 0; i < texCoords.Num(); i++) {
        idDrawVert& dv = basePose[i];

        // some models do have >4 joint weights, so it is necessary to sort and renormalize

        // sort the weights and take the four largest
        int weights[256];
        const int numWeights = numWeightsForVertex[i];
        for (int j = 0; j < numWeights; j++) {
            weights[j] = firstWeightForVertex[i] + j;
        }
        // bubble sort
        for (int j = 0; j < numWeights; j++) {
            for (int k = 0; k < numWeights - 1 - j; k++) {
                if (tempWeights[weights[k]].jointWeight < tempWeights[weights[k + 1]].jointWeight) {
                    SwapValues(weights[k], weights[k + 1]);
                }
            }
        }

        if (numWeights > maxWeightsPerVert) {
            maxWeightsPerVert = numWeights;
        }

        const int usedWeights = Min(MAX_VERTEX_WEIGHTS, numWeights);

        float totalWeight = 0;
        for (int j = 0; j < numWeights; j++) {
            totalWeight += tempWeights[weights[j]].jointWeight;
        }
        assert(totalWeight > 0.999f && totalWeight < 1.001f);

        float usedWeight = 0;
        for (int j = 0; j < usedWeights; j++) {
            usedWeight += tempWeights[weights[j]].jointWeight;
        }

        const float residualWeight = totalWeight - usedWeight;
        if (residualWeight > maxResidualWeight) {
            maxResidualWeight = residualWeight;
        }

        byte finalWeights[MAX_VERTEX_WEIGHTS] = { 0 };
        byte finalJointIndecies[MAX_VERTEX_WEIGHTS] = { 0 };
        for (int j = 0; j < usedWeights; j++) {
            const vertexWeight_t& weight = tempWeights[weights[j]];
            const int jointIndex = weight.joint;
            const float fw = weight.jointWeight;
            assert(fw >= 0.0f && fw <= 1.0f);
            const float normalizedWeight = fw / usedWeight;
            finalWeights[j] = idMath::Ftob(normalizedWeight * 255.0f);
            finalJointIndecies[j] = jointIndex;
        }

        // Sort the weights and indices for hardware skinning
        for (int k = 0; k < 3; ++k) {
            for (int l = k + 1; l < 4; ++l) {
                if (finalWeights[l] > finalWeights[k]) {
                    SwapValues(finalWeights[k], finalWeights[l]);
                    SwapValues(finalJointIndecies[k], finalJointIndecies[l]);
                }
            }
        }

        // Give any left over to the biggest weight
        finalWeights[0] += Max(255 - finalWeights[0] - finalWeights[1] - finalWeights[2] - finalWeights[3], 0);

        dv.color[0] = finalJointIndecies[0];
        dv.color[1] = finalJointIndecies[1];
        dv.color[2] = finalJointIndecies[2];
        dv.color[3] = finalJointIndecies[3];

        dv.color2[0] = finalWeights[0];
        dv.color2[1] = finalWeights[1];
        dv.color2[2] = finalWeights[2];
        dv.color2[3] = finalWeights[3];

        for (int j = usedWeights; j < 4; j++) {
            assert(dv.color2[j] == 0);
        }

        for (int j = 0; j < usedWeights; j++) {
            if (!jointIsUsed[finalJointIndecies[j]]) {
                jointIsUsed[finalJointIndecies[j]] = true;
                numMeshJoints++;
            }
            const idJointMat& joint = joints[finalJointIndecies[j]];
            float dist = (dv.xyz - joint.GetTranslation()).Length();
            if (dist > maxJointVertDist) {
                maxJointVertDist = dist;
            }
        }
    }

    meshJoints = (byte*)Mem_Alloc(numMeshJoints * sizeof(meshJoints[0]), TAG_MODEL);
    numMeshJoints = 0;
    for (int i = 0; i < numJoints; i++) {
        if (jointIsUsed[i]) {
            meshJoints[numMeshJoints++] = i;
        }
    }

    // build the deformInfo and collect a final base pose with the mirror
    // seam verts properly including the bone weights
    deformInfo = R_BuildDeformInfo(texCoords.Num(), basePose, tris.Num(), tris.Ptr(),
        shader->UseUnsmoothedTangents());

    for (int i = 0; i < deformInfo->numOutputVerts; i++) {
        for (int j = 0; j < 4; j++) {
            if (deformInfo->verts[i].color[j] >= numJoints) {
                idLib::FatalError("Bad joint index");
            }
        }
    }

    Mem_Free(basePose);
}

#if defined(__vita__) && defined(__ARM_NEON__)
#include <arm_neon.h>
/*
============
Vita_SkinColumns

The blended joint matrix of a vertex - its four weighted joint matrices
summed row by row - turned into columns (c3 is the translation). Shared by the
full skin and the positions-only one, so both give bit-identical positions.
The weights come from one 8-byte load (color holds the joint indexes, color2
right after it the weights) and one vector conversion.
============
*/
static ID_FORCE_INLINE void Vita_SkinColumns(const idDrawVert& base, const idJointMat* joints, const float32x4_t inv255,
    float32x4_t& c0, float32x4_t& c1, float32x4_t& c2, float32x4_t& c3)
{
    const float* j0 = joints[base.color[0]].ToFloatPtr();
    const float* j1 = joints[base.color[1]].ToFloatPtr();
    const float* j2 = joints[base.color[2]].ToFloatPtr();
    const float* j3 = joints[base.color[3]].ToFloatPtr();
    const uint16x8_t indexesAndWeights = vmovl_u8(vld1_u8(base.color));
    const float32x4_t w = vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(indexesAndWeights))), inv255);
    const float32x2_t w01 = vget_low_f32(w);
    const float32x2_t w23 = vget_high_f32(w);
    // the three rows side by side, so the multiply-adds do not wait on each other
    float32x4_t r0 = vmulq_lane_f32(vld1q_f32(j0 + 0), w01, 0);
    float32x4_t r1 = vmulq_lane_f32(vld1q_f32(j0 + 4), w01, 0);
    float32x4_t r2 = vmulq_lane_f32(vld1q_f32(j0 + 8), w01, 0);
    r0 = vmlaq_lane_f32(r0, vld1q_f32(j1 + 0), w01, 1);
    r1 = vmlaq_lane_f32(r1, vld1q_f32(j1 + 4), w01, 1);
    r2 = vmlaq_lane_f32(r2, vld1q_f32(j1 + 8), w01, 1);
    r0 = vmlaq_lane_f32(r0, vld1q_f32(j2 + 0), w23, 0);
    r1 = vmlaq_lane_f32(r1, vld1q_f32(j2 + 4), w23, 0);
    r2 = vmlaq_lane_f32(r2, vld1q_f32(j2 + 8), w23, 0);
    r0 = vmlaq_lane_f32(r0, vld1q_f32(j3 + 0), w23, 1);
    r1 = vmlaq_lane_f32(r1, vld1q_f32(j3 + 4), w23, 1);
    r2 = vmlaq_lane_f32(r2, vld1q_f32(j3 + 8), w23, 1);
    // rows -> columns (the fourth row is zero)
    const float32x4x2_t t01 = vtrnq_f32(r0, r1);
    const float32x4x2_t t2z = vtrnq_f32(r2, vdupq_n_f32(0.0f));
    c0 = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t2z.val[0]));
    c1 = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t2z.val[1]));
    c2 = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t2z.val[0]));
    c3 = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t2z.val[1]));
}

static ID_FORCE_INLINE float32x4_t Vita_SkinPosition(const idDrawVert& base, const float32x4_t c0, const float32x4_t c1, const float32x4_t c2, const float32x4_t c3)
{
    // x, y, z in one load (the fourth lane is the texture coordinates, unused)
    // and multiplied by lane: with the three floats passed as scalars, GCC
    // built each one's vector through the stack, lane by lane, every vertex.
    // The same multiply-adds by scalar, so the same results.
    const float32x4_t xyz = vld1q_f32(&base.xyz.x);
    const float32x2_t xy = vget_low_f32(xyz);
    const float32x2_t zs = vget_high_f32(xyz);
    return vmlaq_lane_f32(vmlaq_lane_f32(vmlaq_lane_f32(c3, c0, xy, 0), c1, xy, 1), c2, zs, 0);
}

/*
============
Vita_UnitNormalTangent

A base vertex's normal and tangent (bytes: normal[4], then tangent[4]) as the
unit vectors the skin rotates - unpacked and renormalized. They depend on the
base vertex alone, so Vita_UnitNormalsTangents_NEON can make them once per
mesh (idMD5Mesh::VitaUnitNormals) with this very code.
============
*/
static ID_FORCE_INLINE void Vita_UnitNormalTangent(const uint8x8_t bytes, const float32x4_t byteScale, const float32x4_t minusOne, float32x4_t& nn, float32x4_t& nt)
{
    // unpack (b * 2/255 - 1), the fourth lanes are ignored
    const uint16x8_t bytes16 = vmovl_u8(bytes);
    const float32x4_t fn = vmlaq_f32(minusOne, vcvtq_f32_u32(vmovl_u16(vget_low_u16(bytes16))), byteScale);
    const float32x4_t ft = vmlaq_f32(minusOne, vcvtq_f32_u32(vmovl_u16(vget_high_u16(bytes16))), byteScale);
    // renormalize: lane 0 the normal, lane 1 the tangent, each x2 + y2 + z2
    const float32x4_t sqn = vmulq_f32(fn, fn);
    const float32x4_t sqt = vmulq_f32(ft, ft);
    const float32x2x2_t zz = vtrn_f32(vget_high_f32(sqn), vget_high_f32(sqt));
    const float32x2_t len2 = vadd_f32(vpadd_f32(vget_low_f32(sqn), vget_low_f32(sqt)), zz.val[0]);
    float32x2_t inv = vrsqrte_f32(len2);
    inv = vmul_f32(inv, vrsqrts_f32(vmul_f32(len2, inv), inv));
    inv = vmul_f32(inv, vrsqrts_f32(vmul_f32(len2, inv), inv));
    nn = vmulq_lane_f32(fn, inv, 0);
    nt = vmulq_lane_f32(ft, inv, 1);
}

/*
============
Vita_SkinVertexUnit / Vita_SkinVertex

One vertex of Vita_TransformVertsAndTangents_NEON (see there), from its unit
normal and tangent (nn, nt) - given, or made here.
============
*/
static ID_FORCE_INLINE void Vita_SkinVertexUnit(const idDrawVert& base, idDrawVert& out, const uint8x8_t bytes, const float32x4_t nn, const float32x4_t nt, byte* outNormal,
    const idJointMat* joints, const float32x4_t one, const float32x4_t half, const float32x4_t inv255, const uint32x4_t maxByte, const uint8x8_t keepFourth, const float32x4_t halfByteRange)
{
    float32x4_t c0, c1, c2, c3;
    Vita_SkinColumns(base, joints, inv255, c0, c1, c2, c3);

    const float32x4_t pos = Vita_SkinPosition(base, c0, c1, c2, c3);
    vst1_f32(&out.xyz.x, vget_low_f32(pos));
    vst1q_lane_f32(&out.xyz.z, pos, 2);

    // rotate
    float32x4_t rn = vmulq_lane_f32(c0, vget_low_f32(nn), 0);
    float32x4_t rt = vmulq_lane_f32(c0, vget_low_f32(nt), 0);
    rn = vmlaq_lane_f32(rn, c1, vget_low_f32(nn), 1);
    rt = vmlaq_lane_f32(rt, c1, vget_low_f32(nt), 1);
    rn = vmlaq_lane_f32(rn, c2, vget_high_f32(nn), 0);
    rt = vmlaq_lane_f32(rt, c2, vget_high_f32(nt), 0);
    // pack: Ftob( ( x + 1 ) * 127.5 + 0.5 ), truncating, clamped to 0..255
    // (127.5 as a vector: the scalar form moved it from an ARM register into a lane twice a vertex)
    const uint32x4_t un = vminq_u32(vcvtq_u32_f32(vmlaq_f32(half, vaddq_f32(rn, one), halfByteRange)), maxByte);
    const uint32x4_t ut = vminq_u32(vcvtq_u32_f32(vmlaq_f32(half, vaddq_f32(rt, one), halfByteRange)), maxByte);
    const uint8x8_t packed = vmovn_u16(vcombine_u16(vmovn_u32(un), vmovn_u32(ut)));
    vst1_u8(outNormal, vbsl_u8(keepFourth, bytes, packed));
}

static ID_FORCE_INLINE void Vita_SkinVertex(const idDrawVert& base, idDrawVert& out, const byte* baseNormal, byte* outNormal, const idJointMat* joints, const float32x4_t one, const float32x4_t half,
    const float32x4_t byteScale, const float32x4_t minusOne, const float32x4_t inv255, const uint32x4_t maxByte, const uint8x8_t keepFourth, const float32x4_t halfByteRange)
{
    const uint8x8_t bytes = vld1_u8(baseNormal); // normal[4], then tangent[4]
    float32x4_t nn, nt;
    Vita_UnitNormalTangent(bytes, byteScale, minusOne, nn, nt);
    Vita_SkinVertexUnit(base, out, bytes, nn, nt, outNormal, joints, one, half, inv255, maxByte, keepFourth, halfByteRange);
}

/*
============
Vita_TransformVertsAndTangents_NEON

GPU skinning is off on the Vita (no uniform blocks in GLSL ES 1.00), so every
animated mesh is skinned here, on the CPU, every frame it is seen - the
biggest part of the front end's "models" time. The same work in NEON: the
blended joint matrix (Vita_SkinColumns) applied to the position, normal and
tangent (normal and tangent unpacked from bytes, renormalized - reciprocal
square root estimate plus two Newton steps - and packed back with the same
rounding and clamping as VERTEX_FLOAT_TO_BYTE).

Normal and tangent go through together: one 8-byte load, both lengths in one
2-lane reciprocal square root, one 8-byte store. Moving a value from a NEON
register to an ARM one stalls the Cortex-A9 until the NEON pipeline catches
up, and the first version paid that six times a vertex, storing the bytes one
by one; this one never leaves NEON. The arithmetic is unchanged, lane for lane.
============
*/
void Vita_TransformVertsAndTangents_NEON(idDrawVert* __restrict targetVerts, const int numVerts, const idDrawVert* __restrict baseVerts, const idJointMat* __restrict joints)
{
    const float32x4_t one = vdupq_n_f32(1.0f);
    const float32x4_t half = vdupq_n_f32(0.5f);
    const float32x4_t byteScale = vdupq_n_f32(2.0f / 255.0f);
    const float32x4_t minusOne = vdupq_n_f32(-1.0f);
    const float32x4_t inv255 = vdupq_n_f32(1.0f / 255.0f);
    const uint32x4_t maxByte = vdupq_n_u32(255);
    // the fourth byte of each: normal[3] stays, tangent[3] (the bitangent sign) is copied
    const uint8x8_t keepFourth = vcreate_u8(0xFF000000FF000000ULL);

    // Run 22 timed this loop at ~400 cycles a vertex for ~60 instructions.
    // Two vertices an iteration (run 23) did not help: the disassembly showed
    // GCC building the x, y, z scalar operands through the stack lane by lane
    // and moving 127.5 from an ARM register into lanes every vertex, and two
    // vertices' worth of values spilling - fixed in Vita_SkinPosition and with
    // a vector constant, one vertex an iteration again. The data 8 vertices
    // ahead is asked for early (source read, output written). Same
    // operations on every vertex, so the same bits.
    const float32x4_t halfByteRange = vdupq_n_f32(255.0f / 2.0f);
    for (int i = 0; i < numVerts; i++) {
        __builtin_prefetch(&baseVerts[i + 8], 0);
        __builtin_prefetch(&targetVerts[i + 8], 1);
        Vita_SkinVertex(baseVerts[i], targetVerts[i], baseVerts[i].normal, targetVerts[i].normal, joints, one, half, byteScale, minusOne, inv255, maxByte, keepFourth, halfByteRange);
    }
}

/*
============
Vita_UnitNormalsTangents_NEON / Vita_TransformVertsAndTangentsUnit_NEON

The unit normal and tangent of every base vertex, made once (8 floats a
vertex: normal, then tangent, each in a 4-lane vector), and the skin that
reads them instead of unpacking and renormalizing every vertex every frame -
a fifth of the skin's instructions, on its longest chain. The same vectors
from the same code, so the same bytes (tools/vita/neontest).
============
*/
void Vita_UnitNormalsTangents_NEON(float* __restrict unit, const int numVerts, const idDrawVert* __restrict baseVerts)
{
    const float32x4_t byteScale = vdupq_n_f32(2.0f / 255.0f);
    const float32x4_t minusOne = vdupq_n_f32(-1.0f);
    for (int i = 0; i < numVerts; i++) {
        float32x4_t nn, nt;
        Vita_UnitNormalTangent(vld1_u8(baseVerts[i].normal), byteScale, minusOne, nn, nt);
        vst1q_f32(unit + i * 8 + 0, nn);
        vst1q_f32(unit + i * 8 + 4, nt);
    }
}

void Vita_TransformVertsAndTangentsUnit_NEON(idDrawVert* __restrict targetVerts, const int numVerts, const idDrawVert* __restrict baseVerts, const float* __restrict unit, const idJointMat* __restrict joints)
{
    const float32x4_t one = vdupq_n_f32(1.0f);
    const float32x4_t half = vdupq_n_f32(0.5f);
    const float32x4_t inv255 = vdupq_n_f32(1.0f / 255.0f);
    const uint32x4_t maxByte = vdupq_n_u32(255);
    const uint8x8_t keepFourth = vcreate_u8(0xFF000000FF000000ULL);
    const float32x4_t halfByteRange = vdupq_n_f32(255.0f / 2.0f);
    for (int i = 0; i < numVerts; i++) {
        __builtin_prefetch(&baseVerts[i + 8], 0);
        __builtin_prefetch(&targetVerts[i + 8], 1);
        __builtin_prefetch(unit + (i + 8) * 8, 0);
        const uint8x8_t bytes = vld1_u8(baseVerts[i].normal);
        Vita_SkinVertexUnit(baseVerts[i], targetVerts[i], bytes, vld1q_f32(unit + i * 8 + 0), vld1q_f32(unit + i * 8 + 4), targetVerts[i].normal,
            joints, one, half, inv255, maxByte, keepFourth, halfByteRange);
    }
}

/*
============
Vita_TransformPositions_NEON

The positions alone, for render model traces (Vita_PoseForTrace) - the same
arithmetic as the full skin's, so a trace sees exactly the positions the
model is drawn with.
============
*/
static void Vita_TransformPositions_NEON(idVec3* targetXyz, const int numVerts, const idDrawVert* baseVerts, const idJointMat* joints)
{
    const float32x4_t inv255 = vdupq_n_f32(1.0f / 255.0f);
    for (int i = 0; i < numVerts; i++) {
        const idDrawVert& base = baseVerts[i];
        float32x4_t c0, c1, c2, c3;
        Vita_SkinColumns(base, joints, inv255, c0, c1, c2, c3);
        const float32x4_t pos = Vita_SkinPosition(base, c0, c1, c2, c3);
        vst1_f32(&targetXyz[i].x, vget_low_f32(pos));
        vst1q_lane_f32(&targetXyz[i].z, pos, 2);
    }
}
#endif

#if !defined(__vita__) && defined(ID_VITA_PHYSCHECK)
/*
============
Vita_AnimMeasure (PC test build, vita_animLODMeasure 1, with r_useGPUSkinning 0)

What a monster held at 15 Hz shows against its true pose, and what a linear
extrapolation would: every time a living monster's mesh is skinned (once per
game tic it changes), its positions go into a 4-pose history; with poses N
(now) and N-1, N-3 (the two a 15 Hz monster was last skinned at), a held
frame shows N-1 (error |N - (N-1)|) and an extrapolated one would show
N-1 + 0.5 (N-1 - N-3). Summed per vertex in 1/10 units.
============
*/
idCVar vita_animLODMeasure("vita_animLODMeasure", "0", CVAR_RENDERER | CVAR_BOOL, "PC test build: monster pose error of the 15 Hz hold vs a linear extrapolation (needs r_useGPUSkinning 0)");
struct vitaAnimHistory_t {
    const srfTriangles_t* tri;
    int numVerts;
    int count;
    int lastUse;
    int head;               // pose[head] newest, pose[(head + k) & 3] k poses older
    idList<idVec3> pose[4];
};
static vitaAnimHistory_t vitaAnimHistory[48];
static int vitaAnimHistoryUse = 0;
static idSysMutex vitaAnimHistoryLock;

static void Vita_AnimMeasure(const srfTriangles_t* tri)
{
    idScopedCriticalSection lock(vitaAnimHistoryLock);
    vitaAnimHistoryUse++;
    vitaAnimHistory_t* h = NULL;
    for (int i = 0; i < 48 && h == NULL; i++) {
        if (vitaAnimHistory[i].tri == tri && vitaAnimHistory[i].numVerts == tri->numVerts) {
            h = &vitaAnimHistory[i];
        }
    }
    if (h == NULL) {
        h = &vitaAnimHistory[0];
        for (int i = 1; i < 48; i++) {
            if (vitaAnimHistory[i].lastUse < h->lastUse) {
                h = &vitaAnimHistory[i];
            }
        }
        h->tri = tri;
        h->numVerts = tri->numVerts;
        h->count = 0;
    }
    h->lastUse = vitaAnimHistoryUse;
    // the oldest slot becomes the newest pose
    h->head = (h->head + 3) & 3;
    idList<idVec3>& newest = h->pose[h->head];
    newest.SetNum(tri->numVerts);
    for (int v = 0; v < tri->numVerts; v++) {
        newest[v] = tri->verts[v].xyz;
    }
    h->count++;
    if (h->count < 4) {
        return;
    }
    double holdErr = 0.0, extrapErr = 0.0, holdMax = 0.0, extrapMax = 0.0;
    for (int v = 0; v < tri->numVerts; v++) {
        const idVec3& now = h->pose[h->head][v];
        const idVec3& p1 = h->pose[(h->head + 1) & 3][v];
        const idVec3& p3 = h->pose[(h->head + 3) & 3][v];
        const float eh = (now - p1).Length();
        const float ee = (now - (p1 + 0.5f * (p1 - p3))).Length();
        holdErr += eh;
        extrapErr += ee;
        holdMax = Max(holdMax, (double)eh);
        extrapMax = Max(extrapMax, (double)ee);
    }
    VITA_ZONE_ADD("anim measure: vertices compared", tri->numVerts);
    VITA_ZONE_ADD("anim measure: held pose error, summed (1/10 units)", (int)(holdErr * 10.0));
    VITA_ZONE_ADD("anim measure: extrapolated pose error, summed (1/10 units)", (int)(extrapErr * 10.0));
    VITA_ZONE_ADD("anim measure: held pose error, worst vertex of each mesh (1/10 units)", (int)(holdMax * 10.0));
    VITA_ZONE_ADD("anim measure: extrapolated pose error, worst vertex of each mesh (1/10 units)", (int)(extrapMax * 10.0));
    VITA_ZONE_COUNT("anim measure: meshes compared");
}
#endif

#if defined(__vita__) && defined(__ARM_NEON__)
idCVar vita_skinUnitNormals("vita_skinUnitNormals", "1", CVAR_RENDERER | CVAR_BOOL, "Vita: the skin reads each mesh's unit normals and tangents made once instead of renormalizing them every frame (the same bytes)");

/*
============
idMD5Mesh::VitaUnitNormals

Made the first time the mesh is skinned; two model jobs may skin meshes of the
same model at once, so the array is made aside and installed with a
compare-and-swap (the loser frees its copy - the same values either way).
============
*/
const float* idMD5Mesh::VitaUnitNormals() const
{
    float* unit = vitaUnitNT;
    if (unit != NULL || !vita_skinUnitNormals.GetBool() || deformInfo == NULL) {
        return unit;
    }
    const int n = deformInfo->numOutputVerts;
    float* fresh = (float*)Mem_Alloc16(n * 8 * sizeof(float), TAG_MODEL);
    if (fresh == NULL) {
        return NULL;
    }
    Vita_UnitNormalsTangents_NEON(fresh, n, deformInfo->verts);
    if (__sync_bool_compare_and_swap(&vitaUnitNT, (float*)NULL, fresh)) {
        VITA_ZONE_ADD("dynamic model: unit normals made (vertices, once per mesh)", n);
    } else {
        Mem_Free16(fresh);
    }
    return vitaUnitNT;
}
#endif

/*
============
TransformVertsAndTangents
============
*/
void TransformVertsAndTangents(idDrawVert* targetVerts, const int numVerts, const idDrawVert* baseVerts, const idJointMat* joints)
{
#if defined(__vita__) && defined(__ARM_NEON__)
    Vita_TransformVertsAndTangents_NEON(targetVerts, numVerts, baseVerts, joints);
    return;
#endif
    for (int i = 0; i < numVerts; i++) {
        const idDrawVert& base = baseVerts[i];

        const idJointMat& j0 = joints[base.color[0]];
        const idJointMat& j1 = joints[base.color[1]];
        const idJointMat& j2 = joints[base.color[2]];
        const idJointMat& j3 = joints[base.color[3]];

        const float w0 = base.color2[0] * (1.0f / 255.0f);
        const float w1 = base.color2[1] * (1.0f / 255.0f);
        const float w2 = base.color2[2] * (1.0f / 255.0f);
        const float w3 = base.color2[3] * (1.0f / 255.0f);

        idJointMat accum;
        idJointMat::Mul(accum, j0, w0);
        idJointMat::Mad(accum, j1, w1);
        idJointMat::Mad(accum, j2, w2);
        idJointMat::Mad(accum, j3, w3);

        targetVerts[i].xyz = accum * idVec4(base.xyz.x, base.xyz.y, base.xyz.z, 1.0f);
        targetVerts[i].SetNormal(accum * base.GetNormal());
        targetVerts[i].SetTangent(accum * base.GetTangent());
        targetVerts[i].tangent[3] = base.tangent[3];
    }
}

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
/*
============
Vita_TransformPositions

The positions of TransformVertsAndTangents, computed the same way, alone.
============
*/
static void Vita_TransformPositions(idVec3* targetXyz, const int numVerts, const idDrawVert* baseVerts, const idJointMat* joints)
{
#if defined(__vita__) && defined(__ARM_NEON__)
    Vita_TransformPositions_NEON(targetXyz, numVerts, baseVerts, joints);
#else
    for (int i = 0; i < numVerts; i++) {
        const idDrawVert& base = baseVerts[i];

        const idJointMat& j0 = joints[base.color[0]];
        const idJointMat& j1 = joints[base.color[1]];
        const idJointMat& j2 = joints[base.color[2]];
        const idJointMat& j3 = joints[base.color[3]];

        const float w0 = base.color2[0] * (1.0f / 255.0f);
        const float w1 = base.color2[1] * (1.0f / 255.0f);
        const float w2 = base.color2[2] * (1.0f / 255.0f);
        const float w3 = base.color2[3] * (1.0f / 255.0f);

        idJointMat accum;
        idJointMat::Mul(accum, j0, w0);
        idJointMat::Mad(accum, j1, w1);
        idJointMat::Mad(accum, j2, w2);
        idJointMat::Mad(accum, j3, w3);

        targetXyz[i] = accum * idVec4(base.xyz.x, base.xyz.y, base.xyz.z, 1.0f);
    }
#endif
}

/*
============
Vita_TransformPositionsIndexed

Vita_TransformPositions for the listed vertices only, each to its own place:
the same arithmetic per vertex, so the same positions.
============
*/
static void Vita_TransformPositionsIndexed(idVec3* targetXyz, const triIndex_t* list, const int numList, const idDrawVert* baseVerts, const idJointMat* joints)
{
#if defined(__vita__) && defined(__ARM_NEON__)
    const float32x4_t inv255 = vdupq_n_f32(1.0f / 255.0f);
    for (int k = 0; k < numList; k++) {
        const int i = list[k];
        const idDrawVert& base = baseVerts[i];
        float32x4_t c0, c1, c2, c3;
        Vita_SkinColumns(base, joints, inv255, c0, c1, c2, c3);
        const float32x4_t pos = Vita_SkinPosition(base, c0, c1, c2, c3);
        vst1_f32(&targetXyz[i].x, vget_low_f32(pos));
        vst1q_lane_f32(&targetXyz[i].z, pos, 2);
    }
#else
    for (int k = 0; k < numList; k++) {
        const int i = list[k];
        Vita_TransformPositions(targetXyz + i, 1, baseVerts + i, joints);
    }
#endif
}
#endif

/*
====================
idMD5Mesh::UpdateSurface
====================
*/
void idMD5Mesh::UpdateSurface(const struct renderEntity_s* ent, const idJointMat* entJoints,
    const idJointMat* entJointsInverted, modelSurface_t* surf)
{

    tr.pc.c_deformedSurfaces++;
    tr.pc.c_deformedVerts += deformInfo->numOutputVerts;
    tr.pc.c_deformedIndexes += deformInfo->numIndexes;

    surf->shader = shader;

    if (surf->geometry != NULL) {
        // if the number of verts and indexes are the same we can re-use the triangle surface
        if (surf->geometry->numVerts == deformInfo->numOutputVerts && surf->geometry->numIndexes == deformInfo->numIndexes) {
            R_FreeStaticTriSurfVertexCaches(surf->geometry);
        } else {
            R_FreeStaticTriSurf(surf->geometry);
            surf->geometry = R_AllocStaticTriSurf();
        }
    } else {
        surf->geometry = R_AllocStaticTriSurf();
    }

    srfTriangles_t* tri = surf->geometry;

    // note that some of the data is referenced, and should not be freed
    tri->referencedIndexes = true;
    tri->numIndexes = deformInfo->numIndexes;
    tri->indexes = deformInfo->indexes;
    tri->silIndexes = deformInfo->silIndexes;
    tri->numMirroredVerts = deformInfo->numMirroredVerts;
    tri->mirroredVerts = deformInfo->mirroredVerts;
    tri->numDupVerts = deformInfo->numDupVerts;
    tri->dupVerts = deformInfo->dupVerts;
    tri->numSilEdges = deformInfo->numSilEdges;
    tri->silEdges = deformInfo->silEdges;

    tri->indexCache = deformInfo->staticIndexCache;

    tri->numVerts = deformInfo->numOutputVerts;

    // RB: added check wether GPU skinning is available at all
    if (r_useGPUSkinning.GetBool() && glConfig.gpuSkinningAvailable) {
        if (tri->verts != NULL && tri->verts != deformInfo->verts) {
            R_FreeStaticTriSurfVerts(tri);
        }
        tri->verts = deformInfo->verts;
        tri->ambientCache = deformInfo->staticAmbientCache;
        tri->shadowCache = deformInfo->staticShadowCache;
        tri->referencedVerts = true;
    } else {
        if (tri->verts == NULL || tri->verts == deformInfo->verts) {
            tri->verts = NULL;
            R_AllocStaticTriSurfVerts(tri, deformInfo->numOutputVerts);
            assert(tri->verts != NULL);                                                                       // quiet analyze warning
            memcpy(tri->verts, deformInfo->verts, deformInfo->numOutputVerts * sizeof(deformInfo->verts[0])); // copy over the texture coordinates
        }
        {
            // vertices counted, one mesh in 8 timed (a different eighth each frame)
            VITA_ZONE_ADD("dynamic model: vertices skinned (CPU)", deformInfo->numOutputVerts);
#if defined(__vita__) && defined(__ARM_NEON__)
            const float* unit = VitaUnitNormals();
            VITA_ZONE_SAMPLED("dynamic model: skin a mesh (TransformVertsAndTangents; sampled)", ((uintptr_t)tri >> 5) + tr.frameCount, 3);
            if (unit != NULL) {
                Vita_TransformVertsAndTangentsUnit_NEON(tri->verts, deformInfo->numOutputVerts, deformInfo->verts, unit, entJointsInverted);
            } else {
                TransformVertsAndTangents(tri->verts, deformInfo->numOutputVerts, deformInfo->verts, entJointsInverted);
            }
#else
            VITA_ZONE_SAMPLED("dynamic model: skin a mesh (TransformVertsAndTangents; sampled)", ((uintptr_t)tri >> 5) + tr.frameCount, 3);
            TransformVertsAndTangents(tri->verts, deformInfo->numOutputVerts, deformInfo->verts, entJointsInverted);
#endif
        }
#if !defined(__vita__) && defined(ID_VITA_PHYSCHECK)
        if (ent != NULL && ent->vitaHalfRateAnim && vita_animLODMeasure.GetBool()) {
            Vita_AnimMeasure(tri);
        }
#endif
        tri->referencedVerts = false;
    }
    tri->tangentsCalculated = true;

    CalculateBounds(entJoints, tri->bounds);
}

/*
====================
idMD5Mesh::CalculateBounds
====================
*/
void idMD5Mesh::CalculateBounds(const idJointMat* entJoints, idBounds& bounds) const
{
#if defined(USE_INTRINSICS)

    __m128 minX = vector_float_posInfinity;
    __m128 minY = vector_float_posInfinity;
    __m128 minZ = vector_float_posInfinity;
    __m128 maxX = vector_float_negInfinity;
    __m128 maxY = vector_float_negInfinity;
    __m128 maxZ = vector_float_negInfinity;
    for (int i = 0; i < numMeshJoints; i++) {
        const idJointMat& joint = entJoints[meshJoints[i]];
        __m128 x = _mm_load_ps(joint.ToFloatPtr() + 0 * 4);
        __m128 y = _mm_load_ps(joint.ToFloatPtr() + 1 * 4);
        __m128 z = _mm_load_ps(joint.ToFloatPtr() + 2 * 4);
        minX = _mm_min_ps(minX, x);
        minY = _mm_min_ps(minY, y);
        minZ = _mm_min_ps(minZ, z);
        maxX = _mm_max_ps(maxX, x);
        maxY = _mm_max_ps(maxY, y);
        maxZ = _mm_max_ps(maxZ, z);
    }
    __m128 expand = _mm_splat_ps(_mm_load_ss(&maxJointVertDist), 0);
    minX = _mm_sub_ps(minX, expand);
    minY = _mm_sub_ps(minY, expand);
    minZ = _mm_sub_ps(minZ, expand);
    maxX = _mm_add_ps(maxX, expand);
    maxY = _mm_add_ps(maxY, expand);
    maxZ = _mm_add_ps(maxZ, expand);
    _mm_store_ss(bounds.ToFloatPtr() + 0, _mm_splat_ps(minX, 3));
    _mm_store_ss(bounds.ToFloatPtr() + 1, _mm_splat_ps(minY, 3));
    _mm_store_ss(bounds.ToFloatPtr() + 2, _mm_splat_ps(minZ, 3));
    _mm_store_ss(bounds.ToFloatPtr() + 3, _mm_splat_ps(maxX, 3));
    _mm_store_ss(bounds.ToFloatPtr() + 4, _mm_splat_ps(maxY, 3));
    _mm_store_ss(bounds.ToFloatPtr() + 5, _mm_splat_ps(maxZ, 3));

#else

    bounds.Clear();
    for (int i = 0; i < numMeshJoints; i++) {
        const idJointMat& joint = entJoints[meshJoints[i]];
        bounds.AddPoint(joint.GetTranslation());
    }
    bounds.ExpandSelf(maxJointVertDist);

#endif
}

/*
====================
idMD5Mesh::NearestJoint
====================
*/
int idMD5Mesh::NearestJoint(int a, int b, int c) const
{
    // duplicated vertices might not have weights
    int vertNum;
    if (a >= 0 && a < numVerts) {
        vertNum = a;
    } else if (b >= 0 && b < numVerts) {
        vertNum = b;
    } else if (c >= 0 && c < numVerts) {
        vertNum = c;
    } else {
        // all vertices are duplicates which shouldn't happen
        return 0;
    }

    const idDrawVert& v = deformInfo->verts[vertNum];

    int bestWeight = 0;
    int bestJoint = 0;
    for (int i = 0; i < 4; i++) {
        if (v.color2[i] > bestWeight) {
            bestWeight = v.color2[i];
            bestJoint = v.color[i];
        }
    }

    return bestJoint;
}

/***********************************************************************

        idRenderModelMD5

***********************************************************************/

/*
====================
idRenderModelMD5::ParseJoint
====================
*/
void idRenderModelMD5::ParseJoint(idLexer& parser, idMD5Joint* joint, idJointQuat* defaultPose)
{
    //
    // parse name
    //
    idToken token;
    parser.ReadToken(&token);
    joint->name = token;

    //
    // parse parent
    //
    int num = parser.ParseInt();
    if (num < 0) {
        joint->parent = NULL;
    } else {
        if (num >= joints.Num() - 1) {
            parser.Error("Invalid parent for joint '%s'", joint->name.c_str());
        }
        joint->parent = &joints[num];
    }

    //
    // parse default pose
    //
    parser.Parse1DMatrix(3, defaultPose->t.ToFloatPtr());
    parser.Parse1DMatrix(3, defaultPose->q.ToFloatPtr());
    defaultPose->q.w = defaultPose->q.CalcW();
}

/*
====================
idRenderModelMD5::InitFromFile
====================
*/
void idRenderModelMD5::InitFromFile(const char* fileName)
{
    name = fileName;
    LoadModel();
}

/*
========================
idRenderModelMD5::LoadBinaryModel
========================
*/
bool idRenderModelMD5::LoadBinaryModel(idFile* file, const ID_TIME_T sourceTimeStamp)
{

    if (!idRenderModelStatic::LoadBinaryModel(file, sourceTimeStamp)) {
        return false;
    }

    unsigned int magic = 0;
    file->ReadBig(magic);
    if (magic != MD5B_MAGIC) {
        return false;
    }

    int tempNum;
    file->ReadBig(tempNum);
    joints.SetNum(tempNum);
    for (int i = 0; i < joints.Num(); i++) {
        file->ReadString(joints[i].name);
        int offset;
        file->ReadBig(offset);
        if (offset >= 0) {
            joints[i].parent = joints.Ptr() + offset;
        } else {
            joints[i].parent = NULL;
        }
    }

    file->ReadBig(tempNum);
    defaultPose.SetNum(tempNum);
    for (int i = 0; i < defaultPose.Num(); i++) {
        file->ReadBig(defaultPose[i].q.x);
        file->ReadBig(defaultPose[i].q.y);
        file->ReadBig(defaultPose[i].q.z);
        file->ReadBig(defaultPose[i].q.w);
        file->ReadVec3(defaultPose[i].t);
    }

    file->ReadBig(tempNum);
    invertedDefaultPose.SetNum(tempNum);
    for (int i = 0; i < invertedDefaultPose.Num(); i++) {
        file->ReadBigArray(invertedDefaultPose[i].ToFloatPtr(), JOINTMAT_TYPESIZE);
    }
    SIMD_INIT_LAST_JOINT(invertedDefaultPose.Ptr(), joints.Num());

    file->ReadBig(tempNum);
    meshes.SetNum(tempNum);
    for (int i = 0; i < meshes.Num(); i++) {

        idStr materialName;
        file->ReadString(materialName);
        if (materialName.IsEmpty()) {
            meshes[i].shader = NULL;
        } else {
            meshes[i].shader = declManager->FindMaterial(materialName);
        }

        file->ReadBig(meshes[i].numVerts);
        file->ReadBig(meshes[i].numTris);

        file->ReadBig(meshes[i].numMeshJoints);
        meshes[i].meshJoints = (byte*)Mem_Alloc(meshes[i].numMeshJoints * sizeof(meshes[i].meshJoints[0]), TAG_MODEL);
        file->ReadBigArray(meshes[i].meshJoints, meshes[i].numMeshJoints);
        file->ReadBig(meshes[i].maxJointVertDist);

        meshes[i].deformInfo = (deformInfo_t*)R_ClearedStaticAlloc(sizeof(deformInfo_t));
        deformInfo_t& deform = *meshes[i].deformInfo;

        file->ReadBig(deform.numSourceVerts);
        file->ReadBig(deform.numOutputVerts);
        file->ReadBig(deform.numIndexes);
        file->ReadBig(deform.numMirroredVerts);
        file->ReadBig(deform.numDupVerts);
        file->ReadBig(deform.numSilEdges);

        srfTriangles_t tri;
        memset(&tri, 0, sizeof(srfTriangles_t));

        if (deform.numOutputVerts > 0) {
            R_AllocStaticTriSurfVerts(&tri, deform.numOutputVerts);
            deform.verts = tri.verts;
            file->ReadBigArray(deform.verts, deform.numOutputVerts);
        }

        if (deform.numIndexes > 0) {
            R_AllocStaticTriSurfIndexes(&tri, deform.numIndexes);
            R_AllocStaticTriSurfSilIndexes(&tri, deform.numIndexes);
            deform.indexes = tri.indexes;
            deform.silIndexes = tri.silIndexes;
            file->ReadBigArray(deform.indexes, deform.numIndexes);
            file->ReadBigArray(deform.silIndexes, deform.numIndexes);
        }

        if (deform.numMirroredVerts > 0) {
            R_AllocStaticTriSurfMirroredVerts(&tri, deform.numMirroredVerts);
            deform.mirroredVerts = tri.mirroredVerts;
            file->ReadBigArray(deform.mirroredVerts, deform.numMirroredVerts);
        }

        if (deform.numDupVerts > 0) {
            R_AllocStaticTriSurfDupVerts(&tri, deform.numDupVerts);
            deform.dupVerts = tri.dupVerts;
            file->ReadBigArray(deform.dupVerts, deform.numDupVerts * 2);
        }

        if (deform.numSilEdges > 0) {
            R_AllocStaticTriSurfSilEdges(&tri, deform.numSilEdges);
            deform.silEdges = tri.silEdges;
            assert(deform.silEdges != NULL);
            for (int j = 0; j < deform.numSilEdges; j++) {
                file->ReadBig(deform.silEdges[j].p1);
                file->ReadBig(deform.silEdges[j].p2);
                file->ReadBig(deform.silEdges[j].v1);
                file->ReadBig(deform.silEdges[j].v2);
            }
        }

        idShadowVertSkinned* shadowVerts = (idShadowVertSkinned*)Mem_Alloc(ALIGN(deform.numOutputVerts * 2 * sizeof(idShadowVertSkinned), 16), TAG_MODEL);
        idShadowVertSkinned::CreateShadowCache(shadowVerts, deform.verts, deform.numOutputVerts);

        deform.staticAmbientCache = vertexCache.AllocStaticVertex(deform.verts, ALIGN(deform.numOutputVerts * sizeof(idDrawVert), VERTEX_CACHE_ALIGN));
        deform.staticIndexCache = vertexCache.AllocStaticIndex(deform.indexes, ALIGN(deform.numIndexes * sizeof(triIndex_t), INDEX_CACHE_ALIGN));
        deform.staticShadowCache = vertexCache.AllocStaticVertex(shadowVerts, ALIGN(deform.numOutputVerts * 2 * sizeof(idShadowVertSkinned), VERTEX_CACHE_ALIGN));

        Mem_Free(shadowVerts);

        file->ReadBig(meshes[i].surfaceNum);
    }

    return true;
}

/*
========================
idRenderModelMD5::WriteBinaryModel
========================
*/
void idRenderModelMD5::WriteBinaryModel(idFile* file, ID_TIME_T* _timeStamp) const
{

    idRenderModelStatic::WriteBinaryModel(file);

    if (file == NULL) {
        return;
    }

    file->WriteBig(MD5B_MAGIC);

    file->WriteBig(joints.Num());
    for (int i = 0; i < joints.Num(); i++) {
        file->WriteString(joints[i].name);
        int offset = -1;
        if (joints[i].parent != NULL) {
            offset = joints[i].parent - joints.Ptr();
        }
        file->WriteBig(offset);
    }

    file->WriteBig(defaultPose.Num());
    for (int i = 0; i < defaultPose.Num(); i++) {
        file->WriteBig(defaultPose[i].q.x);
        file->WriteBig(defaultPose[i].q.y);
        file->WriteBig(defaultPose[i].q.z);
        file->WriteBig(defaultPose[i].q.w);
        file->WriteVec3(defaultPose[i].t);
    }

    file->WriteBig(invertedDefaultPose.Num());
    for (int i = 0; i < invertedDefaultPose.Num(); i++) {
        file->WriteBigArray(invertedDefaultPose[i].ToFloatPtr(), JOINTMAT_TYPESIZE);
    }

    file->WriteBig(meshes.Num());
    for (int i = 0; i < meshes.Num(); i++) {

        if (meshes[i].shader != NULL && meshes[i].shader->GetName() != NULL) {
            file->WriteString(meshes[i].shader->GetName());
        } else {
            file->WriteString("");
        }

        file->WriteBig(meshes[i].numVerts);
        file->WriteBig(meshes[i].numTris);

        file->WriteBig(meshes[i].numMeshJoints);
        file->WriteBigArray(meshes[i].meshJoints, meshes[i].numMeshJoints);
        file->WriteBig(meshes[i].maxJointVertDist);

        deformInfo_t& deform = *meshes[i].deformInfo;

        file->WriteBig(deform.numSourceVerts);
        file->WriteBig(deform.numOutputVerts);
        file->WriteBig(deform.numIndexes);
        file->WriteBig(deform.numMirroredVerts);
        file->WriteBig(deform.numDupVerts);
        file->WriteBig(deform.numSilEdges);

        if (deform.numOutputVerts > 0) {
            file->WriteBigArray(deform.verts, deform.numOutputVerts);
        }

        if (deform.numIndexes > 0) {
            file->WriteBigArray(deform.indexes, deform.numIndexes);
            file->WriteBigArray(deform.silIndexes, deform.numIndexes);
        }

        if (deform.numMirroredVerts > 0) {
            file->WriteBigArray(deform.mirroredVerts, deform.numMirroredVerts);
        }

        if (deform.numDupVerts > 0) {
            file->WriteBigArray(deform.dupVerts, deform.numDupVerts * 2);
        }

        if (deform.numSilEdges > 0) {
            for (int j = 0; j < deform.numSilEdges; j++) {
                file->WriteBig(deform.silEdges[j].p1);
                file->WriteBig(deform.silEdges[j].p2);
                file->WriteBig(deform.silEdges[j].v1);
                file->WriteBig(deform.silEdges[j].v2);
            }
        }

        file->WriteBig(meshes[i].surfaceNum);
    }
}

/*
====================
idRenderModelMD5::LoadModel

used for initial loads, reloadModel, and reloading the data of purged models
Upon exit, the model will absolutely be valid, but possibly as a default model
====================
*/
void idRenderModelMD5::LoadModel()
{

    int version;
    int num;
    int parentNum;
    idToken token;
    idLexer parser(LEXFL_ALLOWPATHNAMES | LEXFL_NOSTRINGESCAPECHARS);

    if (!purged) {
        PurgeModel();
    }
    purged = false;

    if (!parser.LoadFile(name)) {
        MakeDefaultModel();
        return;
    }

    parser.ExpectTokenString(MD5_VERSION_STRING);
    version = parser.ParseInt();

    if (version != MD5_VERSION) {
        parser.Error("Invalid version %d.  Should be version %d\n", version, MD5_VERSION);
    }

    //
    // skip commandline
    //
    parser.ExpectTokenString("commandline");
    parser.ReadToken(&token);

    // parse num joints
    parser.ExpectTokenString("numJoints");
    num = parser.ParseInt();
    joints.SetGranularity(1);
    joints.SetNum(num);
    defaultPose.SetGranularity(1);
    defaultPose.SetNum(num);

    // parse num meshes
    parser.ExpectTokenString("numMeshes");
    num = parser.ParseInt();
    if (num < 0) {
        parser.Error("Invalid size: %d", num);
    }
    meshes.SetGranularity(1);
    meshes.SetNum(num);

    //
    // parse joints
    //
    parser.ExpectTokenString("joints");
    parser.ExpectTokenString("{");
    idJointMat* poseMat = (idJointMat*)_alloca16(joints.Num() * sizeof(poseMat[0]));
    for (int i = 0; i < joints.Num(); i++) {
        idMD5Joint* joint = &joints[i];
        idJointQuat* pose = &defaultPose[i];

        ParseJoint(parser, joint, pose);
        poseMat[i].SetRotation(pose->q.ToMat3());
        poseMat[i].SetTranslation(pose->t);
        if (joint->parent) {
            parentNum = joint->parent - joints.Ptr();
            pose->q = (poseMat[i].ToMat3() * poseMat[parentNum].ToMat3().Transpose()).ToQuat();
            pose->t = (poseMat[i].ToVec3() - poseMat[parentNum].ToVec3()) * poseMat[parentNum].ToMat3().Transpose();
        }
    }
    parser.ExpectTokenString("}");

    //-----------------------------------------
    // create the inverse of the base pose joints to support tech6 style deformation
    // of base pose vertexes, normals, and tangents.
    //
    // vertex * joints * inverseJoints == vertex when joints is the base pose
    // When the joints are in another pose, it gives the animated vertex position
    //-----------------------------------------
    invertedDefaultPose.SetNum(SIMD_ROUND_JOINTS(joints.Num()));
    for (int i = 0; i < joints.Num(); i++) {
        invertedDefaultPose[i] = poseMat[i];
        invertedDefaultPose[i].Invert();
    }
    SIMD_INIT_LAST_JOINT(invertedDefaultPose.Ptr(), joints.Num());

    for (int i = 0; i < meshes.Num(); i++) {
        parser.ExpectTokenString("mesh");
        meshes[i].ParseMesh(parser, defaultPose.Num(), poseMat);
    }

    // calculate the bounds of the model
    bounds.Clear();
    for (int i = 0; i < meshes.Num(); i++) {
        idBounds meshBounds;
        meshes[i].CalculateBounds(poseMat, meshBounds);
        bounds.AddBounds(meshBounds);
    }

    // set the timestamp for reloadmodels
    fileSystem->ReadFile(name, NULL, &timeStamp);

    common->UpdateLevelLoadPacifier();
}

/*
==============
idRenderModelMD5::Print
==============
*/
void idRenderModelMD5::Print() const
{
    common->Printf("%s\n", name.c_str());
    common->Printf("Dynamic model.\n");
    common->Printf("Generated smooth normals.\n");
    common->Printf("    verts  tris weights material\n");
    int totalVerts = 0;
    int totalTris = 0;
    const idMD5Mesh* mesh = meshes.Ptr();
    for (int i = 0; i < meshes.Num(); i++, mesh++) {
        totalVerts += mesh->NumVerts();
        totalTris += mesh->NumTris();
        common->Printf("%2i: %5i %5i %s\n", i, mesh->NumVerts(), mesh->NumTris(), mesh->shader->GetName());
    }
    common->Printf("-----\n");
    common->Printf("%4i verts.\n", totalVerts);
    common->Printf("%4i tris.\n", totalTris);
    common->Printf("%4i joints.\n", joints.Num());
}

/*
==============
idRenderModelMD5::List
==============
*/
void idRenderModelMD5::List() const
{
    int i;
    const idMD5Mesh* mesh;
    int totalTris = 0;
    int totalVerts = 0;

    for (mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++) {
        totalTris += mesh->numTris;
        totalVerts += mesh->NumVerts();
    }
    common->Printf(" %4ik %3i %4i %4i %s(MD5)", Memory() / 1024, meshes.Num(), totalVerts, totalTris, Name());

    if (defaulted) {
        common->Printf(" (DEFAULTED)");
    }

    common->Printf("\n");
}

/*
====================
idRenderModelMD5::Bounds

This calculates a rough bounds by using the joint radii without
transforming all the points
====================
*/
idBounds idRenderModelMD5::Bounds(const renderEntity_t* ent) const
{
    if (ent == NULL) {
        // this is the bounds for the reference pose
        return bounds;
    }

    return ent->bounds;
}

/*
====================
idRenderModelMD5::DrawJoints
====================
*/
void idRenderModelMD5::DrawJoints(const renderEntity_t* ent, const viewDef_t* view) const
{
    int i;
    int num;
    idVec3 pos;
    const idJointMat* joint;
    const idMD5Joint* md5Joint;
    int parentNum;

    num = ent->numJoints;
    joint = ent->joints;
    md5Joint = joints.Ptr();
    for (i = 0; i < num; i++, joint++, md5Joint++) {
        pos = ent->origin + joint->ToVec3() * ent->axis;
        if (md5Joint->parent) {
            parentNum = md5Joint->parent - joints.Ptr();
            common->RW()->DebugLine(colorWhite, ent->origin + ent->joints[parentNum].ToVec3() * ent->axis, pos);
        }

        common->RW()->DebugLine(colorRed, pos, pos + joint->ToMat3()[0] * 2.0f * ent->axis);
        common->RW()->DebugLine(colorGreen, pos, pos + joint->ToMat3()[1] * 2.0f * ent->axis);
        common->RW()->DebugLine(colorBlue, pos, pos + joint->ToMat3()[2] * 2.0f * ent->axis);
    }

    idBounds bounds;

    bounds.FromTransformedBounds(ent->bounds, vec3_zero, ent->axis);
    common->RW()->DebugBounds(colorMagenta, bounds, ent->origin);

    if ((r_jointNameScale.GetFloat() != 0.0f) && (bounds.Expand(128.0f).ContainsPoint(view->renderView.vieworg - ent->origin))) {
        idVec3 offset(0, 0, r_jointNameOffset.GetFloat());
        float scale;

        scale = r_jointNameScale.GetFloat();
        joint = ent->joints;
        num = ent->numJoints;
        for (i = 0; i < num; i++, joint++) {
            pos = ent->origin + joint->ToVec3() * ent->axis;
            common->RW()->DrawText(joints[i].name, pos + offset, scale, colorWhite, view->renderView.viewaxis, 1);
        }
    }
}

/*
====================
TransformJoints
====================
*/
static void TransformJoints(idJointMat* __restrict outJoints, const int numJoints, const idJointMat* __restrict inJoints1, const idJointMat* __restrict inJoints2)
{

    float* outFloats = outJoints->ToFloatPtr();
    const float* inFloats1 = inJoints1->ToFloatPtr();
    const float* inFloats2 = inJoints2->ToFloatPtr();

    assert_16_byte_aligned(outFloats);
    assert_16_byte_aligned(inFloats1);
    assert_16_byte_aligned(inFloats2);

#if defined(USE_INTRINSICS)

    const __m128 mask_keep_last = __m128c(_mm_set_epi32(0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000));

    for (int i = 0; i < numJoints; i += 2, inFloats1 += 2 * 12, inFloats2 += 2 * 12, outFloats += 2 * 12) {
        __m128 m1a0 = _mm_load_ps(inFloats1 + 0 * 12 + 0);
        __m128 m1b0 = _mm_load_ps(inFloats1 + 0 * 12 + 4);
        __m128 m1c0 = _mm_load_ps(inFloats1 + 0 * 12 + 8);
        __m128 m1a1 = _mm_load_ps(inFloats1 + 1 * 12 + 0);
        __m128 m1b1 = _mm_load_ps(inFloats1 + 1 * 12 + 4);
        __m128 m1c1 = _mm_load_ps(inFloats1 + 1 * 12 + 8);

        __m128 m2a0 = _mm_load_ps(inFloats2 + 0 * 12 + 0);
        __m128 m2b0 = _mm_load_ps(inFloats2 + 0 * 12 + 4);
        __m128 m2c0 = _mm_load_ps(inFloats2 + 0 * 12 + 8);
        __m128 m2a1 = _mm_load_ps(inFloats2 + 1 * 12 + 0);
        __m128 m2b1 = _mm_load_ps(inFloats2 + 1 * 12 + 4);
        __m128 m2c1 = _mm_load_ps(inFloats2 + 1 * 12 + 8);

        __m128 tj0 = _mm_and_ps(m1a0, mask_keep_last);
        __m128 tk0 = _mm_and_ps(m1b0, mask_keep_last);
        __m128 tl0 = _mm_and_ps(m1c0, mask_keep_last);
        __m128 tj1 = _mm_and_ps(m1a1, mask_keep_last);
        __m128 tk1 = _mm_and_ps(m1b1, mask_keep_last);
        __m128 tl1 = _mm_and_ps(m1c1, mask_keep_last);

        __m128 ta0 = _mm_splat_ps(m1a0, 0);
        __m128 td0 = _mm_splat_ps(m1b0, 0);
        __m128 tg0 = _mm_splat_ps(m1c0, 0);
        __m128 ta1 = _mm_splat_ps(m1a1, 0);
        __m128 td1 = _mm_splat_ps(m1b1, 0);
        __m128 tg1 = _mm_splat_ps(m1c1, 0);

        __m128 ra0 = _mm_add_ps(tj0, _mm_mul_ps(ta0, m2a0));
        __m128 rd0 = _mm_add_ps(tk0, _mm_mul_ps(td0, m2a0));
        __m128 rg0 = _mm_add_ps(tl0, _mm_mul_ps(tg0, m2a0));
        __m128 ra1 = _mm_add_ps(tj1, _mm_mul_ps(ta1, m2a1));
        __m128 rd1 = _mm_add_ps(tk1, _mm_mul_ps(td1, m2a1));
        __m128 rg1 = _mm_add_ps(tl1, _mm_mul_ps(tg1, m2a1));

        __m128 tb0 = _mm_splat_ps(m1a0, 1);
        __m128 te0 = _mm_splat_ps(m1b0, 1);
        __m128 th0 = _mm_splat_ps(m1c0, 1);
        __m128 tb1 = _mm_splat_ps(m1a1, 1);
        __m128 te1 = _mm_splat_ps(m1b1, 1);
        __m128 th1 = _mm_splat_ps(m1c1, 1);

        __m128 rb0 = _mm_add_ps(ra0, _mm_mul_ps(tb0, m2b0));
        __m128 re0 = _mm_add_ps(rd0, _mm_mul_ps(te0, m2b0));
        __m128 rh0 = _mm_add_ps(rg0, _mm_mul_ps(th0, m2b0));
        __m128 rb1 = _mm_add_ps(ra1, _mm_mul_ps(tb1, m2b1));
        __m128 re1 = _mm_add_ps(rd1, _mm_mul_ps(te1, m2b1));
        __m128 rh1 = _mm_add_ps(rg1, _mm_mul_ps(th1, m2b1));

        __m128 tc0 = _mm_splat_ps(m1a0, 2);
        __m128 tf0 = _mm_splat_ps(m1b0, 2);
        __m128 ti0 = _mm_splat_ps(m1c0, 2);
        __m128 tf1 = _mm_splat_ps(m1b1, 2);
        __m128 ti1 = _mm_splat_ps(m1c1, 2);
        __m128 tc1 = _mm_splat_ps(m1a1, 2);

        __m128 rc0 = _mm_add_ps(rb0, _mm_mul_ps(tc0, m2c0));
        __m128 rf0 = _mm_add_ps(re0, _mm_mul_ps(tf0, m2c0));
        __m128 ri0 = _mm_add_ps(rh0, _mm_mul_ps(ti0, m2c0));
        __m128 rc1 = _mm_add_ps(rb1, _mm_mul_ps(tc1, m2c1));
        __m128 rf1 = _mm_add_ps(re1, _mm_mul_ps(tf1, m2c1));
        __m128 ri1 = _mm_add_ps(rh1, _mm_mul_ps(ti1, m2c1));

        _mm_store_ps(outFloats + 0 * 12 + 0, rc0);
        _mm_store_ps(outFloats + 0 * 12 + 4, rf0);
        _mm_store_ps(outFloats + 0 * 12 + 8, ri0);
        _mm_store_ps(outFloats + 1 * 12 + 0, rc1);
        _mm_store_ps(outFloats + 1 * 12 + 4, rf1);
        _mm_store_ps(outFloats + 1 * 12 + 8, ri1);
    }

#else

    for (int i = 0; i < numJoints; i++) {
        idJointMat::Multiply(outJoints[i], inJoints1[i], inJoints2[i]);
    }

#endif
}

/*
====================
idRenderModelMD5::InstantiateDynamicModel
====================
*/
idRenderModel* idRenderModelMD5::InstantiateDynamicModel(const struct renderEntity_s* ent, const viewDef_t* view, idRenderModel* cachedModel)
{
    if (cachedModel != NULL && !r_useCachedDynamicModels.GetBool()) {
        delete cachedModel;
        cachedModel = NULL;
    }

    if (purged) {
        common->DWarning("model %s instantiated while purged", Name());
        LoadModel();
    }

    if (!ent->joints) {
        common->Printf("idRenderModelMD5::InstantiateDynamicModel: NULL joints on renderEntity for '%s'\n", Name());
        delete cachedModel;
        return NULL;
    } else if (ent->numJoints != joints.Num()) {
        common->Printf("idRenderModelMD5::InstantiateDynamicModel: renderEntity has different number of joints than model for '%s'\n", Name());
        delete cachedModel;
        return NULL;
    }

    tr.pc.c_generateMd5++;

    idRenderModelStatic* staticModel;
    if (cachedModel != NULL) {
        assert(dynamic_cast<idRenderModelStatic*>(cachedModel) != NULL);
        assert(idStr::Icmp(cachedModel->Name(), MD5_SnapshotName) == 0);
        staticModel = static_cast<idRenderModelStatic*>(cachedModel);
    } else {
        staticModel = new (TAG_MODEL) idRenderModelStatic;
        staticModel->InitEmpty(MD5_SnapshotName);
    }

    staticModel->bounds.Clear();

    if (r_showSkel.GetInteger()) {
        if ((view != NULL) && (!r_skipSuppress.GetBool() || !ent->suppressSurfaceInViewID || (ent->suppressSurfaceInViewID != view->renderView.viewID))) {
            // only draw the skeleton
            DrawJoints(ent, view);
        }

        if (r_showSkel.GetInteger() > 1) {
            // turn off the model when showing the skeleton
            staticModel->InitEmpty(MD5_SnapshotName);
            return staticModel;
        }
    }

    // update the GPU joints array
    const int numInvertedJoints = SIMD_ROUND_JOINTS(joints.Num());
    if (staticModel->jointsInverted == NULL) {
        staticModel->numInvertedJoints = numInvertedJoints;
        const int alignment = glConfig.uniformBufferOffsetAlignment;
        staticModel->jointsInverted = (idJointMat*)Mem_ClearedAlloc(ALIGN(numInvertedJoints * sizeof(idJointMat), alignment), TAG_JOINTMAT);
        staticModel->jointsInvertedBuffer = 0;
    } else {
        assert(staticModel->numInvertedJoints == numInvertedJoints);
    }

    TransformJoints(staticModel->jointsInverted, joints.Num(), ent->joints, invertedDefaultPose.Ptr());

    // create all the surfaces
    idMD5Mesh* mesh = meshes.Ptr();
    for (int i = 0; i < meshes.Num(); i++, mesh++) {
        // avoid deforming the surface if it will be a nodraw due to a skin remapping
        const idMaterial* shader = mesh->shader;

        shader = R_RemapShaderBySkin(shader, ent->customSkin, ent->customShader);

        if (!shader || (!shader->IsDrawn() && !shader->SurfaceCastsShadow())) {
            staticModel->DeleteSurfaceWithId(i);
            mesh->surfaceNum = -1;
            continue;
        }

        modelSurface_t* surf;

        int surfaceNum = 0;
        if (staticModel->FindSurfaceWithId(i, surfaceNum)) {
            mesh->surfaceNum = surfaceNum;
            surf = &staticModel->surfaces[surfaceNum];
        } else {
            mesh->surfaceNum = staticModel->NumSurfaces();
            surf = &staticModel->surfaces.Alloc();
            surf->geometry = NULL;
            surf->shader = NULL;
            surf->id = i;
        }

        mesh->UpdateSurface(ent, ent->joints, staticModel->jointsInverted, surf);
        assert(surf->geometry != NULL); // to get around compiler warning

        // the deformation of the tangents can be deferred until each surface is added to the view
        surf->geometry->staticModelWithJoints = staticModel;

        staticModel->bounds.AddBounds(surf->geometry->bounds);
    }

    return staticModel;
}

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
/*
====================
idMD5Mesh::VitaBuildTraceGroups

The mesh's triangles grouped for render model traces, once per mesh: by the
heaviest joint of each triangle's first vertex, in their order in the mesh.
Each group keeps the vertices its triangles use (each once), the joints those
vertices hang on (weight above zero) and the farthest any of them is from one
of those joints in the base pose - the distance the joint carries it at in
any pose (see VitaPoseForTrace).
====================
*/
void idMD5Mesh::VitaFreeTraceGroups() const
{
    Mem_Free(vitaGroups);
    Mem_Free(vitaGroupJoints);
    Mem_Free(vitaGroupVerts);
    Mem_Free(vitaGroupIndexes);
    Mem_Free(vitaGroupTriPos);
    vitaGroups = NULL;
    vitaGroupJoints = NULL;
    vitaGroupVerts = NULL;
    vitaGroupIndexes = NULL;
    vitaGroupTriPos = NULL;
    vitaNumGroups = 0;
    vitaGroupsOf = NULL;
}

void idMD5Mesh::VitaBuildTraceGroups(const idJointMat* invertedDefaultPose) const
{
    VitaFreeTraceGroups();
    const deformInfo_t* di = deformInfo;
    const int numTris = di->numIndexes / 3;
    const int numMeshVerts = di->numOutputVerts;

    int groupOfJoint[256];
    for (int j = 0; j < 256; j++) {
        groupOfJoint[j] = -1;
    }
    int* triGroup = (int*)Mem_Alloc(Max(1, numTris) * sizeof(int), TAG_MODEL);
    int numGroups = 0;
    for (int t = 0; t < numTris; t++) {
        const int j = di->verts[di->indexes[t * 3]].color[0];
        if (groupOfJoint[j] < 0) {
            groupOfJoint[j] = numGroups++;
        }
        triGroup[t] = groupOfJoint[j];
    }

    vitaGroups = (vitaTraceGroup_t*)Mem_ClearedAlloc(Max(1, numGroups) * sizeof(vitaTraceGroup_t), TAG_MODEL);
    vitaGroupIndexes = (triIndex_t*)Mem_Alloc(Max(1, numTris * 3) * sizeof(triIndex_t), TAG_MODEL);
    vitaGroupTriPos = (int*)Mem_Alloc(Max(1, numTris) * sizeof(int), TAG_MODEL);
    for (int t = 0; t < numTris; t++) {
        vitaGroups[triGroup[t]].numTris++;
    }
    for (int g = 0, first = 0; g < numGroups; g++) {
        vitaGroups[g].firstTri = first;
        first += vitaGroups[g].numTris;
        vitaGroups[g].numTris = 0;
    }
    for (int t = 0; t < numTris; t++) {
        vitaTraceGroup_t& g = vitaGroups[triGroup[t]];
        const int k = g.firstTri + g.numTris++;
        vitaGroupIndexes[k * 3 + 0] = di->indexes[t * 3 + 0];
        vitaGroupIndexes[k * 3 + 1] = di->indexes[t * 3 + 1];
        vitaGroupIndexes[k * 3 + 2] = di->indexes[t * 3 + 2];
        vitaGroupTriPos[k] = t;
    }
    Mem_Free(triGroup);

    // each group's vertices, joints and reach from them
    int* stamp = (int*)Mem_Alloc(Max(1, numMeshVerts) * sizeof(int), TAG_MODEL);
    for (int v = 0; v < numMeshVerts; v++) {
        stamp[v] = -1;
    }
    idList<triIndex_t> verts;
    idList<byte> jointList;
    verts.SetGranularity(1024);
    jointList.SetGranularity(256);
    for (int gi = 0; gi < numGroups; gi++) {
        vitaTraceGroup_t& g = vitaGroups[gi];
        bool hasJoint[256];
        memset(hasJoint, 0, sizeof(hasJoint));
        g.firstVert = verts.Num();
        g.dist = 0.0f;
        for (int k = g.firstTri * 3; k < (g.firstTri + g.numTris) * 3; k++) {
            const int v = vitaGroupIndexes[k];
            if (stamp[v] == gi) {
                continue;
            }
            stamp[v] = gi;
            verts.Append((triIndex_t)v);
            const idDrawVert& dv = di->verts[v];
            // how far it can be from the weighted blend of its joints' origins:
            // each joint carries it at the length of the vertex in the joint's
            // base frame, and the blend weighs those
            float weighted = 0.0f;
            for (int w = 0; w < 4; w++) {
                if (dv.color2[w] == 0) {
                    continue;
                }
                const int j = dv.color[w];
                hasJoint[j] = true;
                const float d = (invertedDefaultPose[j] * idVec4(dv.xyz.x, dv.xyz.y, dv.xyz.z, 1.0f)).Length();
                weighted += dv.color2[w] * (1.0f / 255.0f) * d;
            }
            g.dist = Max(g.dist, weighted);
        }
        g.numVerts = verts.Num() - g.firstVert;
        g.firstJoint = jointList.Num();
        for (int j = 0; j < 256; j++) {
            if (hasJoint[j]) {
                jointList.Append((byte)j);
            }
        }
        g.numJoints = jointList.Num() - g.firstJoint;
    }
    Mem_Free(stamp);

    vitaGroupVerts = (triIndex_t*)Mem_Alloc(Max(1, verts.Num()) * sizeof(triIndex_t), TAG_MODEL);
    if (verts.Num() > 0) {
        memcpy(vitaGroupVerts, verts.Ptr(), verts.Num() * sizeof(triIndex_t));
    }
    vitaGroupJoints = (byte*)Mem_Alloc(Max(1, jointList.Num()), TAG_MODEL);
    if (jointList.Num() > 0) {
        memcpy(vitaGroupJoints, jointList.Ptr(), jointList.Num());
    }
    vitaNumGroups = numGroups;
    vitaGroupsOf = di;
}

/*
====================
idRenderModelMD5::VitaPoseForTrace

The posed positions of the meshes InstantiateDynamicModel would build for this
entity - the same meshes, joints and arithmetic, without the normals and
tangents, the GPU copies or the snapshot. For render model traces when the
entity has no snapshot (RenderWorld.cpp). False when it cannot be done here;
the trace then builds the snapshot as before.

Positions are made on demand, a trace group at a time (VitaTraceMesh): this
sets up the pose's skinning matrices and, for each mesh and each of its
groups, a box that holds every position it can have - so a trace that cannot
reach a box leaves what is in it unposed and untested. A position blends its
joints' transforms of the base vertex, sum of w * (origin + offset): each
offset is as long as the vertex is far from that joint in the base pose (the
joint matrices are rigid), so the position lies within sum of w * length -
the group's distance at most - of the weighted blend of the origins, which
lies in the joints' box (scaled away from the model origin by however much
the weights, bytes / 255, sum past 1). The mesh's own box is the bound
idMD5Mesh::CalculateBounds culls drawn surfaces with; all are widened for
that sum and for rounding.
====================
*/
static void Vita_JointReach(idBounds& reach, const idJointMat* entJoints, const byte* list, const int numList, const float dist, const float weightSumMax)
{
    reach.Clear();
    float farthest = 0.0f;
    for (int j = 0; j < numList; j++) {
        const idVec3 origin = entJoints[list[j]].GetTranslation();
        reach.AddPoint(origin);
        farthest = Max(farthest, origin.Length());
    }
    const float d = dist * 1.001f;
    const float overweight = Max(0.0f, weightSumMax - 1.0f) * (farthest + d);
    reach.ExpandSelf(d + overweight + 0.5f);
}

bool idRenderModelMD5::VitaPoseForTrace(const struct renderEntity_s* ent, vitaPosedModel_t& posed) const
{
    if (purged || ent->joints == NULL || ent->numJoints != joints.Num() || meshes.Num() > VITA_POSED_MAX_MESHES || r_showSkel.GetInteger() > 1) {
        return false;
    }

    // the snapshot's jointsInverted
    const int numInvertedJoints = SIMD_ROUND_JOINTS(joints.Num());
    if (numInvertedJoints > posed.maxJoints) {
        Mem_Free16(posed.joints);
        posed.maxJoints = ALIGN(numInvertedJoints, 16);
        posed.joints = (idJointMat*)Mem_Alloc16(posed.maxJoints * sizeof(idJointMat), TAG_MODEL);
    }
    TransformJoints(posed.joints, joints.Num(), ent->joints, invertedDefaultPose.Ptr());

    int numVerts = 0;
    int numGroups = 0;
    posed.numMeshes = 0;
    for (int i = 0; i < meshes.Num(); i++) {
        const idMD5Mesh& mesh = meshes[i];
        // the meshes InstantiateDynamicModel keeps
        const idMaterial* shader = R_RemapShaderBySkin(mesh.shader, ent->customSkin, ent->customShader);
        if (!shader || (!shader->IsDrawn() && !shader->SurfaceCastsShadow())) {
            continue;
        }
        if (mesh.vitaGroupsOf != mesh.deformInfo) {
            mesh.VitaBuildTraceGroups(invertedDefaultPose.Ptr());
        }
        if (mesh.vitaWeightSumMax < 0.0f) {
            int most = 0;
            for (int v = 0; v < mesh.deformInfo->numOutputVerts; v++) {
                const byte* w = mesh.deformInfo->verts[v].color2;
                most = Max(most, w[0] + w[1] + w[2] + w[3]);
            }
            mesh.vitaWeightSumMax = most / 255.0f;
        }
        vitaPosedMesh_t& pm = posed.meshes[posed.numMeshes++];
        pm.shader = mesh.shader;
        pm.numVerts = mesh.deformInfo->numOutputVerts;
        pm.firstVert = numVerts;
        pm.mesh = i;
        pm.firstGroup = numGroups;
        pm.numGroups = mesh.vitaNumGroups;
        numVerts += pm.numVerts;
        numGroups += pm.numGroups;
    }

    if (numVerts > posed.maxVerts) {
        Mem_Free16(posed.xyz);
        posed.maxVerts = ALIGN(numVerts, 256);
        posed.xyz = (idVec3*)Mem_Alloc16(posed.maxVerts * sizeof(idVec3), TAG_MODEL);
    }
    if (numGroups > posed.maxGroups) {
        Mem_Free(posed.groups);
        posed.maxGroups = ALIGN(numGroups, 64);
        posed.groups = (vitaPosedGroup_t*)Mem_Alloc(posed.maxGroups * sizeof(vitaPosedGroup_t), TAG_MODEL);
    }

    // the reaches, from the joints as they are now
    for (int i = 0; i < posed.numMeshes; i++) {
        vitaPosedMesh_t& pm = posed.meshes[i];
        const idMD5Mesh& mesh = meshes[pm.mesh];
        pm.reachValid = (mesh.numMeshJoints > 0);
        if (pm.reachValid) {
            Vita_JointReach(pm.reach, ent->joints, mesh.meshJoints, mesh.numMeshJoints, mesh.maxJointVertDist, mesh.vitaWeightSumMax);
        }
        for (int g = 0; g < pm.numGroups; g++) {
            const vitaTraceGroup_t& tg = mesh.vitaGroups[g];
            vitaPosedGroup_t& pg = posed.groups[pm.firstGroup + g];
            pg.posed = false;
            pg.reachValid = (tg.numJoints > 0);
            if (pg.reachValid) {
                Vita_JointReach(pg.reach, ent->joints, mesh.vitaGroupJoints + tg.firstJoint, tg.numJoints, tg.dist, mesh.vitaWeightSumMax);
            }
        }
    }
    return true;
}

/*
====================
idRenderModelMD5::VitaTraceMesh

R_LocalTracePositions against one posed mesh: the groups the trace cannot
reach are left unposed and untested (no triangle of theirs can be hit: a hit
lies within the trace's radius of a triangle, so of the group's box), the
others are posed if they are not yet and traced together
(R_LocalTracePositionsSubsets) - the whole mesh's result.
====================
*/
extern idCVar vita_posedTracesVerify;

static void Vita_VerifyGroupReach(const char* model, int mesh, const vitaPosedGroup_t& pg, const idVec3* xyz, const triIndex_t* list, int numList)
{
    static int groups, outside, verts;
    groups++;
    verts += numList;
    for (int k = 0; k < numList; k++) {
        const idVec3& p = xyz[list[k]];
        if (pg.reachValid && !pg.reach.ContainsPoint(p)) {
            if (outside++ < 20) {
                common->Printf("vita_posedTracesVerify: REACH MISSED a vertex of '%s' mesh %d: (%.3f %.3f %.3f) outside (%.3f %.3f %.3f)-(%.3f %.3f %.3f)\n",
                    model, mesh, p.x, p.y, p.z, pg.reach[0].x, pg.reach[0].y, pg.reach[0].z, pg.reach[1].x, pg.reach[1].y, pg.reach[1].z);
            }
        }
    }
    if ((groups % 2000) == 0) {
        common->Printf("vita_posedTracesVerify: %d trace groups posed on demand (%d vertices), %d vertices outside their reach\n", groups, verts, outside);
    }
}

localTrace_t idRenderModelMD5::VitaTraceMesh(vitaPosedModel_t& posed, int posedMesh, const idVec3& localStart, const idVec3& localEnd, float radius) const
{
    localTrace_t none;
    none.fraction = 1.0f;

    vitaPosedMesh_t& pm = posed.meshes[posedMesh];
    const idMD5Mesh& mesh = meshes[pm.mesh];
    if (pm.reachValid) {
        idBounds reach = pm.reach;
        reach.ExpandSelf(radius + 1.0f);
        if (!reach.LineIntersection(localStart, localEnd)) {
            VITA_ZONE_COUNT("model trace: meshes out of reach");
            return none;
        }
    }

    idVec3* xyz = posed.xyz + pm.firstVert;
    vitaTraceSubset_t* subsets = (vitaTraceSubset_t*)_alloca(Max(1, pm.numGroups) * sizeof(vitaTraceSubset_t));
    int numSubsets = 0;
    int posedNow = 0;
    int vertsPosedNow = 0;
    for (int g = 0; g < pm.numGroups; g++) {
        const vitaTraceGroup_t& tg = mesh.vitaGroups[g];
        vitaPosedGroup_t& pg = posed.groups[pm.firstGroup + g];
        const triIndex_t* list = mesh.vitaGroupVerts + tg.firstVert;
        if (!pg.posed) {
            if (pg.reachValid) {
                idBounds reach = pg.reach;
                reach.ExpandSelf(radius + 1.0f);
                if (!reach.LineIntersection(localStart, localEnd)) {
                    VITA_ZONE_COUNT("model trace: groups out of reach, left unposed");
                    continue;
                }
            }
            Vita_TransformPositionsIndexed(xyz, list, tg.numVerts, mesh.deformInfo->verts, posed.joints);
            pg.bounds.Clear();
            for (int k = 0; k < tg.numVerts; k++) {
                pg.bounds.AddPoint(xyz[list[k]]);
            }
            pg.posed = true;
            posedNow++;
            vertsPosedNow += tg.numVerts;
            if (vita_posedTracesVerify.GetBool()) {
                Vita_VerifyGroupReach(Name(), pm.mesh, pg, xyz, list, tg.numVerts);
            }
        }
        idBounds bounds = pg.bounds;
        bounds.ExpandSelf(radius + 1.0f);
        if (!bounds.LineIntersection(localStart, localEnd)) {
            VITA_ZONE_COUNT("model trace: groups missed (posed bounds)");
            continue;
        }
        vitaTraceSubset_t& sub = subsets[numSubsets++];
        sub.verts = list;
        sub.numVerts = tg.numVerts;
        sub.indexes = mesh.vitaGroupIndexes + tg.firstTri * 3;
        sub.triPos = mesh.vitaGroupTriPos + tg.firstTri;
        sub.numTris = tg.numTris;
    }
    VITA_ZONE_ADD("model trace: groups posed", posedNow);
    VITA_ZONE_ADD("model trace: vertices posed", vertsPosedNow);
    VITA_ZONE_ADD("model trace: groups traced", numSubsets);
    if (numSubsets == 0) {
        return none;
    }
    return R_LocalTracePositionsSubsets(localStart, localEnd, radius, xyz, pm.numVerts, subsets, numSubsets);
}

int idRenderModelMD5::VitaMeshNearestJoint(int mesh, int a, int b, int c) const
{
    return meshes[mesh].NearestJoint(a, b, c);
}
#endif

/*
====================
idRenderModelMD5::IsDynamicModel
====================
*/
dynamicModel_t idRenderModelMD5::IsDynamicModel() const
{
    return DM_CACHED;
}

/*
====================
idRenderModelMD5::NumJoints
====================
*/
int idRenderModelMD5::NumJoints() const
{
    return joints.Num();
}

/*
====================
idRenderModelMD5::GetJoints
====================
*/
const idMD5Joint* idRenderModelMD5::GetJoints() const
{
    return joints.Ptr();
}

/*
====================
idRenderModelMD5::GetDefaultPose
====================
*/
const idJointQuat* idRenderModelMD5::GetDefaultPose() const
{
    return defaultPose.Ptr();
}

/*
====================
idRenderModelMD5::GetJointHandle
====================
*/
jointHandle_t idRenderModelMD5::GetJointHandle(const char* name) const
{
    const idMD5Joint* joint = joints.Ptr();
    for (int i = 0; i < joints.Num(); i++, joint++) {
        if (idStr::Icmp(joint->name.c_str(), name) == 0) {
            return (jointHandle_t)i;
        }
    }

    return INVALID_JOINT;
}

/*
=====================
idRenderModelMD5::GetJointName
=====================
*/
const char* idRenderModelMD5::GetJointName(jointHandle_t handle) const
{
    if ((handle < 0) || (handle >= joints.Num())) {
        return "<invalid joint>";
    }

    return joints[handle].name;
}

/*
====================
idRenderModelMD5::NearestJoint
====================
*/
int idRenderModelMD5::NearestJoint(int surfaceNum, int a, int b, int c) const
{
    if (surfaceNum > meshes.Num()) {
        common->Error("idRenderModelMD5::NearestJoint: surfaceNum > meshes.Num()");
    }

    const idMD5Mesh* mesh = meshes.Ptr();
    for (int i = 0; i < meshes.Num(); i++, mesh++) {
        if (mesh->surfaceNum == surfaceNum) {
            return mesh->NearestJoint(a, b, c);
        }
    }
    return 0;
}

/*
====================
idRenderModelMD5::TouchData

models that are already loaded at level start time
will still touch their materials to make sure they
are kept loaded
====================
*/
void idRenderModelMD5::TouchData()
{
    for (int i = 0; i < meshes.Num(); i++) {
        declManager->FindMaterial(meshes[i].shader->GetName());
    }
}

/*
===================
idRenderModelMD5::PurgeModel

frees all the data, but leaves the class around for dangling references,
which can regenerate the data with LoadModel()
===================
*/
void idRenderModelMD5::PurgeModel()
{
    purged = true;
    joints.Clear();
    defaultPose.Clear();
    meshes.Clear();
}

/*
===================
idRenderModelMD5::Memory
===================
*/
int idRenderModelMD5::Memory() const
{
    int total = sizeof(*this);
    total += joints.MemoryUsed() + defaultPose.MemoryUsed() + meshes.MemoryUsed();

    // count up strings
    for (int i = 0; i < joints.Num(); i++) {
        total += joints[i].name.DynamicMemoryUsed();
    }

    // count up meshes
    for (int i = 0; i < meshes.Num(); i++) {
        const idMD5Mesh* mesh = &meshes[i];

        total += mesh->numMeshJoints * sizeof(mesh->meshJoints[0]);

        // sum up deform info
        total += sizeof(mesh->deformInfo);
        total += R_DeformInfoMemoryUsed(mesh->deformInfo);
    }
    return total;
}
