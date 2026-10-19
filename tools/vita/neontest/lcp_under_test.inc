/*
========================
Multiply_SIMD

dst[i] = src0[i] * src1[i];

Assumes the source and destination have the same memory alignment.
========================
*/
static void Multiply_SIMD(float* dst, const float* src0, const float* src1, const int count)
{
    int i = 0;

    // RB: changed unsigned int to uintptr_t
    for (; ((uintptr_t)dst & 0xF) != 0 && i < count; i++)
    // RB end
    {
        dst[i] = src0[i] * src1[i];
    }

#if defined(LCP_SIMD)

    for (; i + 4 <= count; i += 4) {
        assert_16_byte_aligned(&dst[i]);
        assert_16_byte_aligned(&src0[i]);
        assert_16_byte_aligned(&src1[i]);

        __m128 s0 = _mm_load_ps(src0 + i);
        __m128 s1 = _mm_load_ps(src1 + i);
        s0 = _mm_mul_ps(s0, s1);
        _mm_store_ps(dst + i, s0);
    }

#elif defined(LCP_NEON)

    for (; i + 4 <= count; i += 4) {
        vst1q_f32(dst + i, vmulq_f32(vld1q_f32(src0 + i), vld1q_f32(src1 + i)));
    }

#else

    for (; i + 4 <= count; i += 4) {
        assert_16_byte_aligned(&dst[i]);
        assert_16_byte_aligned(&src0[i]);
        assert_16_byte_aligned(&src1[i]);

        dst[i + 0] = src0[i + 0] * src1[i + 0];
        dst[i + 1] = src0[i + 1] * src1[i + 1];
        dst[i + 2] = src0[i + 2] * src1[i + 2];
        dst[i + 3] = src0[i + 3] * src1[i + 3];
    }

#endif

    for (; i < count; i++) {
        dst[i] = src0[i] * src1[i];
    }
}

/*
========================
MultiplyAdd_SIMD

dst[i] += constant * src[i];

Assumes the source and destination have the same memory alignment.
========================
*/
static void MultiplyAdd_SIMD(float* dst, const float constant, const float* src, const int count)
{
    int i = 0;

    // RB: changed unsigned int to uintptr_t
    for (; ((uintptr_t)dst & 0xF) != 0 && i < count; i++)
    // RB end
    {
        dst[i] += constant * src[i];
    }

#if defined(LCP_SIMD)

    __m128 c = _mm_load1_ps(&constant);
    for (; i + 4 <= count; i += 4) {
        assert_16_byte_aligned(&dst[i]);
        assert_16_byte_aligned(&src[i]);

        __m128 s = _mm_load_ps(src + i);
        __m128 d = _mm_load_ps(dst + i);
        s = _mm_add_ps(_mm_mul_ps(s, c), d);
        _mm_store_ps(dst + i, s);
    }

#elif defined(LCP_NEON)

    for (; i + 4 <= count; i += 4) {
        vst1q_f32(dst + i, vmlaq_n_f32(vld1q_f32(dst + i), vld1q_f32(src + i), constant));
    }

#else

    for (; i + 4 <= count; i += 4) {
        assert_16_byte_aligned(&src[i]);
        assert_16_byte_aligned(&dst[i]);

        dst[i + 0] += constant * src[i + 0];
        dst[i + 1] += constant * src[i + 1];
        dst[i + 2] += constant * src[i + 2];
        dst[i + 3] += constant * src[i + 3];
    }

#endif

    for (; i < count; i++) {
        dst[i] += constant * src[i];
    }
}

/*
========================
DotProduct_SIMD

dot = src0[0] * src1[0] + src0[1] * src1[1] + src0[2] * src1[2] + ...
========================
*/
static float DotProduct_SIMD(const float* src0, const float* src1, const int count)
{
    assert_16_byte_aligned(src0);
    assert_16_byte_aligned(src1);

#if defined(LCP_SIMD)

    __m128 sum = (__m128&)SIMD_SP_zero;
    int i = 0;
    for (; i < count - 3; i += 4) {
        __m128 s0 = _mm_load_ps(src0 + i);
        __m128 s1 = _mm_load_ps(src1 + i);
        sum = _mm_add_ps(_mm_mul_ps(s0, s1), sum);
    }
    __m128 mask = _mm_load_ps((float*)&SIMD_SP_indexedEndMask[count & 3]);
    __m128 s0 = _mm_and_ps(_mm_load_ps(src0 + i), mask);
    __m128 s1 = _mm_and_ps(_mm_load_ps(src1 + i), mask);
    sum = _mm_add_ps(_mm_mul_ps(s0, s1), sum);
    sum = _mm_add_ps(sum, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(1, 0, 3, 2)));
    sum = _mm_add_ps(sum, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 3, 0, 1)));
    float dot;
    _mm_store_ss(&dot, sum);
    return dot;

#elif defined(LCP_NEON)

    float32x4_t sum = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i + 4 <= count; i += 4) {
        sum = vmlaq_f32(sum, vld1q_f32(src0 + i), vld1q_f32(src1 + i));
    }
    float s0 = vgetq_lane_f32(sum, 0);
    float s1 = vgetq_lane_f32(sum, 1);
    float s2 = vgetq_lane_f32(sum, 2);
    float s3 = vgetq_lane_f32(sum, 3);
    switch (count - i) {
        NODEFAULT;

    case 4:
        s3 += src0[i + 3] * src1[i + 3];
    case 3:
        s2 += src0[i + 2] * src1[i + 2];
    case 2:
        s1 += src0[i + 1] * src1[i + 1];
    case 1:
        s0 += src0[i + 0] * src1[i + 0];
    case 0:
        break;
    }
    return s0 + s1 + s2 + s3;

#else

    // RB: the old loop caused completely broken rigid body physics and NaN errors
#if 1
    float s0 = 0.0f;
    float s1 = 0.0f;
    float s2 = 0.0f;
    float s3 = 0.0f;

    int i = 0;
    for (; i + 4 <= count; i += 4) {
        s0 += src0[i + 0] * src1[i + 0];
        s1 += src0[i + 1] * src1[i + 1];
        s2 += src0[i + 2] * src1[i + 2];
        s3 += src0[i + 3] * src1[i + 3];
    }

    switch (count - i) {
        NODEFAULT;

    case 4:
        s3 += src0[i + 3] * src1[i + 3];
    case 3:
        s2 += src0[i + 2] * src1[i + 2];
    case 2:
        s1 += src0[i + 1] * src1[i + 1];
    case 1:
        s0 += src0[i + 0] * src1[i + 0];
    case 0:
        break;
    }
    return s0 + s1 + s2 + s3;
#else

    float s = 0;
    for (int i = 0; i < count; i++) {
        s += src0[i] * src1[i];
    }

    return s;
#endif
    // RB end

#endif
}

/*
========================
LowerTriangularSolve_SIMD

Solves x in Lx = b for the n * n sub-matrix of L.
        * if skip > 0 the first skip elements of x are assumed to be valid already
        * L has to be a lower triangular matrix with (implicit) ones on the diagonal
        * x == b is allowed
========================
*/
static void LowerTriangularSolve_SIMD(const idMatX& L, float* x, const float* b, const int n, int skip)
{
    if (skip >= n) {
        return;
    }

    const float* lptr = L.ToFloatPtr();
    int nc = L.GetNumColumns();

    assert((nc & 3) == 0);

    // unrolled cases for n < 8
    if (n < 8) {
#define NSKIP(n, s) ((n << 3) | (s & 7))
        switch (NSKIP(n, skip)) {
        case NSKIP(1, 0):
            x[0] = b[0];
            return;
        case NSKIP(2, 0):
            x[0] = b[0];
        case NSKIP(2, 1):
            x[1] = b[1] - lptr[1 * nc + 0] * x[0];
            return;
        case NSKIP(3, 0):
            x[0] = b[0];
        case NSKIP(3, 1):
            x[1] = b[1] - lptr[1 * nc + 0] * x[0];
        case NSKIP(3, 2):
            x[2] = b[2] - lptr[2 * nc + 0] * x[0] - lptr[2 * nc + 1] * x[1];
            return;
        case NSKIP(4, 0):
            x[0] = b[0];
        case NSKIP(4, 1):
            x[1] = b[1] - lptr[1 * nc + 0] * x[0];
        case NSKIP(4, 2):
            x[2] = b[2] - lptr[2 * nc + 0] * x[0] - lptr[2 * nc + 1] * x[1];
        case NSKIP(4, 3):
            x[3] = b[3] - lptr[3 * nc + 0] * x[0] - lptr[3 * nc + 1] * x[1] - lptr[3 * nc + 2] * x[2];
            return;
        case NSKIP(5, 0):
            x[0] = b[0];
        case NSKIP(5, 1):
            x[1] = b[1] - lptr[1 * nc + 0] * x[0];
        case NSKIP(5, 2):
            x[2] = b[2] - lptr[2 * nc + 0] * x[0] - lptr[2 * nc + 1] * x[1];
        case NSKIP(5, 3):
            x[3] = b[3] - lptr[3 * nc + 0] * x[0] - lptr[3 * nc + 1] * x[1] - lptr[3 * nc + 2] * x[2];
        case NSKIP(5, 4):
            x[4] = b[4] - lptr[4 * nc + 0] * x[0] - lptr[4 * nc + 1] * x[1] - lptr[4 * nc + 2] * x[2] - lptr[4 * nc + 3] * x[3];
            return;
        case NSKIP(6, 0):
            x[0] = b[0];
        case NSKIP(6, 1):
            x[1] = b[1] - lptr[1 * nc + 0] * x[0];
        case NSKIP(6, 2):
            x[2] = b[2] - lptr[2 * nc + 0] * x[0] - lptr[2 * nc + 1] * x[1];
        case NSKIP(6, 3):
            x[3] = b[3] - lptr[3 * nc + 0] * x[0] - lptr[3 * nc + 1] * x[1] - lptr[3 * nc + 2] * x[2];
        case NSKIP(6, 4):
            x[4] = b[4] - lptr[4 * nc + 0] * x[0] - lptr[4 * nc + 1] * x[1] - lptr[4 * nc + 2] * x[2] - lptr[4 * nc + 3] * x[3];
        case NSKIP(6, 5):
            x[5] = b[5] - lptr[5 * nc + 0] * x[0] - lptr[5 * nc + 1] * x[1] - lptr[5 * nc + 2] * x[2] - lptr[5 * nc + 3] * x[3] - lptr[5 * nc + 4] * x[4];
            return;
        case NSKIP(7, 0):
            x[0] = b[0];
        case NSKIP(7, 1):
            x[1] = b[1] - lptr[1 * nc + 0] * x[0];
        case NSKIP(7, 2):
            x[2] = b[2] - lptr[2 * nc + 0] * x[0] - lptr[2 * nc + 1] * x[1];
        case NSKIP(7, 3):
            x[3] = b[3] - lptr[3 * nc + 0] * x[0] - lptr[3 * nc + 1] * x[1] - lptr[3 * nc + 2] * x[2];
        case NSKIP(7, 4):
            x[4] = b[4] - lptr[4 * nc + 0] * x[0] - lptr[4 * nc + 1] * x[1] - lptr[4 * nc + 2] * x[2] - lptr[4 * nc + 3] * x[3];
        case NSKIP(7, 5):
            x[5] = b[5] - lptr[5 * nc + 0] * x[0] - lptr[5 * nc + 1] * x[1] - lptr[5 * nc + 2] * x[2] - lptr[5 * nc + 3] * x[3] - lptr[5 * nc + 4] * x[4];
        case NSKIP(7, 6):
            x[6] = b[6] - lptr[6 * nc + 0] * x[0] - lptr[6 * nc + 1] * x[1] - lptr[6 * nc + 2] * x[2] - lptr[6 * nc + 3] * x[3] - lptr[6 * nc + 4] * x[4] - lptr[6 * nc + 5] * x[5];
            return;
        }
#undef NSKIP
        return;
    }

    // process first 4 rows
    switch (skip) {
    case 0:
        x[0] = b[0];
    case 1:
        x[1] = b[1] - lptr[1 * nc + 0] * x[0];
    case 2:
        x[2] = b[2] - lptr[2 * nc + 0] * x[0] - lptr[2 * nc + 1] * x[1];
    case 3:
        x[3] = b[3] - lptr[3 * nc + 0] * x[0] - lptr[3 * nc + 1] * x[1] - lptr[3 * nc + 2] * x[2];
        skip = 4;
    }

    lptr = L[skip];

    int i = skip;

#if defined(LCP_SIMD)

    // work up to a multiple of 4 rows
    for (; (i & 3) != 0 && i < n; i++) {
        __m128 sum = _mm_load_ss(&b[i]);
        int j = 0;
        for (; j < i - 3; j += 4) {
            __m128 s0 = _mm_load_ps(lptr + j);
            __m128 s1 = _mm_load_ps(x + j);
            sum = _mm_sub_ps(sum, _mm_mul_ps(s0, s1));
        }
        __m128 mask = _mm_load_ps((float*)&SIMD_SP_indexedEndMask[i & 3]);
        __m128 s0 = _mm_and_ps(_mm_load_ps(lptr + j), mask);
        __m128 s1 = _mm_and_ps(_mm_load_ps(x + j), mask);
        sum = _mm_sub_ps(sum, _mm_mul_ps(s0, s1));
        sum = _mm_add_ps(sum, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(1, 0, 3, 2)));
        sum = _mm_add_ps(sum, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 3, 0, 1)));
        _mm_store_ss(&x[i], sum);
        lptr += nc;
    }

    for (; i + 3 < n; i += 4) {
        const float* lptr0 = &lptr[0 * nc];
        const float* lptr1 = &lptr[1 * nc];
        const float* lptr2 = &lptr[2 * nc];
        const float* lptr3 = &lptr[3 * nc];

        assert_16_byte_aligned(lptr0);
        assert_16_byte_aligned(lptr1);
        assert_16_byte_aligned(lptr2);
        assert_16_byte_aligned(lptr3);

        __m128 va = _mm_load_ss(&b[i + 0]);
        __m128 vb = _mm_load_ss(&b[i + 1]);
        __m128 vc = _mm_load_ss(&b[i + 2]);
        __m128 vd = _mm_load_ss(&b[i + 3]);

        __m128 x0 = _mm_load_ps(&x[0]);

        va = _mm_sub_ps(va, _mm_mul_ps(x0, _mm_load_ps(lptr0 + 0)));
        vb = _mm_sub_ps(vb, _mm_mul_ps(x0, _mm_load_ps(lptr1 + 0)));
        vc = _mm_sub_ps(vc, _mm_mul_ps(x0, _mm_load_ps(lptr2 + 0)));
        vd = _mm_sub_ps(vd, _mm_mul_ps(x0, _mm_load_ps(lptr3 + 0)));

        for (int j = 4; j < i; j += 4) {
            __m128 xj = _mm_load_ps(&x[j]);

            va = _mm_sub_ps(va, _mm_mul_ps(xj, _mm_load_ps(lptr0 + j)));
            vb = _mm_sub_ps(vb, _mm_mul_ps(xj, _mm_load_ps(lptr1 + j)));
            vc = _mm_sub_ps(vc, _mm_mul_ps(xj, _mm_load_ps(lptr2 + j)));
            vd = _mm_sub_ps(vd, _mm_mul_ps(xj, _mm_load_ps(lptr3 + j)));
        }

        vb = _mm_sub_ps(vb, _mm_mul_ps(va, _mm_load1_ps(lptr1 + i + 0)));
        vc = _mm_sub_ps(vc, _mm_mul_ps(va, _mm_load1_ps(lptr2 + i + 0)));
        vc = _mm_sub_ps(vc, _mm_mul_ps(vb, _mm_load1_ps(lptr2 + i + 1)));
        vd = _mm_sub_ps(vd, _mm_mul_ps(va, _mm_load1_ps(lptr3 + i + 0)));
        vd = _mm_sub_ps(vd, _mm_mul_ps(vb, _mm_load1_ps(lptr3 + i + 1)));
        vd = _mm_sub_ps(vd, _mm_mul_ps(vc, _mm_load1_ps(lptr3 + i + 2)));

        __m128 ta = _mm_unpacklo_ps(va, vc); // x0, z0, x1, z1
        __m128 tb = _mm_unpackhi_ps(va, vc); // x2, z2, x3, z3
        __m128 tc = _mm_unpacklo_ps(vb, vd); // y0, w0, y1, w1
        __m128 td = _mm_unpackhi_ps(vb, vd); // y2, w2, y3, w3

        va = _mm_unpacklo_ps(ta, tc); // x0, y0, z0, w0
        vb = _mm_unpackhi_ps(ta, tc); // x1, y1, z1, w1
        vc = _mm_unpacklo_ps(tb, td); // x2, y2, z2, w2
        vd = _mm_unpackhi_ps(tb, td); // x3, y3, z3, w3

        va = _mm_add_ps(va, vb);
        vc = _mm_add_ps(vc, vd);
        va = _mm_add_ps(va, vc);

        _mm_store_ps(&x[i], va);

        lptr += 4 * nc;
    }

    // go through any remaining rows
    for (; i < n; i++) {
        __m128 sum = _mm_load_ss(&b[i]);
        int j = 0;
        for (; j < i - 3; j += 4) {
            __m128 s0 = _mm_load_ps(lptr + j);
            __m128 s1 = _mm_load_ps(x + j);
            sum = _mm_sub_ps(sum, _mm_mul_ps(s0, s1));
        }
        __m128 mask = _mm_load_ps((float*)&SIMD_SP_indexedEndMask[i & 3]);
        __m128 s0 = _mm_and_ps(_mm_load_ps(lptr + j), mask);
        __m128 s1 = _mm_and_ps(_mm_load_ps(x + j), mask);
        sum = _mm_sub_ps(sum, _mm_mul_ps(s0, s1));
        sum = _mm_add_ps(sum, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(1, 0, 3, 2)));
        sum = _mm_add_ps(sum, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 3, 0, 1)));
        _mm_store_ss(&x[i], sum);
        lptr += nc;
    }

#else

    // work up to a multiple of 4 rows
    for (; (i & 3) != 0 && i < n; i++) {
        float sum = b[i];
        for (int j = 0; j < i; j++) {
            sum -= lptr[j] * x[j];
        }
        x[i] = sum;
        lptr += nc;
    }

    assert_16_byte_aligned(x);

#if defined(LCP_NEON)
    for (; i + 3 < n; i += 4) {
        const float* lptr0 = &lptr[0 * nc];
        const float* lptr1 = &lptr[1 * nc];
        const float* lptr2 = &lptr[2 * nc];
        const float* lptr3 = &lptr[3 * nc];

        // a0 = -l[0] * x[0] + b[i], a1..a3 = -l[k] * x[k]: adding -0 to the
        // other lanes changes nothing, -0 included
        const float32x4_t negZero = vdupq_n_f32(-0.0f);
        const float32x4_t x0 = vld1q_f32(x);
        float32x4_t va = vaddq_f32(vnegq_f32(vmulq_f32(vld1q_f32(lptr0), x0)), vsetq_lane_f32(b[i + 0], negZero, 0));
        float32x4_t vb = vaddq_f32(vnegq_f32(vmulq_f32(vld1q_f32(lptr1), x0)), vsetq_lane_f32(b[i + 1], negZero, 0));
        float32x4_t vc = vaddq_f32(vnegq_f32(vmulq_f32(vld1q_f32(lptr2), x0)), vsetq_lane_f32(b[i + 2], negZero, 0));
        float32x4_t vd = vaddq_f32(vnegq_f32(vmulq_f32(vld1q_f32(lptr3), x0)), vsetq_lane_f32(b[i + 3], negZero, 0));

        for (int j = 4; j < i; j += 4) {
            const float32x4_t xj = vld1q_f32(x + j);
            va = vmlsq_f32(va, vld1q_f32(lptr0 + j), xj);
            vb = vmlsq_f32(vb, vld1q_f32(lptr1 + j), xj);
            vc = vmlsq_f32(vc, vld1q_f32(lptr2 + j), xj);
            vd = vmlsq_f32(vd, vld1q_f32(lptr3 + j), xj);
        }

        vb = vmlsq_n_f32(vb, va, lptr1[i + 0]);
        vc = vmlsq_n_f32(vc, va, lptr2[i + 0]);
        vc = vmlsq_n_f32(vc, vb, lptr2[i + 1]);
        vd = vmlsq_n_f32(vd, va, lptr3[i + 0]);
        vd = vmlsq_n_f32(vd, vb, lptr3[i + 1]);
        vd = vmlsq_n_f32(vd, vc, lptr3[i + 2]);

        x[i + 0] = vgetq_lane_f32(va, 0) + vgetq_lane_f32(va, 1) + vgetq_lane_f32(va, 2) + vgetq_lane_f32(va, 3);
        x[i + 1] = vgetq_lane_f32(vb, 0) + vgetq_lane_f32(vb, 1) + vgetq_lane_f32(vb, 2) + vgetq_lane_f32(vb, 3);
        x[i + 2] = vgetq_lane_f32(vc, 0) + vgetq_lane_f32(vc, 1) + vgetq_lane_f32(vc, 2) + vgetq_lane_f32(vc, 3);
        x[i + 3] = vgetq_lane_f32(vd, 0) + vgetq_lane_f32(vd, 1) + vgetq_lane_f32(vd, 2) + vgetq_lane_f32(vd, 3);

        lptr += 4 * nc;
    }
#else
    for (; i + 3 < n; i += 4) {
        const float* lptr0 = &lptr[0 * nc];
        const float* lptr1 = &lptr[1 * nc];
        const float* lptr2 = &lptr[2 * nc];
        const float* lptr3 = &lptr[3 * nc];

        assert_16_byte_aligned(lptr0);
        assert_16_byte_aligned(lptr1);
        assert_16_byte_aligned(lptr2);
        assert_16_byte_aligned(lptr3);

        float a0 = -lptr0[0] * x[0] + b[i + 0];
        float a1 = -lptr0[1] * x[1];
        float a2 = -lptr0[2] * x[2];
        float a3 = -lptr0[3] * x[3];

        float b0 = -lptr1[0] * x[0] + b[i + 1];
        float b1 = -lptr1[1] * x[1];
        float b2 = -lptr1[2] * x[2];
        float b3 = -lptr1[3] * x[3];

        float c0 = -lptr2[0] * x[0] + b[i + 2];
        float c1 = -lptr2[1] * x[1];
        float c2 = -lptr2[2] * x[2];
        float c3 = -lptr2[3] * x[3];

        float d0 = -lptr3[0] * x[0] + b[i + 3];
        float d1 = -lptr3[1] * x[1];
        float d2 = -lptr3[2] * x[2];
        float d3 = -lptr3[3] * x[3];

        for (int j = 4; j < i; j += 4) {
            a0 -= lptr0[j + 0] * x[j + 0];
            a1 -= lptr0[j + 1] * x[j + 1];
            a2 -= lptr0[j + 2] * x[j + 2];
            a3 -= lptr0[j + 3] * x[j + 3];

            b0 -= lptr1[j + 0] * x[j + 0];
            b1 -= lptr1[j + 1] * x[j + 1];
            b2 -= lptr1[j + 2] * x[j + 2];
            b3 -= lptr1[j + 3] * x[j + 3];

            c0 -= lptr2[j + 0] * x[j + 0];
            c1 -= lptr2[j + 1] * x[j + 1];
            c2 -= lptr2[j + 2] * x[j + 2];
            c3 -= lptr2[j + 3] * x[j + 3];

            d0 -= lptr3[j + 0] * x[j + 0];
            d1 -= lptr3[j + 1] * x[j + 1];
            d2 -= lptr3[j + 2] * x[j + 2];
            d3 -= lptr3[j + 3] * x[j + 3];
        }

        b0 -= lptr1[i + 0] * a0;
        b1 -= lptr1[i + 0] * a1;
        b2 -= lptr1[i + 0] * a2;
        b3 -= lptr1[i + 0] * a3;

        c0 -= lptr2[i + 0] * a0;
        c1 -= lptr2[i + 0] * a1;
        c2 -= lptr2[i + 0] * a2;
        c3 -= lptr2[i + 0] * a3;

        c0 -= lptr2[i + 1] * b0;
        c1 -= lptr2[i + 1] * b1;
        c2 -= lptr2[i + 1] * b2;
        c3 -= lptr2[i + 1] * b3;

        d0 -= lptr3[i + 0] * a0;
        d1 -= lptr3[i + 0] * a1;
        d2 -= lptr3[i + 0] * a2;
        d3 -= lptr3[i + 0] * a3;

        d0 -= lptr3[i + 1] * b0;
        d1 -= lptr3[i + 1] * b1;
        d2 -= lptr3[i + 1] * b2;
        d3 -= lptr3[i + 1] * b3;

        d0 -= lptr3[i + 2] * c0;
        d1 -= lptr3[i + 2] * c1;
        d2 -= lptr3[i + 2] * c2;
        d3 -= lptr3[i + 2] * c3;

        x[i + 0] = a0 + a1 + a2 + a3;
        x[i + 1] = b0 + b1 + b2 + b3;
        x[i + 2] = c0 + c1 + c2 + c3;
        x[i + 3] = d0 + d1 + d2 + d3;

        lptr += 4 * nc;
    }
#endif

    // go through any remaining rows
    for (; i < n; i++) {
        float sum = b[i];
        for (int j = 0; j < i; j++) {
            sum -= lptr[j] * x[j];
        }
        x[i] = sum;
        lptr += nc;
    }

#endif
}

/*
========================
LowerTriangularSolveTranspose_SIMD

Solves x in L'x = b for the n * n sub-matrix of L.
        * L has to be a lower triangular matrix with (implicit) ones on the diagonal
        * x == b is allowed
========================
*/
static void LowerTriangularSolveTranspose_SIMD(const idMatX& L, float* x, const float* b, const int n)
{
    int nc = L.GetNumColumns();

    assert((nc & 3) == 0);

    int m = n;
    int r = n & 3;

    if ((m & 3) != 0) {
        const float* lptr = L.ToFloatPtr() + m * nc + m;
        if ((m & 3) == 1) {
            x[m - 1] = b[m - 1];
            m -= 1;
        } else if ((m & 3) == 2) {
            x[m - 1] = b[m - 1];
            x[m - 2] = b[m - 2] - lptr[-1 * nc - 2] * x[m - 1];
            m -= 2;
        } else {
            x[m - 1] = b[m - 1];
            x[m - 2] = b[m - 2] - lptr[-1 * nc - 2] * x[m - 1];
            x[m - 3] = b[m - 3] - lptr[-1 * nc - 3] * x[m - 1] - lptr[-2 * nc - 3] * x[m - 2];
            m -= 3;
        }
    }

    const float* lptr = L.ToFloatPtr() + m * nc + m - 4;
    float* xptr = x + m;

#if defined(LCP_SIMD)

    // process 4 rows at a time
    for (int i = m; i >= 4; i -= 4) {
        assert_16_byte_aligned(b);
        assert_16_byte_aligned(xptr);
        assert_16_byte_aligned(lptr);

        __m128 s0 = _mm_load_ps(&b[i - 4]);
        __m128 s1 = (__m128&)SIMD_SP_zero;
        __m128 s2 = (__m128&)SIMD_SP_zero;
        __m128 s3 = (__m128&)SIMD_SP_zero;

        // process 4x4 blocks
        const float* xptr2 = xptr; // x + i;
        const float* lptr2 = lptr; // ptr = L[i] + i - 4;
        for (int j = i; j < m; j += 4) {
            __m128 xj = _mm_load_ps(xptr2);

            s0 = _mm_sub_ps(s0, _mm_mul_ps(_mm_splat_ps(xj, 0), _mm_load_ps(lptr2 + 0 * nc)));
            s1 = _mm_sub_ps(s1, _mm_mul_ps(_mm_splat_ps(xj, 1), _mm_load_ps(lptr2 + 1 * nc)));
            s2 = _mm_sub_ps(s2, _mm_mul_ps(_mm_splat_ps(xj, 2), _mm_load_ps(lptr2 + 2 * nc)));
            s3 = _mm_sub_ps(s3, _mm_mul_ps(_mm_splat_ps(xj, 3), _mm_load_ps(lptr2 + 3 * nc)));

            lptr2 += 4 * nc;
            xptr2 += 4;
        }
        for (int j = 0; j < r; j++) {
            s0 = _mm_sub_ps(s0, _mm_mul_ps(_mm_load_ps(lptr2), _mm_load1_ps(&xptr2[j])));
            lptr2 += nc;
        }
        s0 = _mm_add_ps(s0, s1);
        s2 = _mm_add_ps(s2, s3);
        s0 = _mm_add_ps(s0, s2);
        // process left over of the 4 rows
        lptr -= 4 * nc;
        __m128 t0 = _mm_and_ps(_mm_load_ps(lptr + 3 * nc), (__m128&)SIMD_SP_clearLast1);
        __m128 t1 = _mm_and_ps(_mm_load_ps(lptr + 2 * nc), (__m128&)SIMD_SP_clearLast2);
        __m128 t2 = _mm_load_ss(lptr + 1 * nc);
        s0 = _mm_sub_ps(s0, _mm_mul_ps(t0, _mm_splat_ps(s0, 3)));
        s0 = _mm_sub_ps(s0, _mm_mul_ps(t1, _mm_splat_ps(s0, 2)));
        s0 = _mm_sub_ps(s0, _mm_mul_ps(t2, _mm_splat_ps(s0, 1)));
        // store result
        _mm_store_ps(&xptr[-4], s0);
        // update pointers for next four rows
        lptr -= 4;
        xptr -= 4;
    }

#else

    // process 4 rows at a time
    for (int i = m; i >= 4; i -= 4) {
        assert_16_byte_aligned(b);
        assert_16_byte_aligned(xptr);
        assert_16_byte_aligned(lptr);

#if defined(LCP_NEON)
        // s0..s3 as the lanes of one register: s[k] -= lptr2[k] * t
        float32x4_t vs = vld1q_f32(&b[i - 4]);
        const float* xptr2 = xptr; // x + i;
        const float* lptr2 = lptr; // ptr = L[i] + i - 4;
        for (int j = i; j < m; j += 4) {
            vs = vmlsq_n_f32(vs, vld1q_f32(lptr2), xptr2[0]);
            lptr2 += nc;
            vs = vmlsq_n_f32(vs, vld1q_f32(lptr2), xptr2[1]);
            lptr2 += nc;
            vs = vmlsq_n_f32(vs, vld1q_f32(lptr2), xptr2[2]);
            lptr2 += nc;
            vs = vmlsq_n_f32(vs, vld1q_f32(lptr2), xptr2[3]);
            lptr2 += nc;
            xptr2 += 4;
        }
        for (int j = 0; j < r; j++) {
            vs = vmlsq_n_f32(vs, vld1q_f32(lptr2), xptr2[j]);
            lptr2 += nc;
        }
        float s0 = vgetq_lane_f32(vs, 0);
        float s1 = vgetq_lane_f32(vs, 1);
        float s2 = vgetq_lane_f32(vs, 2);
        float s3 = vgetq_lane_f32(vs, 3);
#else
        float s0 = b[i - 4];
        float s1 = b[i - 3];
        float s2 = b[i - 2];
        float s3 = b[i - 1];
        // process 4x4 blocks
        const float* xptr2 = xptr; // x + i;
        const float* lptr2 = lptr; // ptr = L[i] + i - 4;
        for (int j = i; j < m; j += 4) {
            float t0 = xptr2[0];
            s0 -= lptr2[0] * t0;
            s1 -= lptr2[1] * t0;
            s2 -= lptr2[2] * t0;
            s3 -= lptr2[3] * t0;
            lptr2 += nc;
            float t1 = xptr2[1];
            s0 -= lptr2[0] * t1;
            s1 -= lptr2[1] * t1;
            s2 -= lptr2[2] * t1;
            s3 -= lptr2[3] * t1;
            lptr2 += nc;
            float t2 = xptr2[2];
            s0 -= lptr2[0] * t2;
            s1 -= lptr2[1] * t2;
            s2 -= lptr2[2] * t2;
            s3 -= lptr2[3] * t2;
            lptr2 += nc;
            float t3 = xptr2[3];
            s0 -= lptr2[0] * t3;
            s1 -= lptr2[1] * t3;
            s2 -= lptr2[2] * t3;
            s3 -= lptr2[3] * t3;
            lptr2 += nc;
            xptr2 += 4;
        }
        for (int j = 0; j < r; j++) {
            float t = xptr2[j];
            s0 -= lptr2[0] * t;
            s1 -= lptr2[1] * t;
            s2 -= lptr2[2] * t;
            s3 -= lptr2[3] * t;
            lptr2 += nc;
        }
#endif
        // process left over of the 4 rows
        lptr -= nc;
        s0 -= lptr[0] * s3;
        s1 -= lptr[1] * s3;
        s2 -= lptr[2] * s3;
        lptr -= nc;
        s0 -= lptr[0] * s2;
        s1 -= lptr[1] * s2;
        lptr -= nc;
        s0 -= lptr[0] * s1;
        lptr -= nc;
        // store result
        xptr[-4] = s0;
        xptr[-3] = s1;
        xptr[-2] = s2;
        xptr[-1] = s3;
        // update pointers for next four rows
        lptr -= 4;
        xptr -= 4;
    }

#endif
}

