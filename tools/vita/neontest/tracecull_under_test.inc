#if defined(__vita__) && defined(__ARM_NEON__)
/*
====================
Vita_TraceCullBitsPositions_NEON

R_LocalTracePositions' per-vertex sidedness bits, four vertices at a time:
the same distances (a*x + b*y + c*z + d, in that order), the same sign bits,
so the same bytes (tools/vita/neontest).
====================
*/
static void Vita_TraceCullBitsPositions_NEON(byte* cullBits, byte& totalOr, const float radius, const float* planes, const float* xyz, const int numVerts)
{
    const float32x4_t r = vdupq_n_f32(radius);
    // the four planes loaded once and multiplied by lane (as float scalars GCC
    // re-materialized them inside the loop); the same multiply-adds
    const float32x4_t pl[4] = { vld1q_f32(planes + 0), vld1q_f32(planes + 4), vld1q_f32(planes + 8), vld1q_f32(planes + 12) };
    uint32x4_t total = vdupq_n_u32(0);
    int i = 0;
    for (; i + 4 <= numVerts; i += 4) {
        const float32x4x3_t v = vld3q_f32(xyz + i * 3); // x, y, z of four vertices
        uint32x4_t bits = vdupq_n_u32(0);
        for (int k = 0; k < 4; k++) {
            const float32x2_t p01 = vget_low_f32(pl[k]);
            const float32x2_t p23 = vget_high_f32(pl[k]);
            const float32x4_t d = vaddq_f32(vmlaq_lane_f32(vmlaq_lane_f32(vmulq_lane_f32(v.val[0], p01, 0), v.val[1], p01, 1), v.val[2], p23, 0), vdupq_lane_f32(p23, 1));
            const uint32x4_t tSign = vshrq_n_u32(vreinterpretq_u32_f32(vaddq_f32(d, r)), 31);
            const uint32x4_t sSign = vshrq_n_u32(vreinterpretq_u32_f32(vsubq_f32(d, r)), 31);
            bits = vorrq_u32(bits, vshlq_u32(tSign, vdupq_n_s32(k)));
            bits = vorrq_u32(bits, vshlq_u32(sSign, vdupq_n_s32(k + 4)));
        }
        bits = veorq_u32(bits, vdupq_n_u32(0x0F)); // flip lower four bits
        total = vorrq_u32(total, bits);
        const uint16x4_t b16 = vmovn_u32(bits);
        const uint8x8_t b8 = vmovn_u16(vcombine_u16(b16, b16));
        vst1_lane_u32((uint32_t*)(cullBits + i), vreinterpret_u32_u8(b8), 0);
    }
    const uint32x2_t t2 = vorr_u32(vget_low_u32(total), vget_high_u32(total));
    byte tOr = (byte)(vget_lane_u32(t2, 0) | vget_lane_u32(t2, 1));
    for (; i < numVerts; i++) {
        const float* vp = xyz + i * 3;
        byte bits = 0;
        for (int k = 0; k < 4; k++) {
            const float* p = planes + k * 4;
            const float d = p[0] * vp[0] + p[1] * vp[1] + p[2] * vp[2] + p[3];
            const float tt = d + radius;
            const float s = d - radius;
            bits |= (byte)(IEEE_FLT_SIGNBITSET(tt) << k);
            bits |= (byte)(IEEE_FLT_SIGNBITSET(s) << (k + 4));
        }
        bits ^= 0x0F;
        tOr |= bits;
        cullBits[i] = bits;
    }
    totalOr = tOr;
}
#endif
