#if defined(__SWITCH__)
#include "abi_bridge.h"
#include "hle/gx/gx_internal.h"
#include "dolphin/gx/GXAurora.h"
#include "memory_access.h"
#include "isa/ppc_isa_float.h"
#include "isa/ppc_isa_cr.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t ModeAddress = 0x802b8040u;
// Original PPC base register; its stores use a -32768 displacement.
constexpr uint32_t FifoAddress = 0xcc010000u;
struct Counts { uint64_t channels = 0, integerColors = 0, fallback = 0; } counts;
bool enabled = true;

template<uint32_t Address> bool Available() {
    if (!enabled) return false;
    return IsBaseTranslatedCpuTargetActive<Address>();
}

uint8_t* Range(uint32_t address, uint32_t size, bool write = false) {
    if (size == 0) return nullptr;
    const uint32_t last = address + size - 1;
    if (last < address) return nullptr;
    const uint32_t page1 = address >> MemoryInline::kPageShift;
    const uint32_t page2 = last >> MemoryInline::kPageShift;
    if (page1 == page2) {
        const uintptr_t bias = write ? MemoryInline::g_scalarWritablePageBias[page1]
                                     : MemoryInline::g_scalarReadablePageBias[page1];
        if (bias) return reinterpret_cast<uint8_t*>((bias - 1u) + address);
    }
    return MemoryInline::ResolveRangeHost(address, 0, size, true, write);
}

uint32_t U32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
void W32(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(p, &v, 4); }

bool Overlaps(const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
    const auto x = reinterpret_cast<uintptr_t>(a), y = reinterpret_cast<uintptr_t>(b);
    return x < y + bn && y < x + an;
}

bool Decline() { ++counts.fallback; return false; }

// Only BP/XF commands, at most eight channel writes and the 28-byte mode
// sequence. One owned stack packet replaces per-field FIFO entry calls.
struct Packet {
    std::array<uint8_t, 100> bytes;
    uint32_t size = 0;
    void Xf(uint32_t reg, uint32_t word) {
        bytes[size++] = 0x10;
        W32(bytes.data() + size, reg); size += 4;
        W32(bytes.data() + size, word); size += 4;
    }
    void Bp(uint32_t word) {
        bytes[size++] = 0x61;
        W32(bytes.data() + size, word); size += 4;
    }
};

uint32_t ChannelControl(uint32_t word, uint32_t lights, bool enabled) {
    // Replace the split eight-light mask, retaining the exact original
    // lighting-enable rule and all other register bits.
    return (word & (enabled ? 0xffff87c1u : 0xffff87c3u)) |
           ((lights << 2) & 0x3cu) | ((lights << 7) & 0x7800u);
}
} // namespace

bool MkwTryNativeMaterialChannels(CpuContext* ctx) {
    if (!Available<0x80062c50u>()) [[unlikely]] return Decline();
    // Preflight is read-only and does not materialize deferred reads. Never
    // retain a resolved host pointer across a guest call or fiber suspension.
    auto* handle = Range(ctx->gpr[3], 4);
    if (!handle) [[unlikely]] return Decline();
    const uint32_t material = U32(handle);
    // Prefetch the material data into L1D before we need it. The A57 has
    // only 32 KiB L1D and guest pointer chasing causes frequent misses.
    if (material) __builtin_prefetch(Range(material, 4), 0, 3);
    if (!material || ctx->gpr[1] < 128) [[unlikely]] return Decline();
    auto* mode = Range(ModeAddress, 12, true);
    if (!mode) [[unlikely]] return Decline();
    const bool second = mode[1] == 2;
    const uint32_t materialBytes = second ? 40 : 20;
    auto* data = Range(material, materialBytes);
    auto* ambient = Range(ctx->gpr[8], 4);
    auto* stack = Range(ctx->gpr[1] - 128, 136, true);
    // Include the two floats and the full conversion-bias double.
    auto* constants = Range(ctx->gpr[2] - 29508u, 20);
    if (!data || !ambient || !stack || !constants) [[unlikely]] return Decline();
    const uint32_t modeFlags = U32(mode + 8);
    const bool sendMode = (modeFlags & 4) && (modeFlags & 3) != 3;
    uint8_t* cull = nullptr;
    if (sendMode) {
        const uint32_t cullIndex = U32(mode + 4);
        const uint64_t cullAddress = uint64_t(ctx->gpr[2] - 29560u) + cullIndex;
        if (cullAddress > UINT32_MAX) [[unlikely]] return Decline();
        cull = Range(static_cast<uint32_t>(cullAddress), 1);
        if (!cull) [[unlikely]] return Decline();
    }
    // Native preparation reads fields once. Reject aliases with either write
    // destination so the fallback preserves self-modifying/overlapping inputs.
    if (Overlaps(stack, 136, mode, 12)) [[unlikely]] return Decline();
    const std::array<std::pair<const uint8_t*, size_t>, 5> inputs{{
        {handle, 4}, {data, materialBytes}, {ambient, 4}, {constants, 20}, {cull, cull ? 1u : 0u}}};
    for (auto [p, size] : inputs) {
        if (size && (Overlaps(p, size, stack, 136) || Overlaps(p, size, mode, 12))) [[unlikely]] return Decline();
    }
    // The original integer-to-double construction subtracts this exact bias.
    // Other constants retain their guest-provided bits and PPC rounding.
    const uint64_t biasBits = (uint64_t(U32(constants + 12)) << 32) | U32(constants + 16);
    if (biasBits != 0x4330000080000000ull) [[unlikely]] return Decline();

    Packet packet;
    uint32_t r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5], r6 = ctx->gpr[6];
    uint32_t r7 = ctx->gpr[7], r8 = ctx->gpr[8], r9 = ctx->gpr[9], r10 = material, r12 = ctx->gpr[12];
    const bool lighting = r9 != 0;
    const uint32_t controlMask = lighting ? 0xffff87c1u : 0xffff87c3u;
    const uint32_t flags = U32(data);

    // Preserve even the caller-visible scratch/save bytes of the PPC frame.
    W32(stack, ctx->gpr[1]); W32(stack + 132, ctx->lr);
    for (uint32_t i = 27; i <= 31; ++i) W32(stack + 108 + (i - 27) * 4, ctx->gpr[i]);
    W32(stack + 48, 0x43300000u); W32(stack + 56, 0x43300000u);
    if (flags & 3) {
        const uint32_t color = U32(data + 4);
        const uint32_t offset = (flags & 3) == 3 ? 40 : (flags & 1) ? 36 : 32;
        W32(stack + offset, color);
        packet.Xf(0x100c, color);
        r12 = FifoAddress;
    }

    // Four bounded byte products convert directly to native doubles, avoiding
    // the PPC spill/reload conversion idiom. Keep its single-rounding stages
    // and packed fctiwz results, including volatile FPR outputs.
    const double add = static_cast<double>(std::bit_cast<float>(U32(constants)));
    const double scale = static_cast<double>(std::bit_cast<float>(U32(constants + 4)));
    const bool integerColors = U32(constants) == 0x3f000000u && U32(constants + 4) == 0x3b808081u;
    uint32_t products[4], colors[4];
    for (unsigned i = 0; i < 4; ++i) {
        products[i] = uint32_t(ambient[i]) * data[8 + i];
        if (integerColors) {
            // For byte products 0..65025, round(product/255) equals the PPC
            // single multiply/add/truncate sequence with these two constants.
            // Exact division by 255 over this bounded domain, without the
            // four UDIV instructions selected by the Cortex-A57 compiler.
            const uint32_t biased = products[i] + 128u;
            colors[i] = (biased + (biased >> 8)) >> 8;
            ctx->fpr[3 - i].raw = colors[i]; // original fctiwz packed low word
        } else {
            const double scaled = PpcFmulsInline(scale, static_cast<double>(products[i]));
            const double rounded = static_cast<double>(PpcForceSingleValueInline(add + scaled));
            const double packed = PPC_Fctiwz(rounded);
            ctx->fpr[3 - i].d = packed;
            colors[i] = PPC_FprLowWordInline(packed);
        }
    }
    ctx->fpr[4].d = add; ctx->fpr[5].d = scale; ctx->fpr[6].raw = biasBits;
    W32(stack + 52, products[2] ^ 0x80000000u);
    W32(stack + 60, products[3] ^ 0x80000000u);
    const uint32_t ambientColor = (uint32_t(uint8_t(colors[0])) << 24) | (uint32_t(uint8_t(colors[1])) << 16) |
                                  (uint32_t(uint8_t(colors[2])) << 8) | uint8_t(colors[3]);
    W32(stack + 44, ambientColor);
    r12 = colors[0]; r8 = colors[2];
    if (flags & 12) {
        packet.Xf(0x100a, ambientColor);
        r8 = 0; r12 = ambientColor;
    }
    if (flags & 16) {
        packet.Xf(0x100e, ChannelControl(U32(data + 12), r4, lighting));
        r12 = controlMask; r8 = 0;
    }
    if (flags & 32) {
        packet.Xf(0x1010, ChannelControl(U32(data + 16), r5, lighting));
        r10 = controlMask; r8 = FifoAddress; r4 = 0;
    }
    r4 = ModeAddress;
    if (second) {
        r3 = material;
        const uint32_t otherFlags = U32(data + 20);
        r4 = otherFlags;
        if (otherFlags & 3) {
            const uint32_t color = U32(data + 24);
            const uint32_t offset = (otherFlags & 3) == 3 ? 28 : (otherFlags & 1) ? 24 : 20;
            W32(stack + offset, color); packet.Xf(0x100d, color);
            r5 = FifoAddress; r8 = 16; r4 = 0; r10 = data[27]; r12 = data[25];
        }
        r4 = otherFlags;
        if (otherFlags & 12) {
            const uint32_t color = U32(data + 28);
            const uint32_t offset = (otherFlags & 12) == 12 ? 16 : (otherFlags & 4) ? 12 : 8;
            W32(stack + offset, color); packet.Xf(0x100b, color);
            r5 = FifoAddress; r8 = 16; r4 = 0; r10 = data[31]; r12 = data[29];
        }
        if (otherFlags & 16) {
            r9 = ChannelControl(U32(data + 32), r6, lighting);
            packet.Xf(0x100f, r9);
            r8 = controlMask; r5 = FifoAddress; r4 = 0;
        }
        if (otherFlags & 32) {
            r6 = ChannelControl(U32(data + 36), r7, lighting);
            packet.Xf(0x1011, r6);
            r5 = controlMask; r4 = FifoAddress; r3 = 0;
        }
    } else {
        packet.Xf(0x100f, 0); packet.Xf(0x1011, 0);
        r5 = FifoAddress; r6 = 16; r4 = 0; r3 = 0x100f;
    }
    r3 = 0x802c0000u; r4 = modeFlags;
    SetCRResident(ctx->cr, ctx->xer, 0, static_cast<int32_t>(modeFlags & 4), int32_t{0});
    if (modeFlags & 4) SetCRResident(ctx->cr, ctx->xer, 0, modeFlags & 3, uint32_t{3});
    if (sendMode) {
        r3 = mode[0]; r4 = mode[1];
        const uint32_t tev = uint32_t(mode[2]) - 1;
        r8 = (uint32_t(mode[3]) << 16) | (uint32_t(*cull) << 14);
        const uint32_t gen = r8 | r3 | (r4 << 4) | (tev << 10);
        packet.Bp(0xfe07fc3fu); packet.Bp(gen);
        packet.Xf(0x1009, r4); packet.Xf(0x103f, r3);
        r5 = 0x1009; r6 = 0; r7 = 16; r9 = 0xfe080000u; r10 = FifoAddress;
        W32(mode + 8, modeFlags | 3);
    }
    ctx->gpr[0] = ctx->lr;
    ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5; ctx->gpr[6] = r6;
    ctx->gpr[7] = r7; ctx->gpr[8] = r8; ctx->gpr[9] = r9; ctx->gpr[10] = r10;
    ctx->gpr[11] = ctx->gpr[1]; ctx->gpr[12] = r12;
    if (packet.size) GX_HLE_FIFO_WriteBurst(packet.bytes.data(), packet.size);
    ++counts.channels;
    counts.integerColors += integerColors;
    return true;
}

extern "C" bool MkwNativeModelRecording() {
    return false;
}

extern void MkwReportNativeModel() __attribute__((weak));
extern "C" void MkwReportNativeSubsystems() __attribute__((weak));
extern void MkwReportNativeSceneRegions() __attribute__((weak));

extern "C" void MkwReportNativeScene() {
    if (MkwReportNativeSceneRegions) MkwReportNativeSceneRegions();
    if (MkwReportNativeSubsystems) MkwReportNativeSubsystems();
    if (MkwReportNativeModel) MkwReportNativeModel();
    std::fprintf(stderr, "[HOS native scene] channels=%llu integer-colors=%llu fallback=%llu\n",
                 static_cast<unsigned long long>(counts.channels),
                 static_cast<unsigned long long>(counts.integerColors),
                 static_cast<unsigned long long>(counts.fallback));
    counts = {};
}

extern "C" void GX_HLE_SubmitBP(uint32_t word) {
    if (GxHle::Deferring()) {
        // Same bytes the FIFO fallback below writes; the HLE thread's parser applies them.
        GxHle::AppendU8(0x61u);
        GxHle::AppendU32(word);
        return;
    }
    if (!IsDisplayListActive() && !g_hleGxState.inBegin && g_hleGxState.fifoByteCount == 0) {
        GXApplyBPReg(static_cast<uint8_t>(word >> 24), word & 0x00ffffffu);
        return;
    }
    HleFifoWrite(0x61u, 1);
    HleFifoWrite(word, 4);
}

bool MkwNativeModelSubmit(CpuContext* ctx) __attribute__((weak));

bool MkwTryNativeModelSubmission(CpuContext* ctx) {
    return enabled && MkwNativeModelSubmit && MkwNativeModelSubmit(ctx);
}

extern "C" int kartpad_opt(const char*, int) __attribute__((weak));
// perf.cfg `native_mat_channels` / `native_model` (default 1): 0 = translated originals.
static bool NativeSceneOpt(const char* key) {
    return kartpad_opt == nullptr || kartpad_opt(key, 1) != 0;
}

extern "C" {
void __real_func_80062C50(CpuContext* ctx);
void __wrap_func_80062C50(CpuContext* ctx) {
    static const bool on = NativeSceneOpt("native_mat_channels");
    if (on && MkwTryNativeMaterialChannels(ctx)) [[likely]] return;
    __real_func_80062C50(ctx);
}

void __real_func_80064FD0(CpuContext* ctx);
void __wrap_func_80064FD0(CpuContext* ctx) {
    static const bool on = NativeSceneOpt("native_model");
    if (on && MkwTryNativeModelSubmission(ctx)) [[likely]] return;
    __real_func_80064FD0(ctx);
}
}
#endif
