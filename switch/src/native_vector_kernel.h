#pragma once
#include "isa/ppc_isa_quantized.h"
#include <array>

// SDK matrix/vector operations in their original paired-single order. Inputs
// are bounded before entry, so products and sums cannot produce NaNs/overflow.
// Keep ps_sum0 and PSQ codecs: their NI flushing and unused FPR lanes remain
// caller-visible, even when they do not contribute to the output vector.
namespace NativeVector {
struct Inputs { std::array<double, 6> matrix; double xy; double z1; };
inline uint32_t ReadWord(const uint8_t* p) {
    uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v);
}
template<unsigned Bytes> inline bool Bounded(const uint8_t* p) {
    static_assert(Bytes == 48 || Bytes == 12);
#if defined(__aarch64__)
    if constexpr (Bytes == 48) {
        const auto a = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p)));
        const auto b = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16)));
        const auto c = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 32)));
        const auto mask = vdupq_n_u32(0x7fffffffu);
        return vmaxvq_u32(vmaxq_u32(vmaxq_u32(vandq_u32(a,mask),vandq_u32(b,mask)),vandq_u32(c,mask))) <= 0x4f800000u;
    } else {
        const auto a = vreinterpret_u32_u8(vrev32_u8(vld1_u8(p)));
        return std::max(vmaxv_u32(vand_u32(a,vdup_n_u32(0x7fffffffu))),ReadWord(p+8)&0x7fffffffu) <= 0x4f800000u;
    }
#else
    uint32_t maximum = 0;
    for (unsigned i = 0; i < Bytes; i += 4)
        maximum = std::max(maximum, ReadWord(p + i) & 0x7fffffffu);
    return maximum <= 0x4f800000u;
#endif
}
inline Inputs Load(const uint8_t* matrix, const uint8_t* vector) {
    Inputs in;
    for (unsigned i = 0; i < 6; ++i)
        in.matrix[i] = PpcLoadPairPsqFloatFromHostInline(matrix + i * 8);
    in.xy = PpcLoadPairPsqFloatFromHostInline(vector);
    in.z1 = PpcMakePairedResultInline(PpcBitCastToFloatInline(ReadWord(vector + 8)), 1.0f).d;
    return in;
}
inline double Mul(double a, double b) {
#if defined(__aarch64__)
    return PpcM128ToPsInline(vmul_f32(PpcPsToM128Inline(a), PpcPsToM128Inline(b)));
#else
    return PpcM128ToPsInline(_mm_mul_ps(PpcPsToM128Inline(a), PpcPsToM128Inline(b)));
#endif
}
inline double Madd(double a, double b, double c) {
#if defined(__aarch64__)
    return PpcM128ToPsInline(vfma_f32(PpcPsToM128Inline(c), PpcPsToM128Inline(a), PpcPsToM128Inline(b)));
#else
    return PpcM128ToPsInline(_mm_fmadd_ps(PpcPsToM128Inline(a), PpcPsToM128Inline(b), PpcPsToM128Inline(c)));
#endif
}
inline void StoreSingle(uint8_t* p, double pair) {
    const uint32_t bits = PpcConvertToSingleFTZInline(
        PpcBitCastToU64Inline(static_cast<double>(PpcGetPs0Inline(pair))));
    const uint32_t be = __builtin_bswap32(bits);
    std::memcpy(p, &be, 4);
}
template<bool RotationOnly>
inline void Transform(CpuContext* ctx, const Inputs& in, uint8_t* out) {
    if constexpr (RotationOnly) {
        ctx->fpr[0].d = in.matrix[0]; ctx->fpr[1].d = in.matrix[1];
        ctx->fpr[2].d = in.matrix[2]; ctx->fpr[3].d = in.matrix[3];
        ctx->fpr[4].d = in.matrix[4]; ctx->fpr[5].d = in.matrix[5];
        ctx->fpr[6].d = in.xy; ctx->fpr[7].d = in.z1;
        for (unsigned row = 0; row < 3; ++row) {
            const double product = Mul(in.matrix[row * 2], in.xy);
            const double sum = PPC_PsSum0Inline(product, product, product);
            const double result = Madd(in.matrix[row * 2 + 1], in.z1, sum);
            ctx->fpr[8 + row * 2].d = sum;
            ctx->fpr[9 + row * 2].d = result;
            StoreSingle(out + row * 4, result);
        }
    } else {
        const double p0 = Mul(in.matrix[0], in.xy);
        const double r0 = Madd(in.matrix[1], in.z1, p0);
        const double s0 = PPC_PsSum0Inline(r0, r0, PPC_PsFromScalarInline(ctx->fpr[6].d));
        const double p1 = Mul(in.matrix[2], in.xy);
        const double r1 = Madd(in.matrix[3], in.z1, p1);
        const double s1 = PPC_PsSum0Inline(r1, r1, PPC_PsFromScalarInline(ctx->fpr[12].d));
        const double p2 = Mul(in.matrix[4], in.xy);
        const double r2 = Madd(in.matrix[5], in.z1, p2);
        const double s2 = PPC_PsSum0Inline(r2, r2, s0);
        ctx->fpr[0].d = in.xy; ctx->fpr[1].d = in.z1;
        ctx->fpr[2].d = in.matrix[4]; ctx->fpr[3].d = in.matrix[5];
        ctx->fpr[4].d = p2; ctx->fpr[5].d = r2; ctx->fpr[6].d = s2;
        ctx->fpr[8].d = in.matrix[2]; ctx->fpr[9].d = in.matrix[3];
        ctx->fpr[10].d = p1; ctx->fpr[11].d = r1; ctx->fpr[12].d = s1;
        StoreSingle(out, s0); StoreSingle(out + 4, s1); StoreSingle(out + 8, s2);
    }
}
} // namespace NativeVector
