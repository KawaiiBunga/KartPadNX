// RKG ghost-replay fixture hooks.
//
// The translator injects calls to these into specific translated shards
// (functions/func_8051FC84, func_80643F48, func_8083DFA8, func_80846C1C,
// func_8084E388 via ref/kartpad-main/scripts/inject-*-rkg-*.py). The desktop /
// Apple input HLE defines them to drive automated ghost playback for
// reproducible testing. This port has no RKG replay fixture, so they are
// no-ops: CalcInner returns false (real controller input is used, the original
// function body runs unchanged) and the Force* selectors pass the requested id
// through without overriding character / vehicle / course / drift-button choice.
#include <cstdint>

struct CpuContext;

extern "C" bool KPad_RkgFixture_CalcInner(CpuContext* /*ctx*/) { return false; }
extern "C" uint32_t KPad_RkgFixture_ForceCourseId(uint32_t requested) { return requested; }
extern "C" uint32_t KPad_RkgFixture_ForceCharacterId(uint32_t requested) { return requested; }
extern "C" uint32_t KPad_RkgFixture_ForceVehicleId(uint32_t requested) { return requested; }
extern "C" uint32_t KPad_RkgFixture_ForceDriftButtonId(uint32_t requested) { return requested; }
