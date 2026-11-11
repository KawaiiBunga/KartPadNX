// Course-transition guard for the native renderer (docs/RR-PERFORMANCE-2026-10.md, "GPU timeout at
// course changes"). Every NVIDIA channel timeout recorded so far (2348-0006, vanilla and Retro
// Rewind) came while a course loaded: the first one after the menus (vanilla, private/crash-triage/
// 2026-10-24, and again 2026-10-27) or the next one after a race. The only renderer policy that never
// timed out there completed each GPU operation before recording the next (v3). The renderer asks for
// every GPU operation (switch/deko/backend_commands.cpp) and works that way from the moment the game
// changes to the race scene, or a race stops running, until a race runs, at most kWindow: race frames
// and the menus keep the asynchronous policy and its frame rate.
//
// Fallback only since 2026-11-11 (off unless perf.cfg gpu_guard 1): the timeouts came from copy-engine
// work, now fenced on the GPU in switch/deko/backend_commands.cpp (copy_engine_fence).
#include "memory_access.h"

#include <atomic>
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
// The course has loaded once the race scene's RaceManager exists and frames come steadily: the
// intro flyover. The race frame counter only starts at the countdown, so waiting for it kept every
// GPU operation synchronous through the whole intro (20-28 FPS, 2026-11-08 log). While the course
// loads, presents are 0.1-1.3 s apart, with short fast bursts (24 FPS, min 11 ms, 2026-11-11 log);
// 60 in a row under 100 ms each is a second of intro. They only count once the RaceManager exists:
// the menu fade-out before it runs at a steady 60 FPS and ended the guard mid-load (2026-11-11).
constexpr auto kSteadyPresent = std::chrono::milliseconds(100);
constexpr int kSteadyPresents = 60;
std::atomic<int> steadyPresents{0};
std::atomic<int64_t> lastPresentNs{0};
// Every course load stalls presents (0.1-1.3 s apart); race frames, the intro and paused races never
// do. A stall in the race scene re-arms the guard, whatever started the load.
constexpr auto kStall = std::chrono::milliseconds(300);
std::atomic<bool> stalled{false};

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

extern "C" int kartpad_opt(const char* key, int fallback) __attribute__((weak));

// After every present (switch/deko/device.cpp).
extern "C" void kartpad_gpu_present_done(void) {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    using std::chrono::nanoseconds;
    const int64_t last = lastPresentNs.exchange(now, std::memory_order_relaxed);
    if (last && now - last < std::chrono::duration_cast<nanoseconds>(kSteadyPresent).count())
        steadyPresents.fetch_add(1, std::memory_order_relaxed);
    else
        steadyPresents.store(0, std::memory_order_relaxed);
    if (last && now - last >= std::chrono::duration_cast<nanoseconds>(kStall).count())
        stalled.store(true, std::memory_order_relaxed);
}

// Called under the renderer's object lock (kd::mutex), from replay and present alike: the statics are
// never touched concurrently.
extern "C" int kartpad_gpu_transition_guard(void) {
    using Clock = std::chrono::steady_clock;
    static uint32_t lastManager = 0, lastCounter = 0;
    static Clock::time_point lastAdvance{}, sceneEntered{};
    static bool raced = false, left = false, inRaceScene = false, loaded = false;
    // perf.cfg gpu_guard_until_race 1: the old window, synchronous until the countdown.
    static const bool untilRace = kartpad_opt && kartpad_opt("gpu_guard_until_race", 0) != 0;
    // Off by default since the root cause is fixed: copy-engine work is fenced on the GPU
    // (kd::copy_engine_fence, switch/deko/backend_commands.cpp). perf.cfg gpu_guard 1 brings it back.
    static const bool on = kartpad_opt && kartpad_opt("gpu_guard", 0) != 0;
    if (!on) return 0;
    const auto now = Clock::now();
    uint32_t manager = 0, counter = 0;
    if (!guest_pointer(kRaceManager, manager) ||
        !MemoryInline::TryReadGuestScalar(manager + kRaceFrameCounter, counter))
        manager = 0;
    const bool managerChanged = manager != lastManager;
    if (manager != 0 && !managerChanged && counter != lastCounter) {
        lastAdvance = now;
        raced = true;
        left = false;
    } else if (raced && managerChanged) {
        left = true;  // the race's manager is gone: the scene is changing now
    }
    if (managerChanged) steadyPresents.store(0, std::memory_order_relaxed);
    lastManager = manager;
    lastCounter = counter;
    const auto stopped = now - lastAdvance;
    const bool raceScene = race_scene();
    const bool stall = stalled.exchange(false, std::memory_order_relaxed);
    // A restart or the next Grand Prix race loads a course without leaving the race scene, even
    // during the intro before any race frame (Scene Restart, 2026-11-09 and 2026-11-11): the first
    // course's `loaded` stayed set and the reload ran asynchronously into a 2348-0006 timeout. The
    // RaceManager may be rebuilt at the same address, so the present stall is the sure sign.
    if (raceScene && (!inRaceScene || managerChanged || stall)) {
        sceneEntered = now, loaded = false;
        steadyPresents.store(0, std::memory_order_relaxed);  // menu and results frames don't count
    }
    inRaceScene = raceScene;
    // Changing to the race scene, and its course loading, until the intro plays (or the race runs).
    if (raceScene && !untilRace && !loaded && manager != 0 && (!raced || left) &&
        steadyPresents.load(std::memory_order_relaxed) >= kSteadyPresents)
        loaded = true;
    if (raceScene && now - sceneEntered < kWindow && !loaded && !(raced && !left && stopped <= kStopped)) return 1;
    if (raceScene && loaded && !(raced && !left)) return 0;
    if (!raced) return 0;
    return (left || stopped > kStopped) && stopped < kWindow;
}
