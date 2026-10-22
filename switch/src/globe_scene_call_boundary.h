#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace kartpad {
// GlobeScene archive registration uses its scene pointer after blocking
// archive calls. Enforce ordinary returning PPC EABI; never used for rfi or
// non-link continuation jumps. Guest memory and volatile outputs are retained.
class GlobeSceneCallBoundary {
public:
    GlobeSceneCallBoundary(uint32_t* regs,
        std::initializer_list<std::pair<unsigned,uint32_t>> locals,
        uint32_t target,uint32_t lr):regs_(regs),target_(target),lr_(lr) {
        for(auto [index,value]:locals)regs_[index]=value;
        for(unsigned i=0;i<32;++i)if(Preserved&(1u<<i))saved_[i]=regs_[i];
    }
    ~GlobeSceneCallBoundary() noexcept {
        uint32_t changed=0;const uint32_t afterScene=regs_[30],afterSp=regs_[1];
        for(unsigned i=0;i<32;++i) {
            if(!(Preserved&(1u<<i)))continue;
            if(regs_[i]!=saved_[i])changed|=1u<<i;
            regs_[i]=saved_[i];
        }
        static std::atomic<unsigned> reports{0};
        if(changed&&reports.fetch_add(1,std::memory_order_relaxed)<16) {
            std::fprintf(stderr,"[globe-abi] target=%08x return=%08x changed=%08x sp=%08x->%08x scene=%08x->%08x\n",
                target_,lr_,changed,afterSp,saved_[1],afterScene,saved_[30]);
            std::fflush(stderr);
        }
    }
    GlobeSceneCallBoundary(const GlobeSceneCallBoundary&)=delete;
    GlobeSceneCallBoundary& operator=(const GlobeSceneCallBoundary&)=delete;
private:
    static constexpr uint32_t Preserved=0xffffe006u;
    uint32_t* regs_;
    std::array<uint32_t,32> saved_{};
    uint32_t target_,lr_;
};
}
