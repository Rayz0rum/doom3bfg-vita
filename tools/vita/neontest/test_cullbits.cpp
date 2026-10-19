// Checks Vita_ShadowVolumeCullBits_NEON (ShadowShared.cpp) against the
// generic R_ShadowVolumeCullBits arithmetic, bit for bit, with the NEON
// intrinsics emulated in scalar code (neon_emu.h).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "neon_emu.h"

typedef unsigned char byte;
#define IEEE_FLT_SIGNBITSET(a) (reinterpret_cast<const unsigned int&>(a) >> 31)

#define __vita__ 1
#define __ARM_NEON__ 1
#include "cullbits_under_test.inc"

static float frand(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }

// the generic loop from ShadowShared.cpp (idPlane::Distance is a*x + b*y + c*z + d)
static void Generic(byte* cullBits, byte& totalOr, const float radius, const float* planes, const float* verts, int numVerts)
{
    byte tOr = 0;
    for (int i = 0; i < numVerts; i++) {
        const float* v = verts + i * 4;
        float d[4];
        for (int k = 0; k < 4; k++) {
            d[k] = planes[k * 4 + 0] * v[0] + planes[k * 4 + 1] * v[1] + planes[k * 4 + 2] * v[2] + planes[k * 4 + 3];
        }
        const float t0 = d[0] + radius, t1 = d[1] + radius, t2 = d[2] + radius, t3 = d[3] + radius;
        const float s0 = d[0] - radius, s1 = d[1] - radius, s2 = d[2] - radius, s3 = d[3] - radius;
        byte bits;
        bits = IEEE_FLT_SIGNBITSET(t0) << 0;
        bits |= IEEE_FLT_SIGNBITSET(t1) << 1;
        bits |= IEEE_FLT_SIGNBITSET(t2) << 2;
        bits |= IEEE_FLT_SIGNBITSET(t3) << 3;
        bits |= IEEE_FLT_SIGNBITSET(s0) << 4;
        bits |= IEEE_FLT_SIGNBITSET(s1) << 5;
        bits |= IEEE_FLT_SIGNBITSET(s2) << 6;
        bits |= IEEE_FLT_SIGNBITSET(s3) << 7;
        bits ^= 0x0F;
        tOr |= bits;
        cullBits[i] = bits;
    }
    totalOr = tOr;
}

int main()
{
    srand(4321);
    long long verts = 0, mismatches = 0;
    for (int trial = 0; trial < 400; trial++) {
        const int numVerts = 1 + rand() % 3000;
        std::vector<float> v((numVerts + 4) * 4);
        for (int i = 0; i < numVerts; i++) {
            v[i * 4 + 0] = frand(-2000, 2000);
            v[i * 4 + 1] = frand(-2000, 2000);
            v[i * 4 + 2] = frand(-2000, 2000);
            v[i * 4 + 3] = (i & 1) ? 0.0f : 1.0f;
            if (rand() % 50 == 0) { v[i * 4 + 0] = 0.0f; v[i * 4 + 1] = -0.0f; v[i * 4 + 2] = 0.0f; } // zeros on purpose
        }
        float planes[16];
        for (int i = 0; i < 16; i++) planes[i] = frand(-1, 1);
        if (trial % 5 == 0) { planes[3] = 0.0f; planes[7] = -0.0f; } // distances of exactly +-0
        const float radius = (trial % 7 == 0) ? 0.0f : frand(0.5f, 8.0f);
        std::vector<byte> a((numVerts + 3) & ~3), b((numVerts + 3) & ~3);
        byte ta = 0, tb = 0;
        Generic(a.data(), ta, radius, planes, v.data(), numVerts);
        Vita_ShadowVolumeCullBits_NEON(b.data(), tb, radius, planes, v.data(), numVerts);
        int bad = 0;
        for (int i = 0; i < numVerts; i++) bad += a[i] != b[i];
        if (bad || ta != tb) { if (mismatches < 5) printf("trial %d: %d bytes differ, totalOr %02x vs %02x\n", trial, bad, ta, tb); mismatches += bad + (ta != tb); }
        verts += numVerts;
    }
    printf("cull bits: %lld vertices, %lld mismatches\n", verts, mismatches);
    return mismatches ? 1 : 0;
}
