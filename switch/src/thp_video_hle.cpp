#include "thp_video_decoder.h"
#include "isa/ppc_isa_context.h"
#include "memory.h"

#include <cstdio>
#include <exception>

extern "C" void __real_func_801B3BAC(CpuContext* ctx);
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t address, uint32_t size);

namespace {
constexpr uint32_t kPlayer = 0x809BEB00u;
constexpr uint32_t kDecodeReturn = 0x80552C2Cu;

bool RamRange(uint32_t address, size_t size) {
    const uint32_t physical = address & 0x3fffffffu;
    const uint64_t end = static_cast<uint64_t>(physical) + size;
    return address && size &&
        ((physical < Memory::kMem1Size && end <= Memory::kMem1Size) ||
         (physical >= Memory::kMem2PhysicalBase && end <= Memory::kMem2PhysicalEnd)) &&
        Memory::Contains(address, size);
}

bool TryNativeDecode(CpuContext* ctx, const char*& reason) {
    // THPVideoDecode has no compressed-size argument. This PAL player call site
    // keeps the component-size cursor in r25 and the component index in r26.
    // Validate them against the frame/player layout instead of scanning arbitrary
    // guest memory for an EOI (FF D9 can occur inside THP entropy).
    reason = "unrecognized THP caller";
    if (ctx->lr != kDecodeReturn || ctx->gpr[30] != kPlayer ||
        !RamRange(kPlayer, 424) || !RamRange(ctx->gpr[23], 4)) return false;
    const uint32_t components = Memory::Read32(kPlayer + 108);
    const uint32_t index = ctx->gpr[26];
    const uint32_t frame = Memory::Read32(ctx->gpr[23]);
    const uint32_t capacity = Memory::Read32(kPlayer + 68);
    reason = "invalid THP frame bounds";
    if (!components || components > 16 || index >= components ||
        capacity > ThpVideo::kMaxPacketSize || capacity < 8 + components * 4 ||
        !RamRange(frame, capacity) || ctx->gpr[25] != frame + 8 + index * 4 ||
        Memory::Read8(kPlayer + 112 + index) != 0) return false;
    uint64_t offset = 8 + components * 4;
    for (uint32_t i = 0; i < index; ++i) offset += Memory::Read32(frame + 8 + i * 4);
    const uint32_t bytes = Memory::Read32(ctx->gpr[25]);
    if (!bytes || offset + bytes > capacity || frame + offset != ctx->gpr[3] ||
        ctx->gpr[24] != ctx->gpr[3]) return false;
    const std::span<const uint8_t> packet{static_cast<const uint8_t*>(Memory::GetPointer(ctx->gpr[3], bytes)), bytes};
    ThpVideo::FrameInfo info;
    const auto inspected = ThpVideo::InspectFrame(packet, info);
    reason = ThpVideo::StatusName(inspected);
    if (inspected != ThpVideo::Status::Success) return false;

    reason = "THP dimensions or output buffers disagree with player";
    if (info.width != Memory::Read32(kPlayer + 128) || info.height != Memory::Read32(kPlayer + 132))
        return false;
    // The player's three YUV buffers are allocated from these dimensions.
    // Accept only the exact buffer popped by the existing guest video thread.
    const uint32_t textureSet = ctx->gpr[27];
    if (textureSet < kPlayer + 376 || textureSet > kPlayer + 408 ||
        ((textureSet - kPlayer - 376) % 16)) return false;
    std::array<ThpVideo::Plane, 3> planes;
    for (size_t c = 0; c < 3; ++c) {
        const uint32_t address = ctx->gpr[4 + c];
        if (address != Memory::Read32(textureSet + c * 4) || !RamRange(address, info.PlaneSize(c)))
            return false;
        planes[c] = {static_cast<uint8_t*>(Memory::GetPointer(address, info.PlaneSize(c))), info.PlaneSize(c)};
    }
    for (size_t c = 0; c < 3; ++c) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(planes[c].data);
        for (size_t d = c + 1; d < 3; ++d) {
            const uintptr_t other = reinterpret_cast<uintptr_t>(planes[d].data);
            if (begin < other + planes[d].size && other < begin + planes[c].size) return false;
        }
    }

    static thread_local ThpVideo::Decoder decoder;
    ThpVideo::Status decoded = decoder.Decode(packet, planes);
    reason = decoded == ThpVideo::Status::DecodeError ? decoder.Error() : ThpVideo::StatusName(decoded);
    if (decoded != ThpVideo::Status::Success) return false;
    // Replaces the original LCStoreData DMA notifications. Notify only after
    // all three planes have decoded successfully, so cached textures refresh.
    for (size_t c = 0; c < 3; ++c)
        GxNotifyGuestRamDmaWrite(ctx->gpr[4 + c], static_cast<uint32_t>(planes[c].size));
    ctx->gpr[3] = 0;
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr, "[HOS THP] native libjpeg-turbo decoder active: %ux%u, full-resolution YUV420, GX I8 tiles\n",
                     info.width, info.height);
    }
    return true;
}
} // namespace

// GNU --wrap redirects translated direct calls and registration pointers, while
// retaining the unchanged PPC implementation for unsupported frames/call sites.
extern "C" void __wrap_func_801B3BAC(CpuContext* ctx) {
    const char* reason = "native decoder exception";
    try {
        if (TryNativeDecode(ctx, reason)) return;
    } catch (const std::exception&) {
        reason = "native decoder allocation/memory failure";
    }
    static uint64_t fallbacks = 0;
    ++fallbacks;
    if (fallbacks <= 4)
        std::fprintf(stderr, "[HOS THP] PPC fallback #%llu: %s (lr=%08x)\n",
                     static_cast<unsigned long long>(fallbacks), reason, ctx->lr);
    __real_func_801B3BAC(ctx);
}
