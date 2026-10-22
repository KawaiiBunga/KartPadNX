#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace kartpad {
// Cold archive-loading calls can yield to guest jobs. Publish resident values
// before the call and enforce the ordinary returning PPC EABI afterwards.
// This is never used for OSLoadContext/rfi or a non-returning guest jump.
class ArchiveCallBoundary {
public:
    ArchiveCallBoundary(uint32_t* regs,
        std::initializer_list<std::pair<unsigned, uint32_t>> locals,
        uint32_t target, uint32_t lr) : regs_(regs), target_(target), lr_(lr) {
        for (auto [index, value] : locals) regs_[index] = value;
        for (unsigned i=0; i<32; ++i)
            if (Preserved & (1u<<i)) saved_[i] = regs_[i];
    }
    ~ArchiveCallBoundary() noexcept {
        uint32_t changed=0;
        const uint32_t sp=regs_[1], holder=regs_[30];
        for (unsigned i=0; i<32; ++i) {
            if (!(Preserved & (1u<<i))) continue;
            if (regs_[i]!=saved_[i]) changed|=1u<<i;
            regs_[i]=saved_[i];
        }
        static std::atomic<unsigned> reports{0};
        if (changed && reports.fetch_add(1, std::memory_order_relaxed)<32) {
            std::fprintf(stderr, "[archive-abi] target=%08x return=%08x changed=%08x sp=%08x->%08x r30=%08x->%08x\n",
                target_, lr_, changed, sp, saved_[1], holder, saved_[30]);
            std::fflush(stderr);
        }
    }
    ArchiveCallBoundary(const ArchiveCallBoundary&)=delete;
    ArchiveCallBoundary& operator=(const ArchiveCallBoundary&)=delete;
private:
    static constexpr uint32_t Preserved=0xffffe006u;
    uint32_t* regs_;
    std::array<uint32_t,32> saved_{};
    uint32_t target_,lr_;
};
}
