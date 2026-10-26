// Course-transition guard for the native renderer (docs/RR-PERFORMANCE-2026-10.md, "GPU timeout at
// course changes"). Every NVIDIA channel timeout recorded so far (2348-0006, vanilla and Retro
// Rewind) came between the end of one race and the start of the next, and the only renderer policy
// that never timed out there completed each GPU operation before recording the next (v3). The
// renderer asks for every GPU operation (switch/deko/backend_commands.cpp) and works that way from
// the moment a race stops running until the next one runs, at most kWindow: race frames keep the
// asynchronous policy and its frame rate.
#include "memory_access.h"

#include <chrono>
#include <cstdint>

namespace {
// The RaceManager (Raceinfo) singleton and its race frame counter, as perf_diag.cpp reads them. The
// counter advances at the logic rate (~60/s, in bursts per drawn frame) through the race and its
// results; the race scene's exit replaces or clears the singleton at once.
constexpr uint32_t kRaceManager = 0x809BD730u;
constexpr uint32_t kRaceFrameCounter = 0x20u;
// A paused race: the counter stops. Race frames slower than this are rarer than 1 in 1,000.
constexpr auto kStopped = std::chrono::milliseconds(200);
constexpr auto kWindow = std::chrono::seconds(90);         // loading the next course takes 10-20 s
}  // namespace

// Called under the renderer's object lock (kd::mutex), from replay and present alike: the statics are
// never touched concurrently.
extern "C" int kartpad_gpu_transition_guard(void) {
    using Clock = std::chrono::steady_clock;
    static uint32_t lastManager = 0, lastCounter = 0;
    static Clock::time_point lastAdvance{};
    static bool raced = false, left = false;
    const auto now = Clock::now();
    uint32_t manager = 0, counter = 0;
    if (!MemoryInline::TryReadGuestScalar(kRaceManager, manager) || manager < 0x80000000u ||
        manager >= 0x94000000u || !MemoryInline::TryReadGuestScalar(manager + kRaceFrameCounter, counter))
        manager = 0;
    if (manager != 0 && manager == lastManager && counter != lastCounter) {
        lastAdvance = now;
        raced = true;
        left = false;
    } else if (raced && manager != lastManager) {
        left = true;  // the race's manager is gone: the scene is changing now
    }
    lastManager = manager;
    lastCounter = counter;
    if (!raced) return 0;
    const auto stopped = now - lastAdvance;
    return (left || stopped > kStopped) && stopped < kWindow;
}
