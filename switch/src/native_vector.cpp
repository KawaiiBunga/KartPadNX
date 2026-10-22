#include "native_vector_kernel.h"
#include "memory_access.h"

extern "C" int kartpad_opt(const char*, int) __attribute__((weak));
namespace {
bool Enabled() {
    static const bool enabled = kartpad_opt && kartpad_opt("native_mtx_vec", 1) != 0;
    return enabled;
}
uint8_t* PlainRange(uint32_t addr, uint32_t size, bool write) {
    // Use one cached RAM view only. Physical/uncached mirrors can alias the
    // same backing through distinct host addresses, defeating numeric pointer
    // overlap tests; keep those calls on the translated path.
    if ((addr & 0xe0000000u) != 0x80000000u) return nullptr;
    if (addr > UINT32_MAX - (size - 1)) return nullptr;
    const auto page = addr >> MemoryInline::kPageShift;
    if (((addr + size - 1) >> MemoryInline::kPageShift) != page) return nullptr;
    const uintptr_t bias = write ? MemoryInline::g_scalarWritablePageBias[page]
                                 : MemoryInline::g_scalarReadablePageBias[page];
    return bias ? reinterpret_cast<uint8_t*>(bias - 1u + addr) : nullptr;
}
template<bool RotationOnly>
bool TryTransform(CpuContext* ctx) {
    if (!Enabled() || ctx->gqr[0] != 0) return false;
    auto* matrix = PlainRange(ctx->gpr[3], 48, false);
    auto* vector = PlainRange(ctx->gpr[4], 12, false);
    auto* output = PlainRange(ctx->gpr[5], 12, true);
    if (!matrix || !vector || !output) return false;
    // The affine routine interleaves stores with later matrix loads. Decline
    // overlapping matrix/output aliases; input-vector/output aliasing is safe
    // because all vector components are loaded before the first original store.
    const auto m = reinterpret_cast<uintptr_t>(matrix), o = reinterpret_cast<uintptr_t>(output);
    if (m < o + 12 && o < m + 48) return false;
    if (!NativeVector::Bounded<48>(matrix) || !NativeVector::Bounded<12>(vector)) return false;
    const auto inputs = NativeVector::Load(matrix, vector);
    NativeVector::Transform<RotationOnly>(ctx, inputs, output);
    return true;
}
} // namespace
bool MkwTryNativeMatrixVector(CpuContext* ctx) { return TryTransform<false>(ctx); }
bool MkwTryNativeMatrixVectorSR(CpuContext* ctx) { return TryTransform<true>(ctx); }
extern "C" {
void __real_func_8019A91C(CpuContext*);
void __real_func_8019A970(CpuContext*);
void __wrap_func_8019A91C(CpuContext* ctx) {
    if (!MkwTryNativeMatrixVector(ctx)) __real_func_8019A91C(ctx);
}
void __wrap_func_8019A970(CpuContext* ctx) {
    if (!MkwTryNativeMatrixVectorSR(ctx)) __real_func_8019A970(ctx);
}
}
