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
