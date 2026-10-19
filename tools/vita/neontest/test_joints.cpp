// idSIMD_Generic::TransformJoints' NEON path (Simd_Generic.cpp) against
// idJointMat::operator*= (JointTransform.h, copied below), bit for bit, on
// random skeletons, with the NEON intrinsics emulated in scalar code.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <vector>
#include "neon_emu.h"

struct idJointMat {
    float mat[12];
    float* ToFloatPtr() { return mat; }
    const float* ToFloatPtr() const { return mat; }
    idJointMat& operator*=(const idJointMat& a)
    {
        float tmp[3];
        tmp[0] = mat[0 * 4 + 0] * a.mat[0 * 4 + 0] + mat[1 * 4 + 0] * a.mat[0 * 4 + 1] + mat[2 * 4 + 0] * a.mat[0 * 4 + 2];
        tmp[1] = mat[0 * 4 + 0] * a.mat[1 * 4 + 0] + mat[1 * 4 + 0] * a.mat[1 * 4 + 1] + mat[2 * 4 + 0] * a.mat[1 * 4 + 2];
        tmp[2] = mat[0 * 4 + 0] * a.mat[2 * 4 + 0] + mat[1 * 4 + 0] * a.mat[2 * 4 + 1] + mat[2 * 4 + 0] * a.mat[2 * 4 + 2];
        mat[0 * 4 + 0] = tmp[0];
        mat[1 * 4 + 0] = tmp[1];
        mat[2 * 4 + 0] = tmp[2];
        tmp[0] = mat[0 * 4 + 1] * a.mat[0 * 4 + 0] + mat[1 * 4 + 1] * a.mat[0 * 4 + 1] + mat[2 * 4 + 1] * a.mat[0 * 4 + 2];
        tmp[1] = mat[0 * 4 + 1] * a.mat[1 * 4 + 0] + mat[1 * 4 + 1] * a.mat[1 * 4 + 1] + mat[2 * 4 + 1] * a.mat[1 * 4 + 2];
        tmp[2] = mat[0 * 4 + 1] * a.mat[2 * 4 + 0] + mat[1 * 4 + 1] * a.mat[2 * 4 + 1] + mat[2 * 4 + 1] * a.mat[2 * 4 + 2];
        mat[0 * 4 + 1] = tmp[0];
        mat[1 * 4 + 1] = tmp[1];
        mat[2 * 4 + 1] = tmp[2];
        tmp[0] = mat[0 * 4 + 2] * a.mat[0 * 4 + 0] + mat[1 * 4 + 2] * a.mat[0 * 4 + 1] + mat[2 * 4 + 2] * a.mat[0 * 4 + 2];
        tmp[1] = mat[0 * 4 + 2] * a.mat[1 * 4 + 0] + mat[1 * 4 + 2] * a.mat[1 * 4 + 1] + mat[2 * 4 + 2] * a.mat[1 * 4 + 2];
        tmp[2] = mat[0 * 4 + 2] * a.mat[2 * 4 + 0] + mat[1 * 4 + 2] * a.mat[2 * 4 + 1] + mat[2 * 4 + 2] * a.mat[2 * 4 + 2];
        mat[0 * 4 + 2] = tmp[0];
        mat[1 * 4 + 2] = tmp[1];
        mat[2 * 4 + 2] = tmp[2];
        tmp[0] = mat[0 * 4 + 3] * a.mat[0 * 4 + 0] + mat[1 * 4 + 3] * a.mat[0 * 4 + 1] + mat[2 * 4 + 3] * a.mat[0 * 4 + 2];
        tmp[1] = mat[0 * 4 + 3] * a.mat[1 * 4 + 0] + mat[1 * 4 + 3] * a.mat[1 * 4 + 1] + mat[2 * 4 + 3] * a.mat[1 * 4 + 2];
        tmp[2] = mat[0 * 4 + 3] * a.mat[2 * 4 + 0] + mat[1 * 4 + 3] * a.mat[2 * 4 + 1] + mat[2 * 4 + 3] * a.mat[2 * 4 + 2];
        mat[0 * 4 + 3] = tmp[0];
        mat[1 * 4 + 3] = tmp[1];
        mat[2 * 4 + 3] = tmp[2];
        mat[0 * 4 + 3] += a.mat[0 * 4 + 3];
        mat[1 * 4 + 3] += a.mat[1 * 4 + 3];
        mat[2 * 4 + 3] += a.mat[2 * 4 + 3];
        return *this;
    }
};

// the NEON block, cut from Simd_Generic.cpp by run.py
static void Neon_TransformJoints(idJointMat* jointMats, const int* parents, const int firstJoint, const int lastJoint)
{
#include "joints_under_test.inc"
}

static float frand() { return (rand() / (float)RAND_MAX) * 2.0f - 1.0f; }

int main()
{
    srand(2468);
    long long joints = 0, bad = 0;
    for (int trial = 0; trial < 3000; trial++) {
        const int n = 2 + rand() % 120;
        std::vector<int> parents(n);
        std::vector<idJointMat> a(n), b(n);
        for (int i = 0; i < n; i++) {
            parents[i] = (i == 0) ? -1 : rand() % i;
            for (int k = 0; k < 12; k++) {
                float v = frand() * ((k % 4 == 3) ? 40.0f : 1.0f);
                if (rand() % 20 == 0) {
                    v = (rand() & 1) ? 0.0f : -0.0f;
                }
                a[i].mat[k] = v;
            }
        }
        b = a;
        for (int i = 1; i < n; i++) {
            a[i] *= a[parents[i]];
        }
        Neon_TransformJoints(b.data(), parents.data(), 1, n - 1);
        for (int i = 0; i < n; i++) {
            bad += memcmp(a[i].mat, b[i].mat, sizeof(a[i].mat)) != 0;
        }
        joints += n;
    }
    printf("transform joints: %lld joints, %lld differ\n", joints, bad);
    return bad ? 1 : 0;
}
