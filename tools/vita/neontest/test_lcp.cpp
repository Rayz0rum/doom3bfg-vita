// The LCP kernels of idlib/math/Lcp.cpp - Multiply, MultiplyAdd, DotProduct,
// LowerTriangularSolve, LowerTriangularSolveTranspose - built twice from the
// same source, once with the Vita's NEON branches (LCP_NEON, the intrinsics
// emulated in scalar code by neon_emu.h) and once with the generic code, and
// compared bit for bit on random matrices and vectors.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <vector>
#include "neon_emu.h"
static inline float32x4_t vnegq_f32(float32x4_t a) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = -a.v[i]; return r; }
static inline float32x4_t vmlsq_n_f32(float32x4_t a, float32x4_t b, float s) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] - b.v[i] * s; return r; }

#define assert_16_byte_aligned(p)
#define NODEFAULT default: break
#define ID_INLINE inline

struct idMatX {
    float* data;
    int rows, cols;
    float* ToFloatPtr() const { return data; }
    int GetNumColumns() const { return cols; }
    const float* operator[](int r) const { return data + r * cols; }
};

namespace neonv {
#define LCP_NEON
#include "lcp_under_test.inc"
#undef LCP_NEON
}
namespace generic {
#include "lcp_under_test.inc"
}

static float frand() { return (rand() / (float)RAND_MAX) * 2.0f - 1.0f; }

static float* AlignedFloats(std::vector<float>& storage, int n, int offset)
{
    storage.assign(n + 8, 0.0f);
    float* p = storage.data();
    while (((uintptr_t)p & 15) != 0) {
        p++;
    }
    return p + offset;
}

int main()
{
    srand(4321);
    int bad = 0;
    long long checks = 0;
    for (int trial = 0; trial < 20000 && bad < 10; trial++) {
        // ---- elementwise and dot products, at every alignment of dst
        const int count = rand() % 70;
        const int off = rand() % 4;
        std::vector<float> s0v, s1v, d1v, d2v;
        float* s0 = AlignedFloats(s0v, count, off);
        float* s1 = AlignedFloats(s1v, count, off);
        float* d1 = AlignedFloats(d1v, count, off);
        float* d2 = AlignedFloats(d2v, count, off);
        for (int i = 0; i < count; i++) {
            s0[i] = frand() * ((rand() % 8) ? 1.0f : 1e-20f);
            s1[i] = (rand() % 16) ? frand() : 0.0f;
            d1[i] = d2[i] = frand();
        }
        neonv::Multiply_SIMD(d1, s0, s1, count);
        generic::Multiply_SIMD(d2, s0, s1, count);
        bad += memcmp(d1, d2, count * sizeof(float)) != 0;
        const float c = frand();
        neonv::MultiplyAdd_SIMD(d1, c, s0, count);
        generic::MultiplyAdd_SIMD(d2, c, s0, count);
        bad += memcmp(d1, d2, count * sizeof(float)) != 0;
        float a = neonv::DotProduct_SIMD(s0, s1, count), b = generic::DotProduct_SIMD(s0, s1, count);
        bad += memcmp(&a, &b, sizeof(float)) != 0;
        checks += 3;

        // ---- triangular solves on a padded lower triangular matrix
        const int n = 1 + rand() % 64;
        const int nc = (n + 3) & ~3;
        std::vector<float> Lv, bv, x1v, x2v;
        float* Ld = AlignedFloats(Lv, nc * nc + 8, 0);
        float* bb = AlignedFloats(bv, nc, 0);
        float* x1 = AlignedFloats(x1v, nc, 0);
        float* x2 = AlignedFloats(x2v, nc, 0);
        for (int r = 0; r < nc; r++) {
            for (int k = 0; k < nc; k++) {
                Ld[r * nc + k] = (k < r) ? frand() * 0.5f : ((k == r) ? 1.0f : frand());
            }
            bb[r] = frand() * 10.0f;
            if ((rand() % 10) == 0) {
                bb[r] = -0.0f;
            }
        }
        idMatX L = { Ld, n, nc };
        const int skip = (rand() % 3) ? 0 : rand() % (n + 1);
        for (int k = 0; k < nc; k++) {
            x1[k] = x2[k] = frand(); // the first 'skip' are taken as already solved
        }
        neonv::LowerTriangularSolve_SIMD(L, x1, bb, n, skip);
        generic::LowerTriangularSolve_SIMD(L, x2, bb, n, skip);
        if (memcmp(x1, x2, n * sizeof(float)) != 0) {
            printf("LowerTriangularSolve differs: n %d skip %d\n", n, skip);
            bad++;
        }
        neonv::LowerTriangularSolveTranspose_SIMD(L, x1, bb, n);
        generic::LowerTriangularSolveTranspose_SIMD(L, x2, bb, n);
        if (memcmp(x1, x2, n * sizeof(float)) != 0) {
            printf("LowerTriangularSolveTranspose differs: n %d\n", n);
            bad++;
        }
        checks += 2;
    }
    printf("lcp kernels: %lld checks, %d differ\n", checks, bad);
    return bad ? 1 : 0;
}
