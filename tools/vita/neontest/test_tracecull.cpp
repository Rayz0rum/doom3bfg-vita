// Vita_TraceCullBitsPositions_NEON (tr_trace.cpp, render model traces) against
// R_LocalTracePositions' generic loop, bit for bit, with the NEON intrinsics
// emulated in scalar code (neon_emu.h).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "neon_emu.h"
struct float32x4x3_t { float32x4_t val[3]; };
static inline float32x4x3_t vld3q_f32(const float* p) { float32x4x3_t r; for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) r.val[k].v[i] = p[i * 3 + k]; return r; }

typedef unsigned char byte;
#define IEEE_FLT_SIGNBITSET(a) (reinterpret_cast<const unsigned int&>(a) >> 31)

#define __vita__ 1
#define __ARM_NEON__ 1
#include "tracecull_under_test.inc"

static float frand(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }

// the generic loop of R_LocalTracePositions (idPlane::Distance is a*x + b*y + c*z + d)
static void Generic(byte* cullBits, byte& totalOr, const float radius, const float* planes, const float* xyz, int numVerts)
{
    byte tOr = 0;
    for (int i = 0; i < numVerts; i++) {
        const float* v = xyz + i * 3;
        float d[4];
        for (int k = 0; k < 4; k++) {
            d[k] = planes[k * 4 + 0] * v[0] + planes[k * 4 + 1] * v[1] + planes[k * 4 + 2] * v[2] + planes[k * 4 + 3];
        }
        byte bits = 0;
        for (int k = 0; k < 4; k++) {
            const float t = d[k] + radius;
            const float s = d[k] - radius;
            bits |= (byte)(IEEE_FLT_SIGNBITSET(t) << k);
            bits |= (byte)(IEEE_FLT_SIGNBITSET(s) << (k + 4));
        }
        bits ^= 0x0F;
        tOr |= bits;
        cullBits[i] = bits;
    }
    totalOr = tOr;
}

int main()
{
    srand(777);
    long long verts = 0, mismatches = 0;
    for (int trial = 0; trial < 400; trial++) {
        const int numVerts = 1 + rand() % 3000;
        std::vector<float> v((numVerts + 4) * 3);
        for (int i = 0; i < numVerts * 3; i++) {
            v[i] = frand(-100, 100);
            if (rand() % 80 == 0) {
                v[i] = (rand() & 1) ? 0.0f : -0.0f;
            }
        }
        float planes[16];
        for (int k = 0; k < 16; k++) {
            planes[k] = frand(-1, 1);
        }
        const float radius = (rand() % 3) ? 0.0f : frand(0, 8);
        std::vector<byte> a(numVerts + 4), b(numVerts + 4);
        byte ta = 0, tb = 0;
        Vita_TraceCullBitsPositions_NEON(a.data(), ta, radius, planes, v.data(), numVerts);
        Generic(b.data(), tb, radius, planes, v.data(), numVerts);
        for (int i = 0; i < numVerts; i++) {
            mismatches += a[i] != b[i];
        }
        mismatches += ta != tb;
        verts += numVerts;
    }
    printf("trace cull bits: %lld vertices, %lld mismatches\n", verts, mismatches);
    return mismatches ? 1 : 0;
}
