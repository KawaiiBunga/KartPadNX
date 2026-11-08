// Out-of-line runtime symbols the inline memory/psq helpers reference, for the host test only.
// The whole 4 GiB guest space is one flat RAM mapping at MKW_FLAT_GUEST_BASE (interpreter_test.cpp
// maps it), so every page gets the plain "host = flat base + guest" bias and the slow paths are
// plain big-endian accesses through the same mapping.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ppc_runtime.h"

namespace RecompMod {
std::atomic<uint8_t> g_executableWriteGuardPages[kExecutableWriteGuardPageCount];
}

namespace MemoryInline {
PageEntry g_pageTable[kPageCount];
uintptr_t g_fullReadablePageBias[kPageCount];
uintptr_t g_fullWritablePageBias[kPageCount];
const SparseWritablePageTable* g_sparseWritablePageTables[kPageCount];

namespace {
const bool g_mapped = [] {
    for (uint32_t p = 0; p < kPageCount; ++p) {
        g_pageTable[p] = {MKW_FLAT_GUEST_BASE + (uint64_t(p) << kPageShift), kPageSize};
        g_fullReadablePageBias[p] = g_fullWritablePageBias[p] = reinterpret_cast<uintptr_t>(MKW_FLAT_GUEST_BASE) + 1u;
    }
    return true;
}();

template <typename T>
T Load(uint32_t a) {
    T v;
    std::memcpy(&v, MKW_FLAT_GUEST_BASE + a, sizeof v);
    return MaybeByteSwap(v);
}
template <typename T>
void Store(uint32_t a, T v) {
    v = MaybeByteSwap(v);
    std::memcpy(MKW_FLAT_GUEST_BASE + a, &v, sizeof v);
}
}  // namespace

uint8_t Read8Slow(uint32_t a) { return Load<uint8_t>(a); }
uint16_t Read16Slow(uint32_t a) { return Load<uint16_t>(a); }
uint32_t Read32Slow(uint32_t a) { return Load<uint32_t>(a); }
void Write8Slow(uint32_t a, uint8_t v) { Store(a, v); }
void Write16Slow(uint32_t a, uint16_t v) { Store(a, v); }
void Write32Slow(uint32_t a, uint32_t v) { Store(a, v); }
void Write64Slow(uint32_t a, uint64_t v) { Store(a, v); }
void WriteFloat32Slow(uint32_t a, double v) { Store(a, ConvertPpcDoubleToSingleBits(v)); }
void WriteFloat64Slow(uint32_t a, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    Store(a, bits);
}
}  // namespace MemoryInline

// Same bodies as runtime/src/ppc_helpers.cpp: scaled quantized psq accesses land here.
extern "C" double PPC_PsqL(uint32_t addr, uint32_t w, uint32_t i) {
    const uint32_t gqr = CurrentCpuContext()->gqr[i & 7];
    return w == 0 ? PPC_PsqLStateFallback<0u, 0u, false>(gqr, addr) : PPC_PsqLStateFallback<1u, 0u, false>(gqr, addr);
}
extern "C" void PPC_PsqSt(uint32_t addr, double value, uint32_t w, uint32_t i) {
    const uint32_t gqr = CurrentCpuContext()->gqr[i & 7];
    if (w == 0) PPC_PsqStStateFallback<0u, 0u, false>(gqr, addr, value);
    else PPC_PsqStStateFallback<1u, 0u, false>(gqr, addr, value);
}
