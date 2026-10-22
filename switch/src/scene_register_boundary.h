#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

namespace kartpad {
// GameScene keeps its scene/heap pointers in resident r26-r31. Publish them
// before entering blocking SDK/virtual calls, and enforce their EABI return
// contract even if a nested host callback leaves CpuContext holding scratch
// registers. Volatile outputs and all guest memory side effects remain intact.
class SceneRegisterBoundary {
public:
    SceneRegisterBoundary(uint32_t* regs, uint32_t sp,
                          const std::array<uint32_t, 6>& resident,
                          uint32_t target, uint32_t returnAddress)
        : regs_(regs), target_(target), returnAddress_(returnAddress) {
        regs_[1] = sp;
        for (unsigned i = 0; i < resident.size(); ++i) regs_[26 + i] = resident[i];
        for (unsigned i = 0; i < 32; ++i)
            if (Preserved & (1u << i)) saved_[i] = regs_[i];
    }
    ~SceneRegisterBoundary() noexcept {
        uint32_t changed = 0;
        const uint32_t afterSp = regs_[1], afterScene = regs_[31];
        for (unsigned i = 0; i < 32; ++i) {
            if (!(Preserved & (1u << i))) continue;
            if (regs_[i] != saved_[i]) changed |= 1u << i;
            regs_[i] = saved_[i];
        }
        // Cold scene transitions only; bounded evidence for the responsible
        // nested boundary, without pixel readbacks or shader-source dumps.
        static unsigned reports = 0;
        if (changed && reports++ < 16)
            std::fprintf(stderr,
                "[scene-abi] repaired target=%08x return=%08x changed=%08x "
                "sp=%08x->%08x scene=%08x->%08x\n",
                target_, returnAddress_, changed, afterSp, saved_[1], afterScene, saved_[31]);
    }
    SceneRegisterBoundary(const SceneRegisterBoundary&) = delete;
    SceneRegisterBoundary& operator=(const SceneRegisterBoundary&) = delete;
private:
    static constexpr uint32_t Preserved = 0xffffe006u; // r1/r2/r13-r31
    uint32_t* regs_;
    std::array<uint32_t, 32> saved_{};
    uint32_t target_, returnAddress_;
};
} // namespace kartpad
