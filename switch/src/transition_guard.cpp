// Course-transition guard for the native renderer (docs/RR-PERFORMANCE-2026-10.md, "GPU timeout at
// course changes"). Every NVIDIA channel timeout recorded so far (2348-0006, vanilla and Retro
// Rewind) came while a course loaded: the first one after the menus (vanilla, private/crash-triage/
// 2026-10-24, and again 2026-10-27) or the next one after a race. The only renderer policy that never
// timed out there completed each GPU operation before recording the next (v3). The renderer asks for
// every GPU operation (switch/deko/backend_commands.cpp) and works that way from the moment the game
// changes to the race scene, or a race stops running, until a race runs, at most kWindow: race frames
// and the menus keep the asynchronous policy and its frame rate.
#include "memory_access.h"

#include <chrono>
#include <cstdint>

namespace {
// The RaceManager (Raceinfo) singleton and its race frame counter, as perf_diag.cpp reads them. The
// counter advances at the logic rate (~60/s, in bursts per drawn frame) through the race and its
// results; the race scene's exit replaces or clears the singleton at once.
constexpr uint32_t kRaceManager = 0x809BD730u;
constexpr uint32_t kRaceFrameCounter = 0x20u;
// The scene manager (EGG::TSystem::GetSceneMgr: RKSystem 0x802A4080 + 0x54) and its next and current
// scene IDs (EGG::SceneManager::setupNextSceneID). changeSiblingSceneAfterFadeOut sets the next one for
// the fade-out; changeScene makes it current before it creates the scene, whose constructor loads the
// course. GameSceneCreator::create builds the race scene (RaceScene::__ct) for ID 2. The RaceManager
// only appears later in that load, so it cannot tell the first course load after the menus.
constexpr uint32_t kSceneManager = 0x802A40D4u;
constexpr uint32_t kNextSceneId = 0x14u;
constexpr uint32_t kCurrentSceneId = 0x18u;
constexpr uint32_t kRaceScene = 2u;
// A paused race: the counter stops. Race frames slower than this are rarer than 1 in 1,000.
constexpr auto kStopped = std::chrono::milliseconds(200);
// Loading the next course takes 10-20 s (13 s with the guard, test 7). After a Grand Prix's last race
// the menus follow; every operation completing made them slow, so the window ends at 30 s, not 90.
constexpr auto kWindow = std::chrono::seconds(30);

bool guest_pointer(uint32_t address, uint32_t& value) {
    return MemoryInline::TryReadGuestScalar(address, value) && value >= 0x80000000u && value < 0x94000000u;
}

bool race_scene() {
    uint32_t manager = 0, next = 0, current = 0;
    return guest_pointer(kSceneManager, manager) &&
           MemoryInline::TryReadGuestScalar(manager + kNextSceneId, next) &&
           MemoryInline::TryReadGuestScalar(manager + kCurrentSceneId, current) &&
           (next == kRaceScene || current == kRaceScene);
}
}  // namespace

// Called under the renderer's object lock (kd::mutex), from replay and present alike: the statics are
// never touched concurrently.
extern "C" int kartpad_gpu_transition_guard(void) {
    using Clock = std::chrono::steady_clock;
    static uint32_t lastManager = 0, lastCounter = 0;
    static Clock::time_point lastAdvance{}, sceneEntered{};
    static bool raced = false, left = false, inRaceScene = false;
    const auto now = Clock::now();
    uint32_t manager = 0, counter = 0;
    if (!guest_pointer(kRaceManager, manager) ||
        !MemoryInline::TryReadGuestScalar(manager + kRaceFrameCounter, counter))
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
    const auto stopped = now - lastAdvance;
    const bool raceScene = race_scene();
    if (raceScene && !inRaceScene) sceneEntered = now;
    inRaceScene = raceScene;
    // Changing to the race scene, and its course loading, until the race runs.
    if (raceScene && now - sceneEntered < kWindow && !(raced && !left && stopped <= kStopped)) return 1;
    if (!raced) return 0;
    return (left || stopped > kStopped) && stopped < kWindow;
}
