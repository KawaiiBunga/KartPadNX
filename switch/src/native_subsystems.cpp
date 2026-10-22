#include "native_subsystem_kernels.h"
#include "abi_bridge.h"
#include <cstdio>

extern "C" int kartpad_opt(const char*,int) __attribute__((weak));
namespace {
struct Counts { uint64_t accepted=0, fallback=0; } counts[5];
template<uint32_t Target, unsigned Family, int Default, class Kernel>
bool Try(CpuContext* c, const char* key, Kernel kernel) {
    static const bool on=kartpad_opt ? kartpad_opt(key,Default)!=0 : Default!=0;
    if (on && IsBaseTranslatedCpuTargetActive<Target>() && kernel(c)) {
        ++counts[Family].accepted; return true;
    }
    ++counts[Family].fallback; return false;
}
}
extern "C" void MkwReportNativeSubsystems() {
    const char* families[]={"particles","collision","matrix-copy","scene-walk","animation"};
    for (unsigned i=0;i<5;++i) {
        std::fprintf(stderr,"[HOS native subsystem] %s accepted=%llu fallback=%llu\n",families[i],
            (unsigned long long)counts[i].accepted,(unsigned long long)counts[i].fallback);
        counts[i]={};
    }
}
#define NATIVE_WRAPPER(Address, Family, Default, Key, Kernel) \
    extern "C" void __real_func_##Address(CpuContext*); \
    extern "C" void __wrap_func_##Address(CpuContext* c) { \
        if (!Try<0x##Address##u,Family,Default>(c,Key,Kernel)) __real_func_##Address(c); \
    }
// Defaults select the three kernels with a promising host/ARM64 timing result.
// Equivalence alone is insufficient to promote an experimental replacement.
NATIVE_WRAPPER(80035340,0,0,"native_particle_passes",[](CpuContext* c){return NativeSubsystem::ParticlePass(c,true);})
NATIVE_WRAPPER(800353B0,0,0,"native_particle_passes",[](CpuContext* c){return NativeSubsystem::ParticlePass(c,false);})
NATIVE_WRAPPER(800365D0,0,1,"native_particle_lists",NativeSubsystem::GatherList)
NATIVE_WRAPPER(80041D10,0,0,"native_particles",NativeSubsystem::ParticleDirection)
NATIVE_WRAPPER(807BE030,1,0,"native_collision",NativeSubsystem::CollisionLookup)
NATIVE_WRAPPER(80199D30,2,1,"native_mtx_copy",NativeSubsystem::MatrixCopy)
NATIVE_WRAPPER(8006D6E0,3,1,"native_scene_world",[](CpuContext* c){return IsBaseTranslatedCpuTargetActive<0x80199D30u>() && NativeSubsystem::SceneWorldCopy(c);})
NATIVE_WRAPPER(80052FD0,4,0,"native_animation",NativeSubsystem::Keyframe)
NATIVE_WRAPPER(8006E6F0,3,0,"native_scene_walk",[](CpuContext* c){return NativeSubsystem::SceneVertexWalk(c,[](uint32_t a,CpuContext* x){InvokeIndirectCpu(a,x);});})
#undef NATIVE_WRAPPER
