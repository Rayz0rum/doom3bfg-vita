#if defined(__vita__) && defined(__ARM_NEON__)
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
