#pragma once
#include "isa/ppc_isa_quantized.h"

// Three four-lane rows, in guest matrix order rather than packed FPR order.
// The caller proves mapped ranges and finite inputs with magnitude <= 2^32.
// That bound keeps every product/sum finite, so the rare PPC NaN repair path
// is unnecessary inside the kernel. FMA ordering and PSQ store flushing stay
// identical to the original matrix routines.
namespace NativeMatrix {
#if defined(__aarch64__)
using Row = float32x4_t;
inline Row LoadRow(const uint8_t* p) { return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p))); }
inline Row Mul(Row a, Row b) { return vmulq_f32(a, b); }
inline Row Madd(Row a, Row b, Row c) { return vfmaq_f32(c, a, b); }
template<int Lane> inline Row Splat(Row a) { return vdupq_laneq_f32(a, Lane); }
inline bool Bounded(Row a) {
    const auto magnitude = vandq_u32(vreinterpretq_u32_f32(a), vdupq_n_u32(0x7fffffffu));
    return vmaxvq_u32(magnitude) <= 0x4f800000u;
}
template<int Half> inline double Pack(Row a) {
    const auto pair = Half ? vget_high_f32(a) : vget_low_f32(a);
    return PpcM128ToPsInline(vreinterpret_f32_u32(vrev64_u32(vreinterpret_u32_f32(pair))));
}
inline Row Finish(Row result, Row a, Row constants, bool array) {
    const auto factors = array ? vget_high_f32(a) : vdup_n_f32(vgetq_lane_f32(a, 3));
    const auto hi = vfma_f32(vget_high_f32(result), vget_low_f32(constants), factors);
    return vcombine_f32(vget_low_f32(result), hi);
}
inline void StoreRow(uint8_t* p, Row a) {
    const auto lo = PpcStorePairPsqFloatBitsLanesInline(vreinterpret_u32_f32(vget_low_f32(a)));
    const auto hi = PpcStorePairPsqFloatBitsLanesInline(vreinterpret_u32_f32(vget_high_f32(a)));
    vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_u32(vcombine_u32(lo, hi))));
}
inline Row LoadConstants(const uint8_t* p) {
    const auto pair = vreinterpret_f32_u8(vrev32_u8(vld1_u8(p)));
    return vcombine_f32(pair, vdup_n_f32(0));
}
#else
using Row = __m128;
inline Row LoadRow(const uint8_t* p) {
    const auto order = _mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), order));
}
inline Row Mul(Row a, Row b) { return _mm_mul_ps(a, b); }
inline Row Madd(Row a, Row b, Row c) { return _mm_fmadd_ps(a, b, c); }
template<int Lane> inline Row Splat(Row a) { return _mm_shuffle_ps(a,a,_MM_SHUFFLE(Lane,Lane,Lane,Lane)); }
inline bool Bounded(Row a) {
    const auto magnitude = _mm_and_si128(_mm_castps_si128(a), _mm_set1_epi32(0x7fffffff));
    return _mm_movemask_epi8(_mm_cmpgt_epi32(magnitude,_mm_set1_epi32(0x4f800000))) == 0;
}
template<int Half> inline double Pack(Row a) {
    return PpcM128ToPsInline(_mm_shuffle_ps(a,a,Half ? _MM_SHUFFLE(3,2,2,3) : _MM_SHUFFLE(1,0,0,1)));
}
inline Row Finish(Row result, Row a, Row constants, bool array) {
    const auto factors = array ? _mm_movehl_ps(a,a) : Splat<3>(a);
    const auto hi = Madd(constants, factors, _mm_movehl_ps(result,result));
    return _mm_movelh_ps(result,hi);
}
inline void StoreRow(uint8_t* p, Row a) {
    const auto lanes = PpcStorePairPsqFloatBitsLanesInline(_mm_castps_si128(a));
    const auto order = _mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p),_mm_shuffle_epi8(lanes,order));
}
inline Row LoadConstants(const uint8_t* p) {
    const auto order = _mm_setr_epi8(3,2,1,0,7,6,5,4,-1,-1,-1,-1,-1,-1,-1,-1);
    return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p)),order));
}
#endif
struct Matrix { Row rows[3]; };
inline Matrix Load(const uint8_t* p) { return {{LoadRow(p),LoadRow(p+16),LoadRow(p+32)}}; }
inline bool Bounded(const Matrix& m) { return Bounded(m.rows[0]) && Bounded(m.rows[1]) && Bounded(m.rows[2]); }
inline Row ConcatRow(Row a, const Matrix& b, Row constants, bool array) {
    Row r = Mul(b.rows[0],Splat<0>(a));
    r = Madd(b.rows[1],Splat<1>(a),r);
    r = Madd(b.rows[2],Splat<2>(a),r);
    return Finish(r,a,constants,array);
}
inline Matrix Concat(const Matrix& a, const Matrix& b, Row constants, bool array) {
    return {{ConcatRow(a.rows[0],b,constants,array),ConcatRow(a.rows[1],b,constants,array),ConcatRow(a.rows[2],b,constants,array)}};
}
inline void Store(uint8_t* p, const Matrix& m) { for (unsigned i=0;i<3;++i) StoreRow(p+16*i,m.rows[i]); }
inline void SetPairs(CpuContext* ctx, unsigned first, const Matrix& m) {
    for (unsigned i=0;i<3;++i) {
        ctx->fpr[first+2*i].d=Pack<0>(m.rows[i]);
        ctx->fpr[first+2*i+1].d=Pack<1>(m.rows[i]);
    }
}
}
