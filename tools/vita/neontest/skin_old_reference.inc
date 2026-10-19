// the first Vita NEON skinning (before the 8-byte normal/tangent store), kept as the reference
static void Old_TransformVertsAndTangents_NEON(idDrawVert* targetVerts, const int numVerts, const idDrawVert* baseVerts, const idJointMat* joints)
{
    const float32x4_t zero = vdupq_n_f32(0.0f);
    const float32x4_t one = vdupq_n_f32(1.0f);
    const float32x4_t half = vdupq_n_f32(0.5f);
    const float32x4_t byteScale = vdupq_n_f32(2.0f / 255.0f);
    const float32x4_t minusOne = vdupq_n_f32(-1.0f);
    const uint32x4_t maxByte = vdupq_n_u32(255);

    for (int i = 0; i < numVerts; i++) {
        const idDrawVert& base = baseVerts[i];
        idDrawVert& out = targetVerts[i];

        const float* j0 = joints[base.color[0]].ToFloatPtr();
        const float* j1 = joints[base.color[1]].ToFloatPtr();
        const float* j2 = joints[base.color[2]].ToFloatPtr();
        const float* j3 = joints[base.color[3]].ToFloatPtr();
        const float w0 = base.color2[0] * (1.0f / 255.0f);
        const float w1 = base.color2[1] * (1.0f / 255.0f);
        const float w2 = base.color2[2] * (1.0f / 255.0f);
        const float w3 = base.color2[3] * (1.0f / 255.0f);
        float32x4_t r[3];
        for (int row = 0; row < 3; row++) {
            float32x4_t m = vmulq_n_f32(vld1q_f32(j0 + row * 4), w0);
            m = vmlaq_n_f32(m, vld1q_f32(j1 + row * 4), w1);
            m = vmlaq_n_f32(m, vld1q_f32(j2 + row * 4), w2);
            r[row] = vmlaq_n_f32(m, vld1q_f32(j3 + row * 4), w3);
        }
        // rows -> columns (the fourth row is zero)
        const float32x4x2_t t01 = vtrnq_f32(r[0], r[1]);
        const float32x4x2_t t2z = vtrnq_f32(r[2], zero);
        const float32x4_t c0 = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t2z.val[0]));
        const float32x4_t c1 = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t2z.val[1]));
        const float32x4_t c2 = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t2z.val[0]));
        const float32x4_t c3 = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t2z.val[1])); // translation

        const float32x4_t pos = vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(c3, c0, base.xyz.x), c1, base.xyz.y), c2, base.xyz.z);
        vst1_f32(&out.xyz.x, vget_low_f32(pos));
        out.xyz.z = vgetq_lane_f32(pos, 2);

        for (int v = 0; v < 2; v++) {
            const byte* src = (v == 0) ? base.normal : base.tangent;
            byte* dst = (v == 0) ? out.normal : out.tangent;
            // unpack (b * 2/255 - 1), the fourth lane is ignored
            uint32_t packed;
            memcpy(&packed, src, 4);
            const float32x4_t f = vmlaq_f32(minusOne, vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vcreate_u8(packed))))), byteScale);
            // renormalize
            const float32x4_t sq = vmulq_f32(f, f);
            const float32x2_t len2 = vadd_f32(vpadd_f32(vget_low_f32(sq), vget_low_f32(sq)), vget_high_f32(sq)); // lane 0: x2 + y2 + z2
            float32x2_t inv = vrsqrte_f32(len2);
            inv = vmul_f32(inv, vrsqrts_f32(vmul_f32(len2, inv), inv));
            inv = vmul_f32(inv, vrsqrts_f32(vmul_f32(len2, inv), inv));
            const float32x4_t n = vmulq_lane_f32(f, inv, 0);
            // rotate
            float32x4_t t = vmulq_lane_f32(c0, vget_low_f32(n), 0);
            t = vmlaq_lane_f32(t, c1, vget_low_f32(n), 1);
            t = vmlaq_lane_f32(t, c2, vget_high_f32(n), 0);
            // pack: Ftob( ( x + 1 ) * 127.5 + 0.5 ), truncating, clamped to 0..255
            const uint32x4_t u = vminq_u32(vcvtq_u32_f32(vmlaq_n_f32(half, vaddq_f32(t, one), 255.0f / 2.0f)), maxByte);
            dst[0] = (byte)vgetq_lane_u32(u, 0);
            dst[1] = (byte)vgetq_lane_u32(u, 1);
            dst[2] = (byte)vgetq_lane_u32(u, 2);
        }
        out.tangent[3] = base.tangent[3];
    }
}
