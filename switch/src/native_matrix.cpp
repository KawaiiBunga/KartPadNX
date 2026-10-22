#include "native_matrix_kernel.h"
#include "memory_access.h"
#include "isa/ppc_isa_context.h"
#include <cstring>

namespace {
uint8_t* Range(uint32_t address, uint32_t size, bool write = false) {
    return MemoryInline::ResolveRangeHost(address, 0, size, true, write);
}

void W32(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}

bool Overlaps(const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
    const auto x = reinterpret_cast<uintptr_t>(a);
    const auto y = reinterpret_cast<uintptr_t>(b);
    return x < y + bn && y < x + an;
}

template<bool Array>
bool MatrixConcat(CpuContext* ctx) {
    constexpr uint32_t Frame = Array ? 96u : 64u;
    if (ctx->gqr[0] != 0) return false;
    const uint32_t count = Array ? ctx->gpr[6] : 1;
    if (count < (Array ? 2u : 1u) || count > 4096 || ctx->gpr[1] < Frame) return false;
    const uint32_t length = count * 48;
    const uint32_t constantsAddress = Array ? ctx->gpr[13] - 29176u : 0x80385a08u;
    auto* left = Range(ctx->gpr[3], 48);
    auto* right = Range(ctx->gpr[4], length);
    auto* output = Range(ctx->gpr[5], length, true);
    auto* constants = Range(constantsAddress, 8);
    auto* stack = Range(ctx->gpr[1] - Frame, 4, true);
    if (!left || !right || !output || !constants || !stack) return false;
    if ((right != output && Overlaps(right, length, output, length)) ||
        Overlaps(stack, 4, left, 48) || Overlaps(stack, 4, right, length) ||
        Overlaps(stack, 4, output, length) || Overlaps(stack, 4, constants, 8) ||
        Overlaps(output, length, constants, 8)) return false;
    const auto a = NativeMatrix::Load(left);
    const auto c = NativeMatrix::LoadConstants(constants);
    if (!NativeMatrix::Bounded(a) || !NativeMatrix::Bounded(c)) return false;
    // Single-pass: check bounds and concat in one traversal to halve memory reads.
    // Bail to translated fallback on the first out-of-bounds matrix.
    W32(stack, ctx->gpr[1]);
    NativeMatrix::Matrix b, result;
    for (uint32_t i = 0; i < count; ++i) {
        b = NativeMatrix::Load(right + 48 * i);
        if (!NativeMatrix::Bounded(b)) [[unlikely]] return false;
        result = NativeMatrix::Concat(a, b, c, Array);
        NativeMatrix::Store(output + 48 * i, result);
    }
    NativeMatrix::SetPairs(ctx, 0, a);
    NativeMatrix::SetPairs(ctx, 6, b);
    ctx->fpr[12].d = NativeMatrix::Pack<0>(result.rows[0]);
    ctx->fpr[13].d = NativeMatrix::Pack<1>(result.rows[0]);
    if constexpr (Array) {
        ctx->gpr[0] = count - 1;
        ctx->gpr[4] += 48 * (count - 1);
        ctx->gpr[5] += 48 * (count - 1);
        ctx->ctr = 0;
    } else {
        ctx->fpr[0].d = NativeMatrix::Pack<1>(result.rows[2]);
        ctx->fpr[2].d = NativeMatrix::Pack<0>(result.rows[2]);
    }
    ctx->gpr[6] = constantsAddress;
    return true;
}
} // namespace

bool MkwTryNativeMatrixConcat(CpuContext* ctx) {
    return MatrixConcat<false>(ctx);
}

bool MkwTryNativeMatrixConcatArray(CpuContext* ctx) {
    return MatrixConcat<true>(ctx);
}

extern "C" int kartpad_opt(const char*, int) __attribute__((weak));
// perf.cfg `native_mtx_concat` (default 1): 0 = translated PSMTXConcat/ConcatArray.
static bool NativeConcatEnabled() {
    static const bool on = kartpad_opt == nullptr || kartpad_opt("native_mtx_concat", 1) != 0;
    return on;
}

extern "C" {
void __real_func_80199D64(CpuContext* ctx);
void __wrap_func_80199D64(CpuContext* ctx) {
    if (NativeConcatEnabled() && MkwTryNativeMatrixConcat(ctx)) return;
    __real_func_80199D64(ctx);
}

void __real_func_80199E30(CpuContext* ctx);
void __wrap_func_80199E30(CpuContext* ctx) {
    if (NativeConcatEnabled() && MkwTryNativeMatrixConcatArray(ctx)) return;
    __real_func_80199E30(ctx);
}
}
