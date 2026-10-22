#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace kartpad {
// Ordinary bl/bctrl calls in the stitched MenuDriverModel function. Internal
// non-link hooks/continuations deliberately do not pass through an EABI guard.
class MenuModelCallBoundary {
public:
    MenuModelCallBoundary(uint32_t* regs,
        std::initializer_list<std::pair<unsigned,uint32_t>> locals,
        uint32_t target, uint32_t lr) : regs_(regs), target_(target), lr_(lr) {
        for (auto [index,value] : locals) regs_[index] = value;
        for (unsigned i=0;i<32;++i)
            if (Preserved & (1u<<i)) saved_[i] = regs_[i];
    }
    ~MenuModelCallBoundary() noexcept {
        uint32_t changed=0;
        for (unsigned i=0;i<32;++i) {
            if (!(Preserved & (1u<<i))) continue;
            if (regs_[i] != saved_[i]) changed |= 1u<<i;
            regs_[i]=saved_[i];
        }
        static std::atomic<unsigned> reports{0};
        if (changed && reports.fetch_add(1,std::memory_order_relaxed)<16) {
            std::fprintf(stderr,"[menu-model-abi] target=%08x return=%08x changed=%08x\n",
                target_,lr_,changed);
            std::fflush(stderr);
        }
    }
    MenuModelCallBoundary(const MenuModelCallBoundary&)=delete;
    MenuModelCallBoundary& operator=(const MenuModelCallBoundary&)=delete;
private:
    static constexpr uint32_t Preserved=0xffffe006u;
    uint32_t* regs_;
    std::array<uint32_t,32> saved_{};
    uint32_t target_,lr_;
};
}
