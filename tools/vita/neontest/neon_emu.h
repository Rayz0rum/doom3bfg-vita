// Scalar stand-ins for the NEON intrinsics the Vita shadow code uses, to
// check its lane logic on a PC against the generic C code.
#pragma once
#include <cstdint>
#include <cstring>

struct float32x4_t { float v[4]; };
struct float32x2_t { float v[2]; };
struct uint32x4_t { uint32_t v[4]; };
struct uint32x2_t { uint32_t v[2]; };
struct uint16x4_t { uint16_t v[4]; };
struct uint16x8_t { uint16_t v[8]; };
struct uint8x8_t { uint8_t v[8]; };
struct float32x4x2_t { float32x4_t val[2]; };

static inline float32x4_t vld1q_f32(const float* p) { float32x4_t r; memcpy(r.v, p, 16); return r; }
static inline void vst1q_f32(float* p, float32x4_t a) { memcpy(p, a.v, 16); }
static inline float32x4_t vdupq_n_f32(float f) { return { { f, f, f, f } }; }
static inline float32x2_t vdup_n_f32(float f) { return { { f, f } }; }
static inline uint32x4_t vdupq_n_u32(uint32_t f) { return { { f, f, f, f } }; }
static inline uint32x4_t vld1q_u32(const uint32_t* p) { uint32x4_t r; memcpy(r.v, p, 16); return r; }
static inline float32x4x2_t vtrnq_f32(float32x4_t a, float32x4_t b)
{
    float32x4x2_t r;
    r.val[0] = { { a.v[0], b.v[0], a.v[2], b.v[2] } };
    r.val[1] = { { a.v[1], b.v[1], a.v[3], b.v[3] } };
    return r;
}
static inline float32x2_t vget_low_f32(float32x4_t a) { return { { a.v[0], a.v[1] } }; }
static inline float32x2_t vget_high_f32(float32x4_t a) { return { { a.v[2], a.v[3] } }; }
static inline uint32x2_t vget_low_u32(uint32x4_t a) { return { { a.v[0], a.v[1] } }; }
static inline uint32x2_t vget_high_u32(uint32x4_t a) { return { { a.v[2], a.v[3] } }; }
static inline float32x4_t vcombine_f32(float32x2_t a, float32x2_t b) { return { { a.v[0], a.v[1], b.v[0], b.v[1] } }; }
static inline uint16x8_t vcombine_u16(uint16x4_t a, uint16x4_t b) { return { { a.v[0], a.v[1], a.v[2], a.v[3], b.v[0], b.v[1], b.v[2], b.v[3] } }; }
#define BIN4(name, op) static inline float32x4_t name(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] op b.v[i]; return r; }
BIN4(vsubq_f32, -)
BIN4(vmulq_f32, *)
static inline float32x4_t vmlaq_f32(float32x4_t a, float32x4_t b, float32x4_t c) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] + b.v[i] * c.v[i]; return r; }
static inline float32x4_t vmlsq_f32(float32x4_t a, float32x4_t b, float32x4_t c) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] - b.v[i] * c.v[i]; return r; }
static inline float32x4_t vmulq_n_f32(float32x4_t a, float s) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] * s; return r; }
static inline float32x4_t vmlaq_n_f32(float32x4_t a, float32x4_t b, float s) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] + b.v[i] * s; return r; }
static inline uint32x4_t vcgtq_f32(float32x4_t a, float32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] > b.v[i] ? 0xFFFFFFFFu : 0u; return r; }
static inline uint32x4_t vcltq_f32(float32x4_t a, float32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? 0xFFFFFFFFu : 0u; return r; }
static inline uint32x4_t vorrq_u32(uint32x4_t a, uint32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] | b.v[i]; return r; }
static inline uint32x4_t vandq_u32(uint32x4_t a, uint32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] & b.v[i]; return r; }
static inline uint32x4_t vaddq_u32(uint32x4_t a, uint32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] + b.v[i]; return r; }
static inline uint32x4_t vmvnq_u32(uint32x4_t a) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = ~a.v[i]; return r; }
static inline uint16x4_t vmovn_u32(uint32x4_t a) { uint16x4_t r; for (int i = 0; i < 4; i++) r.v[i] = (uint16_t)a.v[i]; return r; }
static inline uint8x8_t vmovn_u16(uint16x8_t a) { uint8x8_t r; for (int i = 0; i < 8; i++) r.v[i] = (uint8_t)a.v[i]; return r; }
static inline uint32x2_t vreinterpret_u32_u8(uint8x8_t a) { uint32x2_t r; memcpy(r.v, a.v, 8); return r; }
static inline void vst1_lane_u32(uint32_t* p, uint32x2_t a, int lane) { *p = a.v[lane]; }
static inline uint32x2_t vadd_u32(uint32x2_t a, uint32x2_t b) { return { { a.v[0] + b.v[0], a.v[1] + b.v[1] } }; }
static inline uint32x2_t vpadd_u32(uint32x2_t a, uint32x2_t b) { return { { a.v[0] + a.v[1], b.v[0] + b.v[1] } }; }
static inline uint32_t vget_lane_u32(uint32x2_t a, int lane) { return a.v[lane]; }
static inline float32x4_t vsetq_lane_f32(float f, float32x4_t a, int lane) { a.v[lane] = f; return a; }
static inline float32x2_t vadd_f32(float32x2_t a, float32x2_t b) { return { { a.v[0] + b.v[0], a.v[1] + b.v[1] } }; }
static inline float32x2_t vpadd_f32(float32x2_t a, float32x2_t b) { return { { a.v[0] + a.v[1], b.v[0] + b.v[1] } }; }
static inline float32x2_t vext_f32(float32x2_t a, float32x2_t b, int n) { float t[4] = { a.v[0], a.v[1], b.v[0], b.v[1] }; return { { t[n], t[n + 1] } }; }

// --- for the skinning test
#include <cmath>
static inline uint8x8_t vcreate_u8(uint64_t x) { uint8x8_t r; memcpy(r.v, &x, 8); return r; }
static inline uint16x8_t vmovl_u8(uint8x8_t a) { uint16x8_t r; for (int i = 0; i < 8; i++) r.v[i] = a.v[i]; return r; }
static inline uint16x4_t vget_low_u16(uint16x8_t a) { return { { a.v[0], a.v[1], a.v[2], a.v[3] } }; }
static inline uint32x4_t vmovl_u16(uint16x4_t a) { return { { a.v[0], a.v[1], a.v[2], a.v[3] } }; }
static inline float32x4_t vcvtq_f32_u32(uint32x4_t a) { return { { (float)a.v[0], (float)a.v[1], (float)a.v[2], (float)a.v[3] } }; }
static inline uint32_t emu_cvt_u32(float f) { if (!(f > 0.0f)) return 0; if (f >= 4294967296.0f) return 0xFFFFFFFFu; return (uint32_t)f; }
static inline uint32x4_t vcvtq_u32_f32(float32x4_t a) { return { { emu_cvt_u32(a.v[0]), emu_cvt_u32(a.v[1]), emu_cvt_u32(a.v[2]), emu_cvt_u32(a.v[3]) } }; }
static inline uint32x4_t vminq_u32(uint32x4_t a, uint32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; return r; }
static inline float32x4_t vaddq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] + b.v[i]; return r; }
static inline float32x2_t vmul_f32(float32x2_t a, float32x2_t b) { return { { a.v[0] * b.v[0], a.v[1] * b.v[1] } }; }
// the estimate: ~8 bits, like the hardware's
static inline float emu_rsqrte(float x) { float e = 1.0f / sqrtf(x); int ex; float m = frexpf(e, &ex); m = floorf(m * 512.0f) / 512.0f; return ldexpf(m, ex); }
static inline float32x2_t vrsqrte_f32(float32x2_t a) { return { { emu_rsqrte(a.v[0]), emu_rsqrte(a.v[1]) } }; }
static inline float32x2_t vrsqrts_f32(float32x2_t a, float32x2_t b) { return { { (3.0f - a.v[0] * b.v[0]) * 0.5f, (3.0f - a.v[1] * b.v[1]) * 0.5f } }; }
static inline float32x4_t vnegq_f32(float32x4_t a) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = -a.v[i]; return r; }
static inline float32x4_t vdupq_lane_f32(float32x2_t a, int l) { float32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[l]; return r; }
static inline float32x4_t vmulq_lane_f32(float32x4_t a, float32x2_t b, int l) { return vmulq_n_f32(a, b.v[l]); }
static inline float32x4_t vmlaq_lane_f32(float32x4_t a, float32x4_t b, float32x2_t c, int l) { return vmlaq_n_f32(a, b, c.v[l]); }
static inline void vst1_f32(float* p, float32x2_t a) { p[0] = a.v[0]; p[1] = a.v[1]; }
static inline float vgetq_lane_f32(float32x4_t a, int l) { return a.v[l]; }
static inline uint32_t vgetq_lane_u32(uint32x4_t a, int l) { return a.v[l]; }
// for Vita_ShadowVolumeCullBits_NEON (ShadowShared.cpp)
struct int32x4_t { int32_t v[4]; };
struct float32x4x4_t { float32x4_t val[4]; };
static inline float32x4x4_t vld4q_f32(const float* p) { float32x4x4_t r; for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) r.val[k].v[i] = p[i * 4 + k]; return r; }
static inline uint32x4_t vreinterpretq_u32_f32(float32x4_t a) { uint32x4_t r; memcpy(r.v, a.v, 16); return r; }
static inline uint32x4_t vshrq_n_u32(uint32x4_t a, int n) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] >> n; return r; }
static inline int32x4_t vdupq_n_s32(int32_t f) { return { { f, f, f, f } }; }
static inline uint32x4_t vshlq_u32(uint32x4_t a, int32x4_t s) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = s.v[i] >= 0 ? (a.v[i] << s.v[i]) : (a.v[i] >> -s.v[i]); return r; }
static inline uint32x4_t veorq_u32(uint32x4_t a, uint32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] ^ b.v[i]; return r; }
static inline uint32x2_t vorr_u32(uint32x2_t a, uint32x2_t b) { return { { a.v[0] | b.v[0], a.v[1] | b.v[1] } }; }
// for the second skinning version (Model_md5.cpp)
struct float32x2x2_t { float32x2_t val[2]; };
static inline uint8x8_t vld1_u8(const uint8_t* p) { uint8x8_t r; memcpy(r.v, p, 8); return r; }
static inline void vst1_u8(uint8_t* p, uint8x8_t a) { memcpy(p, a.v, 8); }
static inline uint16x4_t vget_high_u16(uint16x8_t a) { return { { a.v[4], a.v[5], a.v[6], a.v[7] } }; }
static inline float32x2x2_t vtrn_f32(float32x2_t a, float32x2_t b) { float32x2x2_t r; r.val[0] = { { a.v[0], b.v[0] } }; r.val[1] = { { a.v[1], b.v[1] } }; return r; }
static inline uint32x2_t vdup_n_u32(uint32_t x) { return { { x, x } }; }
static inline uint32x2_t vcreate_u32(uint64_t x) { uint32x2_t r; memcpy(r.v, &x, 8); return r; }
static inline uint32x4_t vcombine_u32(uint32x2_t a, uint32x2_t b) { return { { a.v[0], a.v[1], b.v[0], b.v[1] } }; }
static inline float32x4_t vbslq_f32(uint32x4_t m, float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) { uint32_t x, y; memcpy(&x, &a.v[i], 4); memcpy(&y, &b.v[i], 4); x = (m.v[i] & x) | (~m.v[i] & y); memcpy(&r.v[i], &x, 4); } return r; }
static inline uint8x8_t vbsl_u8(uint8x8_t m, uint8x8_t a, uint8x8_t b) { uint8x8_t r; for (int i = 0; i < 8; i++) r.v[i] = (m.v[i] & a.v[i]) | (~m.v[i] & b.v[i]); return r; }
static inline void vst1q_lane_f32(float* p, float32x4_t a, int lane) { *p = a.v[lane]; }
