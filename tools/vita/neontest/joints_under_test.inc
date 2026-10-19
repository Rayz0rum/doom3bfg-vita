    // Every animated entity's skeleton, every frame it is built (idAnimator::
    // CreateFrame, ~140 times a frame in a fight). idJointMat::operator*= in
    // NEON, a row at a time: lane c of new row r is m[0][c] * a[r][0] +
    // m[1][c] * a[r][1] + m[2][c] * a[r][2], in that order, as the scalar
    // code sums it (VMLA rounds the product before the add, as it does), and
    // the translation lane then gets a[r][3]; the other lanes get -0, which
    // changes nothing, signed zeros included (tools/vita/neontest).
    for (int i = firstJoint; i <= lastJoint; i++) {
        assert(parents[i] < i);
        float* m = jointMats[i].ToFloatPtr();
        const float* a = jointMats[parents[i]].ToFloatPtr();
        const float32x4_t m0 = vld1q_f32(m + 0);
        const float32x4_t m1 = vld1q_f32(m + 4);
        const float32x4_t m2 = vld1q_f32(m + 8);
        const float32x4_t negZero = vdupq_n_f32(-0.0f);
        // the parent's rows loaded as vectors and multiplied by lane (as float
        // scalars, GCC built them through the stack every joint); the
        // translation is the parent row's lane 3 over -0 elsewhere, selected
        // bit for bit. The same arithmetic.
        const uint32x4_t lane3 = vcombine_u32(vdup_n_u32(0), vcreate_u32(0xFFFFFFFF00000000ULL));
        for (int r = 0; r < 3; r++) {
            const float32x4_t ar = vld1q_f32(a + r * 4);
            const float32x2_t a01 = vget_low_f32(ar);
            const float32x2_t a23 = vget_high_f32(ar);
            float32x4_t row = vmulq_lane_f32(m0, a01, 0);
            row = vmlaq_lane_f32(row, m1, a01, 1);
            row = vmlaq_lane_f32(row, m2, a23, 0);
            row = vaddq_f32(row, vbslq_f32(lane3, ar, negZero));
            vst1q_f32(m + r * 4, row);
        }
    }
