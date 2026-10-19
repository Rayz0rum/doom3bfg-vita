// Vita NEON idRenderMatrix::Multiply and CullBoundsToMVPbits vs the engine's generic code.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "neon_emu.h"
typedef unsigned char byte;
#include "rendermatrix_under_test.inc"

// the generic code, as RenderMatrix.cpp has it
static void Multiply_Generic(const float* a, const float* b, float* out)
{
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
}
static int CullBits_Generic(const float* mvp, const float* bounds, bool zeroToOne)
{
    int bits = 0;
    float v[3];
    for (int x = 0; x < 2; x++) {
        v[0] = bounds[x * 3 + 0];
        for (int y = 0; y < 2; y++) {
            v[1] = bounds[y * 3 + 1];
            for (int z = 0; z < 2; z++) {
                v[2] = bounds[z * 3 + 2];
                float c[4];
                for (int i = 0; i < 4; i++) {
                    c[i] = v[0] * mvp[i * 4 + 0] + v[1] * mvp[i * 4 + 1] + v[2] * mvp[i * 4 + 2] + mvp[i * 4 + 3];
                }
                const float minW = zeroToOne ? 0.0f : -c[3];
                const float maxW = c[3];
                const float minZ = minW;
                if (c[0] > minW) bits |= (1 << 0);
                if (c[0] < maxW) bits |= (1 << 1);
                if (c[1] > minW) bits |= (1 << 2);
                if (c[1] < maxW) bits |= (1 << 3);
                if (c[2] > minZ) bits |= (1 << 4);
                if (c[2] < maxW) bits |= (1 << 5);
            }
        }
    }
    return bits;
}
static float frand(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }
int main()
{
    srand(7);
    int mulDiff = 0, cullDiff = 0, culled = 0;
    const int n = 300000;
    for (int t = 0; t < n; t++) {
        float a[16], b[16], o1[16], o2[16], bounds[6];
        for (int i = 0; i < 16; i++) { a[i] = frand(-3, 3); b[i] = frand(-300, 300); }
        Multiply_Generic(a, b, o1);
        Vita_MultiplyRenderMatrix_NEON(a, b, o2);
        mulDiff += memcmp(o1, o2, sizeof(o1)) != 0;
        for (int k = 0; k < 3; k++) {
            const float c = frand(-500, 500), e = frand(0, 200);
            bounds[k] = c - e;
            bounds[k + 3] = c + e;
        }
        float mvp[16];
        for (int i = 0; i < 16; i++) mvp[i] = frand(-1, 1) * (i % 4 == 3 ? 400.0f : 1.0f);
        const bool zeroToOne = (t & 1) != 0;
        const int g = CullBits_Generic(mvp, bounds, zeroToOne);
        const int v = Vita_CullBoundsBits_NEON(mvp, bounds, zeroToOne);
        cullDiff += g != v;
        culled += g != 63;
    }
    printf("render matrix: %d multiplies, %d differ | %d bounds culls (%d culled), %d differ\n", n, mulDiff, n, culled, cullDiff);
    return (mulDiff != 0 || cullDiff != 0) ? 1 : 0;
}
