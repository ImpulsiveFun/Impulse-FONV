#pragma once
#include "rt.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>

#define RT_PACKED_BINARY(name, vector_op, scalar_op)                                \
static inline void name(uint8_t *dst, const uint8_t *src) {                          \
    float32x4_t left, right;                                                        \
    memcpy(&left, dst, sizeof left);                                                \
    memcpy(&right, src, sizeof right);                                              \
    float32x4_t result = vector_op(left, right);                                     \
    if (vminvq_u32(vceqq_f32(result, result))) {                                     \
        memcpy(dst, &result, sizeof result);                                        \
        return;                                                                    \
    }                                                                              \
    /* Preserve the translator's NaN payload and invalid-result convention. */    \
    for (int lane = 0; lane < 16; lane += 4)                                        \
        STV(dst + lane, FBINV(scalar_op, LDV(dst + lane, 4), 4,                       \
                             LDV(src + lane, 4), 4, 4), 4);                         \
}
#else
#define RT_PACKED_BINARY(name, vector_op, scalar_op)                                \
static inline void name(uint8_t *dst, const uint8_t *src) {                          \
    for (int lane = 0; lane < 16; lane += 4)                                        \
        STV(dst + lane, FBINV(scalar_op, LDV(dst + lane, 4), 4,                       \
                             LDV(src + lane, 4), 4, 4), 4);                         \
}
#endif

RT_PACKED_BINARY(rt_packed_add, vaddq_f32, +)
RT_PACKED_BINARY(rt_packed_sub, vsubq_f32, -)
RT_PACKED_BINARY(rt_packed_mul, vmulq_f32, *)
#undef RT_PACKED_BINARY

#if defined(__clang__)
typedef uint32_t RtPackedBits __attribute__((vector_size(16)));
#define rt_packed_shuffle(dst, src, imm) do {                                      \
    RtPackedBits rt_shuffle_left, rt_shuffle_right, rt_shuffle_result;             \
    memcpy(&rt_shuffle_left, (dst), sizeof rt_shuffle_left);                        \
    memcpy(&rt_shuffle_right, (src), sizeof rt_shuffle_right);                      \
    rt_shuffle_result = __builtin_shufflevector(rt_shuffle_left, rt_shuffle_right,  \
        (imm) & 3, ((imm) >> 2) & 3, 4 + (((imm) >> 4) & 3), 4 + ((imm) >> 6));      \
    memcpy((dst), &rt_shuffle_result, sizeof rt_shuffle_result);                    \
} while (0)
#else
static inline void rt_packed_shuffle(uint8_t *dst, const uint8_t *src, unsigned imm) {
    uint32_t left[4], right[4];
    memcpy(left, dst, sizeof left);
    memcpy(right, src, sizeof right);
    uint32_t result[] = {left[imm & 3], left[(imm >> 2) & 3],
                         right[(imm >> 4) & 3], right[imm >> 6]};
    memcpy(dst, result, sizeof result);
}
#endif
