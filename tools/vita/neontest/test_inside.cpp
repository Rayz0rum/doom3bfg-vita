// Checks Vita_InsideShadowVolume (DynamicShadowVolume.cpp) - the precise
// "is the view inside the shadow volume" test with its plane prefilter, NEON
// emulated (neon_emu.h) - against the plain loop it replaced: every triangle
// facing away from the light given R_LineIntersectsTriangleExpandedWithSphere
// (the engine's, cut from ShadowShared.cpp). The answers must be the same.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <malloc.h>
#include "neon_emu.h"

typedef unsigned char byte;
typedef unsigned short triIndex_t;
#define ID_FORCE_INLINE inline
#define _alloca16(n) ((void*)(((uintptr_t)alloca((n) + 15) + 15) & ~(uintptr_t)15))

struct idMath {
    static constexpr float FLT_SMALLEST_NON_DENORMAL = 1.1754943508e-038f;
    static float Fabs(float f) { return fabsf(f); }
    static float InvSqrt(float f) { return 1.0f / sqrtf(f); }
    static float ClampFloat(float lo, float hi, float v) { return v < lo ? lo : (v > hi ? hi : v); }
};

struct idVec3 {
    float x, y, z;
    float operator[](int i) const { return (&x)[i]; }
    float& operator[](int i) { return (&x)[i]; }
    const float* ToFloatPtr() const { return &x; }
    idVec3 operator-(const idVec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    idVec3 operator+(const idVec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    idVec3 operator*(float f) const { return { x * f, y * f, z * f }; }
    float operator*(const idVec3& o) const { return x * o.x + y * o.y + z * o.z; }
    idVec3 Cross(const idVec3& a) const { return { y * a.z - z * a.y, z * a.x - x * a.z, x * a.y - y * a.x }; }
    float LengthSqr() const { return x * x + y * y + z * z; }
    void NormalVectors(idVec3& left, idVec3& down) const
    {
        float d = x * x + y * y;
        if (!d) {
            left = { 1, 0, 0 };
        } else {
            d = idMath::InvSqrt(d);
            left = { -y * d, x * d, 0 };
        }
        down = left.Cross(*this);
    }
};
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
struct idJointMat {
    float mat[12];
    const float* ToFloatPtr() const { return mat; }
};
struct idRenderMatrix {
    float m[16];
    const float* operator[](int i) const { return m + i * 4; }
};
#define VITA_ZONE(name)

#include "sphere_under_test.inc"

static byte TriangleFacing_Generic(const idVec3&, const idVec3&, const idVec3&, const idVec3&) { return 0; }
static byte TriangleCulled_Generic(const idVec3&, const idVec3&, const idVec3&, const idRenderMatrix&) { return 0; }

#define __vita__ 1
#define __ARM_NEON__ 1
#include "block_under_test.inc"

static float frand(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }

int main()
{
    srand(4321);
    int checks = 0, mismatches = 0, insides = 0;
    long long trisTested = 0, trisSkipped = 0;
    for (int trial = 0; trial < 20000; trial++) {
        // a cloud of small triangles around the origin; the segment from a
        // view point to a light point passes through, near or far from it
        const int numVerts = 5 + rand() % 400;
        const int numTris = 1 + rand() % 600;
        const float spread = frand(5, 200);
        const bool drawVerts = trial & 1; // idDrawVert stride or idVec4 (skinned) stride
        std::vector<idDrawVert> dv(numVerts);
        std::vector<idVec4> v4(numVerts);
        for (int i = 0; i < numVerts; i++) {
            const idVec3 p = { frand(-spread, spread), frand(-spread, spread), frand(-spread, spread) };
            memset(&dv[i], 0x5A, sizeof(dv[i]));
            dv[i].xyz = p;
            v4[i] = { p.x, p.y, p.z, 1.0f };
        }
        std::vector<triIndex_t> idx(numTris * 3);
        for (int t = 0; t < numTris; t++) {
            // mostly local triangles (a vertex and two near it in index order)
            const int a = rand() % numVerts;
            idx[t * 3 + 0] = a;
            idx[t * 3 + 1] = (rand() & 3) ? (a + 1) % numVerts : rand() % numVerts;
            idx[t * 3 + 2] = (rand() & 3) ? (a + 2) % numVerts : rand() % numVerts;
        }
        std::vector<byte> facing(numTris);
        for (auto& f : facing) f = (rand() & 1) ? 255 : 0;

        const float far = spread * frand(0.2f, 3.0f);
        const idVec3 lineStart = { frand(-far, far), frand(-far, far), frand(-far, far) };
        idVec3 lineEnd = { frand(-far, far), frand(-far, far), frand(-far, far) };
        if ((trial % 7) == 0) lineEnd = lineStart + idVec3{ frand(-0.001f, 0.001f), 0, 0 }; // near-degenerate
        const idVec3 delta = lineEnd - lineStart;
        const float lenSqr = delta.LengthSqr();
        if (lenSqr <= 0.0f) continue;
        const float rcp = idMath::InvSqrt(lenSqr);
        const idVec3 lineDir = delta * rcp;
        const float lineLength = lenSqr * rcp;
        const float radius = frand(0.0f, 12.0f);

        const byte* positions = drawVerts ? (const byte*)dv.data() : (const byte*)v4.data();
        const int stride = drawVerts ? (int)sizeof(idDrawVert) : (int)sizeof(idVec4);

        bool reference = false;
        for (int k = 0, n = 0; k + 3 <= numTris * 3; k += 3, n++) {
            if (!facing[n]) {
                const idVec3& a = *(const idVec3*)(positions + idx[k + 0] * stride);
                const idVec3& b = *(const idVec3*)(positions + idx[k + 1] * stride);
                const idVec3& c = *(const idVec3*)(positions + idx[k + 2] * stride);
                if (R_LineIntersectsTriangleExpandedWithSphere(lineStart, lineEnd, lineDir, lineLength, radius, c, b, a)) {
                    reference = true;
                    break;
                }
            }
        }
        const bool got = Vita_InsideShadowVolume(facing.data(), idx.data(), numTris * 3, positions, stride, numVerts,
            lineStart, lineEnd, lineDir, lineLength, radius);
        checks++;
        insides += reference ? 1 : 0;
        if (got != reference) {
            if (mismatches++ < 10) {
                printf("trial %d: MISMATCH reference %d prefiltered %d (radius %.3f, %d verts, %d tris)\n", trial, reference, got, radius, numVerts, numTris);
            }
        }
        (void)trisTested;
        (void)trisSkipped;
    }
    printf("inside test: %d checks (%d inside), %d answers differ\n", checks, insides, mismatches);
    return mismatches == 0 ? 0 : 1;
}
