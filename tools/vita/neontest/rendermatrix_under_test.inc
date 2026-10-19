// ---- Vita NEON render matrix kernels (tools/vita/neontest cuts this block) ----
/*
========================
Vita_MultiplyRenderMatrix_NEON / Vita_CullBoundsBits_NEON

idRenderMatrix::Multiply and CullBoundsToMVPbits on the Vita, which takes the
generic paths (no SSE): out row i is a[i][0] * b row 0 + a[i][1] * b row 1 +
a[i][2] * b row 2 + a[i][3] * b row 3, and a box corner's clip coordinate i
is x * m[i][0] + y * m[i][1] + z * m[i][2] + m[i][3] - both summed in the
generic code's order, four lanes at a time (VMLA rounds the product before
the add, as the scalar code does), the corners four to a vector, the same
comparisons for the same bits. NEON flushes denormals to zero, so a value
below 1.2e-38 can compare differently, nothing else. The cull runs per
surface, per entity and light, per light and entity in the light jobs -
thousands of times a frame, ~200 multiply-adds each in scalar code.
========================
*/
static void Vita_MultiplyRenderMatrix_NEON(const float* a, const float* b, float* out)
{
    const float32x4_t b0 = vld1q_f32(b + 0);
    const float32x4_t b1 = vld1q_f32(b + 4);
    const float32x4_t b2 = vld1q_f32(b + 8);
    const float32x4_t b3 = vld1q_f32(b + 12);
    for (int i = 0; i < 4; i++) {
        const float32x4_t ar = vld1q_f32(a + i * 4);
        const float32x2_t a01 = vget_low_f32(ar);
        const float32x2_t a23 = vget_high_f32(ar);
        float32x4_t row = vmulq_lane_f32(b0, a01, 0);
        row = vmlaq_lane_f32(row, b1, a01, 1);
        row = vmlaq_lane_f32(row, b2, a23, 0);
        row = vmlaq_lane_f32(row, b3, a23, 1);
        vst1q_f32(out + i * 4, row);
    }
}

static inline float32x4_t Vita_ClipRow(const float32x4_t X, const float32x4_t Y, const float32x4_t Z, const float32x4_t r)
{
    const float32x2_t r01 = vget_low_f32(r);
    const float32x2_t r23 = vget_high_f32(r);
    return vaddq_f32(vmlaq_lane_f32(vmlaq_lane_f32(vmulq_lane_f32(X, r01, 0), Y, r01, 1), Z, r23, 0), vdupq_lane_f32(r23, 1));
}

// bounds: min x, y, z then max x, y, z; returns the generic code's 'bits'
static int Vita_CullBoundsBits_NEON(const float* mvp, const float* bounds, bool zeroToOne)
{
    const float xs[4] = { bounds[0], bounds[3], bounds[0], bounds[3] };
    const float ys[4] = { bounds[1], bounds[1], bounds[4], bounds[4] };
    const float32x4_t X = vld1q_f32(xs);
    const float32x4_t Y = vld1q_f32(ys);
    const float32x4_t r0 = vld1q_f32(mvp + 0);
    const float32x4_t r1 = vld1q_f32(mvp + 4);
    const float32x4_t r2 = vld1q_f32(mvp + 8);
    const float32x4_t r3 = vld1q_f32(mvp + 12);
    const float32x4_t zero = vdupq_n_f32(0.0f);
    uint32x4_t bits = vdupq_n_u32(0);
    for (int k = 0; k < 2; k++) {
        const float32x4_t Z = vdupq_n_f32(bounds[2 + k * 3]);
        const float32x4_t c0 = Vita_ClipRow(X, Y, Z, r0);
        const float32x4_t c1 = Vita_ClipRow(X, Y, Z, r1);
        const float32x4_t c2 = Vita_ClipRow(X, Y, Z, r2);
        const float32x4_t c3 = Vita_ClipRow(X, Y, Z, r3);
        const float32x4_t minW = zeroToOne ? zero : vnegq_f32(c3);
        bits = vorrq_u32(bits, vandq_u32(vcgtq_f32(c0, minW), vdupq_n_u32(1 << 0)));
        bits = vorrq_u32(bits, vandq_u32(vcltq_f32(c0, c3), vdupq_n_u32(1 << 1)));
        bits = vorrq_u32(bits, vandq_u32(vcgtq_f32(c1, minW), vdupq_n_u32(1 << 2)));
        bits = vorrq_u32(bits, vandq_u32(vcltq_f32(c1, c3), vdupq_n_u32(1 << 3)));
        bits = vorrq_u32(bits, vandq_u32(vcgtq_f32(c2, minW), vdupq_n_u32(1 << 4)));
        bits = vorrq_u32(bits, vandq_u32(vcltq_f32(c2, c3), vdupq_n_u32(1 << 5)));
    }
    const uint32x2_t b2 = vorr_u32(vget_low_u32(bits), vget_high_u32(bits));
    return (int)(vget_lane_u32(b2, 0) | vget_lane_u32(b2, 1));
}
