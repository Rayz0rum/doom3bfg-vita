// Checks the Vita NEON shadow-volume helpers (lane logic) against the generic
// C code, with the intrinsics emulated in scalar code (neon_emu.h).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <malloc.h>
#include "neon_emu.h"

typedef unsigned char byte;
typedef unsigned short triIndex_t;
#define ID_FORCE_INLINE inline

struct idVec3 {
    float x, y, z;
    float operator[](int i) const { return (&x)[i]; }
    const float* ToFloatPtr() const { return &x; }
    idVec3 Cross(const idVec3& a) const { return { y * a.z - z * a.y, z * a.x - x * a.z, x * a.y - y * a.x }; }
    void NormalVectors(idVec3& left, idVec3& down) const
    {
        float d = x * x + y * y;
        left = d ? idVec3{ -y / sqrtf(d), x / sqrtf(d), 0 } : idVec3{ 1, 0, 0 };
        down = left.Cross(*this);
    }
};
#define _alloca16(n) ((void*)(((uintptr_t)alloca((n) + 15) + 15) & ~(uintptr_t)15))
struct idVec4 {
    float x, y, z, w;
    float* ToFloatPtr() { return &x; }
    idVec3& ToVec3() { return *(idVec3*)&x; }
};
struct idDrawVert {
    idVec3 xyz;
    unsigned short st[2];
    byte normal[4];
    byte tangent[4];
    byte color[4];
    byte color2[4];
};
static_assert(sizeof(idDrawVert) == 32, "drawvert");
struct idJointMat {
    float mat[12];
    const float* ToFloatPtr() const { return mat; }
};
struct idRenderMatrix {
    float m[16];
    const float* operator[](int i) const { return m + i * 4; }
};
static bool R_LineIntersectsTriangleExpandedWithSphere(const idVec3&, const idVec3&, const idVec3&, float, float, const idVec3&, const idVec3&, const idVec3&) { return false; }

static byte TriangleFacing_Generic(const idVec3& v1, const idVec3& v2, const idVec3& v3, const idVec3& lightOrigin)
{
    const float sx = v2.x - v1.x, sy = v2.y - v1.y, sz = v2.z - v1.z;
    const float tx = v3.x - v1.x, ty = v3.y - v1.y, tz = v3.z - v1.z;
    const float normalX = ty * sz - tz * sy;
    const float normalY = tz * sx - tx * sz;
    const float normalZ = tx * sy - ty * sx;
    const float normalW = normalX * v1.x + normalY * v1.y + normalZ * v1.z;
    const float d = lightOrigin.x * normalX + lightOrigin.y * normalY + lightOrigin.z * normalZ - normalW;
    return (d > 0.0f) ? 255 : 0;
}

static byte TriangleCulled_Generic(const idVec3& v1, const idVec3& v2, const idVec3& v3, const idRenderMatrix& lightProject)
{
    float c[3][4];
    for (int i = 0; i < 4; i++) {
        c[0][i] = v1[0] * lightProject[i][0] + v1[1] * lightProject[i][1] + v1[2] * lightProject[i][2] + lightProject[i][3];
        c[1][i] = v2[0] * lightProject[i][0] + v2[1] * lightProject[i][1] + v2[2] * lightProject[i][2] + lightProject[i][3];
        c[2][i] = v3[0] * lightProject[i][0] + v3[1] * lightProject[i][1] + v3[2] * lightProject[i][2] + lightProject[i][3];
    }
    int bits = 0;
    for (int i = 0; i < 3; i++) {
        const float minW = 0.0f, maxW = c[i][3];
        if (c[i][0] > minW) bits |= 1;
        if (c[i][0] < maxW) bits |= 2;
        if (c[i][1] > minW) bits |= 4;
        if (c[i][1] < maxW) bits |= 8;
        if (c[i][2] > minW) bits |= 16;
        if (c[i][2] < maxW) bits |= 32;
    }
    return (bits != 63) ? 255 : 0;
}

#define __vita__ 1
#define __ARM_NEON__ 1
#include "block_under_test.inc"

static float frand(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }

int main()
{
    srand(1234);
    int totalMismatch = 0, totalTris = 0;
    for (int trial = 0; trial < 50; trial++) {
        const int numVerts = 300 + rand() % 300;
        const int numTris = 900 + rand() % 7; // leftovers 0..3 exercised
        std::vector<idDrawVert> verts(numVerts);
        for (auto& v : verts) {
            memset(&v, 0, sizeof(v));
            v.xyz = { frand(-100, 100), frand(-100, 100), frand(-100, 100) };
            v.st[0] = 0x1234; // junk after z, must be ignored
        }
        std::vector<triIndex_t> idx(numTris * 3);
        for (auto& i : idx) i = (triIndex_t)(rand() % numVerts);
        idRenderMatrix lp;
        for (int i = 0; i < 16; i++) lp.m[i] = frand(-0.02f, 0.02f);
        lp.m[3] = 0.5f; lp.m[7] = 0.5f; lp.m[11] = 0.5f; lp.m[15] = 1.0f;
        const idVec3 lo = { frand(-150, 150), frand(-150, 150), frand(-150, 150) };
        const bool cullToLight = trial & 1;

        std::vector<byte> fN(numTris + 8, 7), cN(numTris + 8, 7), fG(numTris), cG(numTris);
        const int nN = Vita_TriangleFacingCulled_NEON(fN.data(), cN.data(), idx.data(), numTris * 3, (const byte*)verts.data(), sizeof(idDrawVert), lo, lp, cullToLight);
        int nG = 0;
        for (int t = 0; t < numTris; t++) {
            const idVec3& a = verts[idx[t * 3]].xyz;
            const idVec3& b = verts[idx[t * 3 + 1]].xyz;
            const idVec3& c = verts[idx[t * 3 + 2]].xyz;
            cG[t] = TriangleCulled_Generic(a, b, c, lp);
            fG[t] = TriangleFacing_Generic(a, b, c, lo) | (cG[t] & (cullToLight ? 255 : 0));
            nG += fG[t] & 1;
        }
        int mism = 0;
        for (int t = 0; t < numTris; t++) mism += (fN[t] != fG[t]) + (cN[t] != cG[t]);

        // the per-vertex version must give the per-corner version's bytes exactly
        {
            const int numVerts4 = (numVerts + 3) & ~3;
            std::vector<float> packed(numVerts4 * 4 + 4);
            std::vector<uint32_t> bitsStore(numVerts4 / 4 + 1);
            std::vector<byte> fV(numTris + 8, 7), cV(numTris + 8, 7);
            const int nV = Vita_TriangleFacingCulled_PerVertex_NEON(fV.data(), cV.data(), idx.data(), numTris * 3, (const byte*)verts.data(), sizeof(idDrawVert),
                numVerts, packed.data(), (byte*)bitsStore.data(), lo, lp, cullToLight);
            int exact = 0;
            for (int t = 0; t < numTris; t++) exact += (fV[t] != fN[t]) + (cV[t] != cN[t]);
            if (fV[numTris] != 7 || cV[numTris] != 7) { printf("trial %d: per-vertex wrote past the end\n", trial); return 1; }
            if (nV != nN || exact) { printf("trial %d: PER-VERTEX DIFFERS from per-corner: front %d vs %d, %d bytes\n", trial, nV, nN, exact); totalMismatch += exact + 1; }
        }
        if (fN[numTris] != 7 || cN[numTris] != 7) { printf("trial %d: wrote past the end\n", trial); return 1; }
        totalMismatch += mism;
        totalTris += numTris;
        if (nN != nG || mism) printf("trial %d: front %d vs %d, %d byte mismatches\n", trial, nN, nG, mism);

        // skinning
        std::vector<idJointMat> joints(20);
        for (auto& j : joints) for (int i = 0; i < 12; i++) j.mat[i] = frand(-2, 2);
        for (auto& v : verts) {
            for (int k = 0; k < 4; k++) v.color[k] = (byte)(rand() % 20);
            int a = rand() % 256, b = rand() % (256 - a), c = rand() % (256 - a - b);
            v.color2[0] = a; v.color2[1] = b; v.color2[2] = c; v.color2[3] = 255 - a - b - c;
        }
        std::vector<idVec4> tv(numVerts);
        Vita_SkinPositions_NEON(tv.data(), verts.data(), numVerts, joints.data());
        double worst = 0;
        for (int i = 0; i < numVerts; i++) {
            const idDrawVert& v = verts[i];
            float acc[12] = {};
            for (int k = 0; k < 4; k++) {
                const float w = v.color2[k] * (1.0f / 255.0f);
                for (int e = 0; e < 12; e++) acc[e] += joints[v.color[k]].mat[e] * w;
            }
            const float p[4] = { v.xyz.x, v.xyz.y, v.xyz.z, 1.0f };
            for (int r = 0; r < 3; r++) {
                const float ref = acc[r * 4] * p[0] + acc[r * 4 + 1] * p[1] + acc[r * 4 + 2] * p[2] + acc[r * 4 + 3] * p[3];
                const float got = (&tv[i].x)[r];
                worst = std::max(worst, (double)fabs(ref - got) / (1.0 + fabs(ref)));
            }
            if (tv[i].w != 1.0f) { printf("w != 1 at %d: %f\n", i, tv[i].w); return 1; }
        }
        if (worst > 1e-5) printf("trial %d: skinning relative error %g\n", trial, worst);
    }
    printf("done: %d triangles, %d facing/culled bytes differ\n", totalTris, totalMismatch);
    return 0;
}
