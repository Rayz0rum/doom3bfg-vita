// Vita NEON TransformVertsAndTangents vs the engine's scalar version.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include "neon_emu.h"
typedef unsigned char byte;
#define ID_FORCE_INLINE inline
struct idVec3 { float x, y, z; };
struct idDrawVert { idVec3 xyz; unsigned short st[2]; byte normal[4]; byte tangent[4]; byte color[4]; byte color2[4]; };
struct idJointMat { float mat[12]; const float* ToFloatPtr() const { return mat; } };
#define __vita__ 1
#define __ARM_NEON__ 1
#include "skin_under_test.inc"
#include "skin_old_reference.inc"

static byte Ftob(float f) { int i = (int)f; return i < 0 ? 0 : (i > 255 ? 255 : (byte)i); }
static void Ref(idDrawVert* out, int n, const idDrawVert* base, const idJointMat* joints)
{
    for (int i = 0; i < n; i++) {
        const idDrawVert& b = base[i];
        float acc[12] = {};
        for (int k = 0; k < 4; k++) { const float w = b.color2[k] * (1.0f / 255.0f); for (int e = 0; e < 12; e++) acc[e] = (k == 0) ? joints[b.color[0]].mat[e] * w : acc[e] + joints[b.color[k]].mat[e] * w; }
        out[i].xyz.x = acc[0] * b.xyz.x + acc[1] * b.xyz.y + acc[2] * b.xyz.z + acc[3];
        out[i].xyz.y = acc[4] * b.xyz.x + acc[5] * b.xyz.y + acc[6] * b.xyz.z + acc[7];
        out[i].xyz.z = acc[8] * b.xyz.x + acc[9] * b.xyz.y + acc[10] * b.xyz.z + acc[11];
        for (int v = 0; v < 2; v++) {
            const byte* s = v ? b.tangent : b.normal; byte* d = v ? out[i].tangent : out[i].normal;
            float x = s[0] * (2.0f / 255.0f) - 1.0f, y = s[1] * (2.0f / 255.0f) - 1.0f, z = s[2] * (2.0f / 255.0f) - 1.0f;
            const float inv = 1.0f / sqrtf(x * x + y * y + z * z); x *= inv; y *= inv; z *= inv;
            const float tx = acc[0] * x + acc[1] * y + acc[2] * z, ty = acc[4] * x + acc[5] * y + acc[6] * z, tz = acc[8] * x + acc[9] * y + acc[10] * z;
            d[0] = Ftob((tx + 1.0f) * 127.5f + 0.5f); d[1] = Ftob((ty + 1.0f) * 127.5f + 0.5f); d[2] = Ftob((tz + 1.0f) * 127.5f + 0.5f);
        }
        out[i].tangent[3] = b.tangent[3];
    }
}
static float frand(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }
int main()
{
    srand(99);
    // joints: rotation-ish (orthonormal-ish) 3x3 plus translation, like a skeleton
    std::vector<idJointMat> joints(64);
    for (auto& j : joints) {
        float a = frand(0, 6.283f), c = cosf(a), s = sinf(a);
        float m[12] = { c, -s, 0, frand(-50, 50), s, c, 0, frand(-50, 50), 0, 0, 1, frand(-50, 50) };
        memcpy(j.mat, m, sizeof(m));
    }
    const int n = 20000;
    std::vector<idDrawVert> base(n), a(n), b(n);
    for (auto& v : base) {
        memset(&v, 0, sizeof(v));
        v.xyz = { frand(-60, 60), frand(-60, 60), frand(-60, 60) };
        for (int k = 0; k < 4; k++) { v.normal[k] = rand() % 256; v.tangent[k] = rand() % 256; v.color[k] = rand() % 64; }
        if (v.normal[0] == 127 && v.normal[1] == 127 && v.normal[2] == 127) v.normal[0] = 0;
        int w0 = rand() % 256, w1 = rand() % (256 - w0), w2 = rand() % (256 - w0 - w1);
        v.color2[0] = w0; v.color2[1] = w1; v.color2[2] = w2; v.color2[3] = 255 - w0 - w1 - w2;
    }
    a = base; b = base;
    Ref(a.data(), n, base.data(), joints.data());
    Vita_TransformVertsAndTangents_NEON(b.data(), n, base.data(), joints.data());
    int byteDiff = 0, byteDiffBig = 0; double posErr = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 4; k++) {
            int dn = abs(a[i].normal[k] - b[i].normal[k]), dt = abs(a[i].tangent[k] - b[i].tangent[k]);
            byteDiff += (dn != 0) + (dt != 0); byteDiffBig += (dn > 1) + (dt > 1);
        }
        posErr = std::max(posErr, (double)std::max({ fabsf(a[i].xyz.x - b[i].xyz.x), fabsf(a[i].xyz.y - b[i].xyz.y), fabsf(a[i].xyz.z - b[i].xyz.z) }));
        if (memcmp(a[i].st, b[i].st, 4) || memcmp(a[i].color, b[i].color, 8)) { printf("other fields changed at %d\n", i); return 1; }
    }
    printf("%d verts: position max abs error %g, normal/tangent bytes off by 1: %d, by more: %d (of %d)\n", n, posErr, byteDiff - byteDiffBig, byteDiffBig, n * 8);
    // the current version against the first NEON one: every byte the same
    std::vector<idDrawVert> c = base;
    Old_TransformVertsAndTangents_NEON(c.data(), n, base.data(), joints.data());
    int verts = 0;
    for (int i = 0; i < n; i++) verts += memcmp(&b[i], &c[i], sizeof(idDrawVert)) != 0;
    printf("against the first NEON version: %d of %d vertices differ\n", verts, n);
    // with the unit normals and tangents made once per mesh: every byte the same
    std::vector<float> unit(n * 8);
    Vita_UnitNormalsTangents_NEON(unit.data(), n, base.data());
    std::vector<idDrawVert> u = base;
    Vita_TransformVertsAndTangentsUnit_NEON(u.data(), n, base.data(), unit.data(), joints.data());
    int unitDiff = 0;
    for (int i = 0; i < n; i++) unitDiff += memcmp(&b[i], &u[i], sizeof(idDrawVert)) != 0;
    printf("with the unit normals made once: %d of %d vertices differ\n", unitDiff, n);
    if (unitDiff != 0) return 1;
    // positions only: the full skin's positions, bit for bit
    std::vector<idVec3> xyz(n);
    Vita_TransformPositions_NEON(xyz.data(), n, base.data(), joints.data());
    int posDiff = 0;
    for (int i = 0; i < n; i++) posDiff += memcmp(&xyz[i], &b[i].xyz, sizeof(idVec3)) != 0;
    printf("positions only against the full skin: %d of %d positions differ\n", posDiff, n);
    return (verts == 0 && posDiff == 0) ? 0 : 1;
}
