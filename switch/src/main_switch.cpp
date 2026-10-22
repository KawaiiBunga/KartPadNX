// NRO entry point for KartPad-NX.
//
// Responsibilities (thin): bring up libnx subsystems, mount romfs + SD, ensure
// game data is present (on-console ingestion), then hand control to the
// translated runtime. The runtime + Aurora own the game loop and rendering.
//
// STATUS: full-app entry is live. The translated base-game runtime is linked
// through KartPadRuntimeMain; this file owns only libnx/data/log handoff.

#include "kartpad_mobile_runtime_host.h"
#include "clang_tls_switch.h"
#include "launch_ts.h"
#include "log_switch.h"
#include "perf_switch.h"

#include <switch.h>

#include <fcntl.h>
#include <cstdio>
#include <iostream>
#include <unistd.h>
#include <string_view>
#include <string>
#include <chrono>
#include <mutex>

namespace mkw_launcher_args {
  static std::string g_returnPath;
  static std::string g_overrideVariant;
  static uint64_t g_launchTsMs = 0;
}

namespace {
static uint64_t s_bootEntryTick = 0;
static uint64_t s_lastPhaseTick = 0;
static uint64_t s_bootEntryEpochMs = 0;
static uint64_t s_handoffLatencyMs = 0;
static HandoffStatus s_handoffStatus = HandoffStatus::NotProvided;
static bool s_outputRedirected = false;

struct EarlyPhaseRecord {
  const char* name;
  double elapsedMs;
  double deltaMs;
};
static constexpr size_t kMaxEarlyPhases = 32;
static EarlyPhaseRecord s_earlyPhases[kMaxEarlyPhases];
static size_t s_earlyPhaseCount = 0;
static std::mutex s_bootMutex;
}  // namespace

extern "C" {
void kartpad_boot_log_phase(const char* phase) {
  if (!phase) return;
  const uint64_t now = armGetSystemTick();
  std::lock_guard<std::mutex> lock(s_bootMutex);
  const double elapsedMs = (s_bootEntryTick != 0 && now >= s_bootEntryTick)
      ? static_cast<double>(now - s_bootEntryTick) / 19200.0 : 0.0;
  const double deltaMs = (s_lastPhaseTick != 0 && now >= s_lastPhaseTick)
      ? static_cast<double>(now - s_lastPhaseTick) / 19200.0 : 0.0;
  if (now > s_lastPhaseTick) {
    s_lastPhaseTick = now;
  }

  if (s_outputRedirected) {
    std::fprintf(stderr, "[boot] phase '%s' at +%.2f ms (delta: %.2f ms)\n", phase, elapsedMs, deltaMs);
    std::fflush(stderr);
  } else if (s_earlyPhaseCount < kMaxEarlyPhases) {
    s_earlyPhases[s_earlyPhaseCount++] = { phase, elapsedMs, deltaMs };
  }
}
}


// libnx heap-region globals filled in by __libnx_initheap (weak in libnx).
extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;
}

// Cap the malloc heap so physical memory stays free for the translated runtime's
// guest RAM (GuestFlat's svcCreateSharedMemory) and the GPU.
//
// In Sphaira/hbl application-takeover the loader calls svcSetHeapSize for ~all
// process memory, loads our NRO at the heap base, and hands us the REST of the
// heap -- up to the heap top -- as the malloc override (verified in
// nx-hbloader setupHbHeap: nro_heap_start = heapAddr + nro_size,
// nro_heap_size = heapSize + heapAddr - nro_heap_start). libnx's default maps
// that whole override as the newlib heap, committing nearly all physical
// (hardware: freeMem=0), so svcCreateSharedMemory fails OutOfResource (rc=0x10801).
//
// The loader keeps NO state above our override region -- it ends exactly at the
// heap top, and the loader's own thread stack/TLS live outside the heap (a
// hardware memmap dump confirmed this: heap top == override end, loader stack is
// a separate low mapping). So shrinking the heap TOP with svcSetHeapSize unmaps
// only our own untouched malloc tail and returns that physical to the pool, while
// every byte at and below fake_heap_start stays mapped. Keep fake_heap_start at
// the loader's mapped override base; never move it to svcSetHeapSize's returned
// HeapRegion base (that base holds the NRO image / loader-reserved pages, and
// using it as the malloc base Data-Aborts libnx argvSetup).
extern "C" void __libnx_initheap(void) {
  constexpr u64 kAlign = 0x200000;  // svcSetHeapSize granularity (2 MiB)
  // Physical to keep OUT of the malloc heap: guest banks (~150 MiB) + GPU
  // (Dawn/NVK framebuffers, textures, command buffers) + kernel/shared slack.
  constexpr u64 kFreeForRuntime = 1024ull * 1024 * 1024;  // 1 GiB

  void* addr = nullptr;
  u64 size = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();

    u64 heapBase = 0;
    if (size > kFreeForRuntime + 2 * kAlign &&
        R_SUCCEEDED(svcGetInfo(&heapBase, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0)) &&
        heapBase != 0 && heapBase <= reinterpret_cast<u64>(addr)) {
      const u64 base = reinterpret_cast<u64>(addr);
      const u64 newTop = (base + size - kFreeForRuntime) & ~(kAlign - 1);
      if (newTop > base + kAlign) {
        void* out = nullptr;
        // svcSetHeapSize takes an absolute size from the fixed HeapRegion base.
        if (R_SUCCEEDED(svcSetHeapSize(&out, newTop - heapBase))) {
          size = newTop - base;  // base unchanged; only our tail was freed
        }
      }
    }
  } else {
    // No loader override: we own the whole HeapRegion. Map all-but-reserve.
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    u64 avail = (total > used + kAlign) ? ((total - used - kAlign) & ~(kAlign - 1)) : 0;
    if (avail == 0) avail = 0x2000000u * 16;  // libnx fallback (512 MiB)
    const u64 want = (avail > kFreeForRuntime + kAlign)
                         ? ((avail - kFreeForRuntime) & ~(kAlign - 1))
                         : avail;
    void* out = nullptr;
    if (R_SUCCEEDED(svcSetHeapSize(&out, want))) {
      addr = out;
      size = want;
    } else if (R_SUCCEEDED(svcSetHeapSize(&out, avail))) {
      addr = out;
      size = avail;
    }
  }

  fake_heap_start = static_cast<char*>(addr);
  fake_heap_end = static_cast<char*>(addr) + size;
}

namespace {

extern "C" {
// Result of socketInitializeDefault(); 0xFFFFFFFF = never called (base profile).
uint32_t kartpad_socket_init_rc = 0xFFFFFFFFu;
}

bool InitServices() {
  Result rc;
  if (R_FAILED(rc = romfsInit())) {
    std::printf("[kartpad] romfsInit failed: 0x%x\n", rc);
    // Non-fatal: app data may live only on SD.
  }
#if defined(MKW_PROFILE_RETRO_REWIND) || defined(MKW_ENABLE_RETRO_WFC)
  rc = socketInitializeDefault();
  kartpad_socket_init_rc = rc;  // logged by the network HLE once the runtime log is up
  if (R_FAILED(rc)) {
    std::printf("[kartpad] socketInitializeDefault failed: 0x%x\n", rc);
  }
#endif
  // sdmc is auto-mounted by libnx's fs-dev; nothing to do for sdmc:/ paths.
  return true;
}

void ShutdownServices() { 
#if defined(MKW_PROFILE_RETRO_REWIND) || defined(MKW_ENABLE_RETRO_WFC)
  socketExit(); 
#endif
  romfsExit(); 
}

bool RedirectRuntimeOutputToSd() {
  // consoleExit() releases the framebuffer/NWindow that libnx's software console
  // renders into. stdout/stderr remain bound to that console device unless we
  // explicitly move them first; any later std::cout/std::cerr write then calls
  // ConsoleSwRenderer_drawChar against the released framebuffer and Data Aborts.
  // Keep the runtime diagnostic stream alive on SD before Aurora/Vulkan takes
  // ownership of the default NWindow.
  constexpr const char* kRuntimeLog = "sdmc:/switch/kartpad/kartpad-runtime.log";
  std::fflush(stdout);
  std::fflush(stderr);

  // Do not freopen both streams independently: opening the same libnx fsdev
  // path twice can leave the second stream in an error state. freopen stdout
  // exactly once (so its FILE object is rebound to the SD devoptab), then
  // duplicate that descriptor onto stderr. Rebinding only raw descriptors
  // with open()/dup2() is insufficient for newlib's pre-existing console FILE
  // objects on Horizon and previously produced an empty log.
  auto flushBootPhases = []() {
    std::lock_guard<std::mutex> lock(s_bootMutex);
    s_outputRedirected = true;
    std::fprintf(stderr, "\n[boot] =================== Boot Phase Telemetry ===================\n");
    switch (s_handoffStatus) {
      case HandoffStatus::Valid:
        std::fprintf(stderr, "[boot] launcher handoff latency (wall-clock estimate): %llu ms (launcher=%llu ms, entry=%llu ms)\n",
                     static_cast<unsigned long long>(s_handoffLatencyMs),
                     static_cast<unsigned long long>(mkw_launcher_args::g_launchTsMs),
                     static_cast<unsigned long long>(s_bootEntryEpochMs));
        break;
      case HandoffStatus::FutureTimestamp:
        std::fprintf(stderr, "[boot] launcher handoff latency (wall-clock estimate): unavailable (future timestamp / clock skew; launcher=%llu ms, entry=%llu ms)\n",
                     static_cast<unsigned long long>(mkw_launcher_args::g_launchTsMs),
                     static_cast<unsigned long long>(s_bootEntryEpochMs));
        break;
      case HandoffStatus::ImplausiblyLow:
        std::fprintf(stderr, "[boot] launcher handoff latency (wall-clock estimate): unavailable (implausibly low timestamp < 1.7e12 ms; launcher=%llu ms)\n",
                     static_cast<unsigned long long>(mkw_launcher_args::g_launchTsMs));
        break;
      case HandoffStatus::Stale:
        std::fprintf(stderr, "[boot] launcher handoff latency (wall-clock estimate): unavailable (stale handoff exceeds 10m limit: %llu ms; launcher=%llu ms, entry=%llu ms)\n",
                     static_cast<unsigned long long>(s_handoffLatencyMs),
                     static_cast<unsigned long long>(mkw_launcher_args::g_launchTsMs),
                     static_cast<unsigned long long>(s_bootEntryEpochMs));
        break;
      case HandoffStatus::Malformed:
        std::fprintf(stderr, "[boot] launcher handoff latency (wall-clock estimate): unavailable (malformed --launch-ts; non-digit, sign, or whitespace detected)\n");
        break;
      case HandoffStatus::RangeError:
        std::fprintf(stderr, "[boot] launcher handoff latency (wall-clock estimate): unavailable (range overflow in --launch-ts)\n");
        break;
      case HandoffStatus::NotProvided:
      default:
        std::fprintf(stderr, "[boot] launcher handoff latency: unavailable (direct launch or no --launch-ts)\n");
        break;
    }
    std::fprintf(stderr, "[boot] early phase log (monotonic CNTFRQ_EL0 19.2 MHz):\n");
    for (size_t i = 0; i < s_earlyPhaseCount; ++i) {
      std::fprintf(stderr, "[boot]   phase '%s' at +%.2f ms (delta: %.2f ms)\n",
                   s_earlyPhases[i].name, s_earlyPhases[i].elapsedMs, s_earlyPhases[i].deltaMs);
    }
    std::fprintf(stderr, "[boot] ==========================================================\n\n");
    std::fflush(stderr);
  };

  if (kartpad_log_redirect_stdio(kRuntimeLog)) {
    std::cout.clear();
    std::cerr.clear();
    std::fprintf(stderr, "\n[kartpad] console released; runtime log redirected to async SD sink\n");
    flushBootPhases();
    return true;
  }
  // Fallback: direct (synchronous) SD stream.
  if (std::freopen(kRuntimeLog, "a", stdout) == nullptr) return false;
  if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) return false;
  std::clearerr(stdout);
  std::clearerr(stderr);
  std::cout.clear();
  std::cerr.clear();
  std::fprintf(stderr, "\n[kartpad] console released; runtime log redirected to SD\n");
  std::fflush(stderr);
  flushBootPhases();
  return true;
}

// One-shot diagnostic: dump the full process memory map + region/limit info to
// its OWN SD file, then fclose it immediately so the data reaches the card even
// if the runtime hard-crashes afterward (the shared stdout->SD redirect has
// proven unreliable, coming back empty). This is the data needed to safely free
// physical for guest RAM in the Sphaira/hbl application-takeover environment,
// where the loader keeps live state inside the shared heap region.
void DumpProcessMemoryMap(const char* tag) {
  std::FILE* f = std::fopen("sdmc:/switch/kartpad/memmap.txt", "w");
  if (!f) return;

  auto info = [](InfoType t) -> u64 {
    u64 v = 0;
    svcGetInfo(&v, t, CUR_PROCESS_HANDLE, 0);
    return v;
  };
  const u64 total = info(InfoType_TotalMemorySize);
  const u64 used = info(InfoType_UsedMemorySize);
  const u64 heapBase = info(InfoType_HeapRegionAddress);
  const u64 heapSz = info(InfoType_HeapRegionSize);
  const u64 aliasBase = info(InfoType_AliasRegionAddress);
  const u64 aliasSz = info(InfoType_AliasRegionSize);
  const u64 stackBase = info(InfoType_StackRegionAddress);
  const u64 stackSz = info(InfoType_StackRegionSize);
  const u64 aslrBase = info(InfoType_AslrRegionAddress);
  const u64 aslrSz = info(InfoType_AslrRegionSize);

  std::fprintf(f,
      "[memmap:%s] total=0x%llx used=0x%llx free=%lluMiB applet=%d hasOverride=%d\n",
      tag, (unsigned long long)total, (unsigned long long)used,
      (unsigned long long)((total > used) ? (total - used) >> 20 : 0),
      (int)appletGetAppletType(), (int)envHasHeapOverride());
  if (envHasHeapOverride()) {
    std::fprintf(f, "[memmap:%s] heapOverride=[0x%llx +0x%llx)\n", tag,
                 (unsigned long long)(u64)envGetHeapOverrideAddr(),
                 (unsigned long long)envGetHeapOverrideSize());
  }
  std::fprintf(f,
      "[memmap:%s] heap=[0x%llx +0x%llx) alias=[0x%llx +0x%llx) stack=[0x%llx +0x%llx) aslr=[0x%llx +0x%llx)\n",
      tag, (unsigned long long)heapBase, (unsigned long long)heapSz,
      (unsigned long long)aliasBase, (unsigned long long)aliasSz,
      (unsigned long long)stackBase, (unsigned long long)stackSz,
      (unsigned long long)aslrBase, (unsigned long long)aslrSz);

  // Walk every mapped region across the ASLR address space.
  u64 addr = aslrBase;
  const u64 end = (aslrBase && aslrSz) ? (aslrBase + aslrSz) : (1ull << 39);
  int shown = 0;
  for (int i = 0; i < 8192 && addr < end; ++i) {
    MemoryInfo mi = {};
    u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) break;
    if (mi.type != 0 /*MemType_Unmapped*/ && mi.size != 0) {
      std::fprintf(f, "  [0x%011llx +0x%010llx] type=0x%02x perm=%u attr=0x%x ipc=%u dev=%u\n",
                   (unsigned long long)mi.addr, (unsigned long long)mi.size,
                   (unsigned)(mi.type & 0xff), (unsigned)mi.perm, (unsigned)mi.attr,
                   (unsigned)mi.ipc_refcount, (unsigned)mi.device_refcount);
      ++shown;
    }
    const u64 next = mi.addr + mi.size;
    if (next <= addr) break;
    addr = next;
  }
  std::fprintf(f, "[memmap:%s] end (%d mapped regions)\n", tag, shown);
  std::fflush(f);
  fsync(fileno(f));
  std::fclose(f);
}

// Generated per link by cmake/BuildStamp.cmake; weak for the fixture targets.
extern "C" const char kartpad_build_stamp[] __attribute__((weak));
extern "C" void kartpad_prof_start_guest(void);
// Aurora GX decode worker (aurora-main/lib/gx/gx_worker.cpp); perf.cfg `gx_thread 1|2`.
extern "C" void aurora_gx_worker_configure(int enabled);

// First runtime-log line: names the exact binary that ran, so a stale NRO on the
// card can't masquerade as a new build.
#ifndef KARTPAD_PGO_TAG
#define KARTPAD_PGO_TAG "off"
#endif
#ifndef KARTPAD_CPU_OPT_TAG
#define KARTPAD_CPU_OPT_TAG "baseline"
#endif
#ifndef KARTPAD_ORDER_TAG
#define KARTPAD_ORDER_TAG "off"
#endif

void LogBuildIdentity() {
  // pgo= names the sample profile the translated shards were built with (FullApp.cmake), so the
  // PGO and plain NROs are told apart in every log.
  std::fprintf(stderr, "[build] %s (main_switch compiled " __DATE__ " " __TIME__ ") perf.cfg profile=%s pgo=%s order=%s cpu=%s\n",
               kartpad_build_stamp ? kartpad_build_stamp : "KARTPAD-BUILD unknown",
               kartpad_perf_tuned() ? "tuned" : "legacy", KARTPAD_PGO_TAG, KARTPAD_ORDER_TAG, KARTPAD_CPU_OPT_TAG);
}

// Provided by the linked translated runtime. Returns process exit code. Kept
// weak so the lightweight shim/bring-up target can still link without fullapp.
extern "C" int KartPadRuntimeMain(const char* profile) __attribute__((weak));

#if defined(MKW_PROFILE_RETRO_REWIND)
constexpr const char* kBuiltVariant = "Retro Rewind";
#else
constexpr const char* kBuiltVariant = "Vanilla";
#endif

void RecordBootEntry() {
  s_bootEntryTick = armGetSystemTick();
  s_lastPhaseTick = s_bootEntryTick;
  const auto entryNow = std::chrono::system_clock::now();
  s_bootEntryEpochMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(entryNow.time_since_epoch()).count());
  kartpad_boot_log_phase("main_entry");
}

// Launcher arguments: --variant=<name>, --return=<launcher NRO>, --launch-ts=<epoch ms>.
// --profile is accepted and ignored: each NRO is built for one profile.
void ParseLaunchArguments(int argc, char** argv) {
  const char* launchTsArg = nullptr;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg.starts_with("--variant=")) {
      mkw_launcher_args::g_overrideVariant = arg.substr(10);
    } else if (arg.starts_with("--return=")) {
      mkw_launcher_args::g_returnPath = arg.substr(9);
    } else if (arg.starts_with("--launch-ts=")) {
      launchTsArg = argv[i] + 12;
    }
  }

  if (launchTsArg != nullptr) {
    ParseLaunchTimestamp(launchTsArg, s_bootEntryEpochMs,
                         mkw_launcher_args::g_launchTsMs, s_handoffLatencyMs,
                         s_handoffStatus);
  } else {
    s_handoffStatus = HandoffStatus::NotProvided;
    mkw_launcher_args::g_launchTsMs = 0;
    s_handoffLatencyMs = 0;
  }
  kartpad_boot_log_phase("argv_parsed");

  if (!mkw_launcher_args::g_overrideVariant.empty() && mkw_launcher_args::g_overrideVariant != kBuiltVariant) {
    std::printf("[kartpad] Warning: --variant=%s does not match built variant %s\n",
                mkw_launcher_args::g_overrideVariant.c_str(), kBuiltVariant);
  }
}

// Keeps a message on the text console until the user presses +.
void WaitForPlusOnConsole() {
  consoleUpdate(nullptr);
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
  }
}

// Legacy placement: main thread preferring core 1, allowed on cores 1+2. The tuned perf.cfg
// profile replaces it after the log redirect (kartpad_thread_apply).
void ApplyLegacyCoreMask() {
  u64 allowedCores = 0;
  if (!kartpad_perf_tuned() &&
      R_SUCCEEDED(svcGetInfo(&allowedCores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && allowedCores != 0) {
    s32 pref = (allowedCores & (1u << 1)) ? 1 : 0;
    u32 mask = (allowedCores > 1) ? static_cast<u32>(allowedCores & ~1ull) : static_cast<u32>(allowedCores);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, pref, mask);
  }
}

// When the launcher started us, load it again when this process exits.
void LoadLauncherNext() {
  if (mkw_launcher_args::g_returnPath.empty()) return;
  std::string argv_str = "\"" + mkw_launcher_args::g_returnPath + "\"";
  if (mkw_launcher_args::g_returnPath.size() < 512 && argv_str.size() < 2048) {
    envSetNextLoad(mkw_launcher_args::g_returnPath.c_str(), argv_str.c_str());
  }
}

// Runs the translated game. The text console is released here, after stdout/stderr moved to the
// SD log: consoleExit frees the framebuffer but leaves the streams on the console devoptab, and
// the first hardware boot crashed when RuntimeMain's config logger then wrote through std::cout.
// False when the SD log could not be opened; the console and services are then already shut down.
bool RunGame(const char* profile, int& code) {
  ApplyLegacyCoreMask();
  if (!RedirectRuntimeOutputToSd()) {
    // Do not enter the runtime with streams still pointing at a dead console. Leave the
    // visible console up so the failure itself remains diagnosable.
    std::printf("[kartpad] Failed to open sdmc:/switch/kartpad/kartpad-runtime.log\n");
    std::printf("Press + to exit.\n");
    WaitForPlusOnConsole();
    ShutdownServices();
    consoleExit(nullptr);
    return false;
  }
  consoleExit(nullptr);  // Hand the framebuffer to the renderer.
  kartpad_boot_log_phase("console_released");
  LogBuildIdentity();
  kartpad_thread_apply(KARTPAD_ROLE_GUEST, 0);
  kartpad_prof_start_guest();
  // Default on since b19 (b18/r14: race 46 -> ~33 ms). `gx_thread 0` or `opt_safe 1` disables it.
  aurora_gx_worker_configure(kartpad_opt("gx_thread", 1) > 0 ? 1 : 0);
  kartpad_boot_log_phase("runtime_main_call");
  code = KartPadRuntimeMain(profile);
  kartpad_boot_log_phase("runtime_main_returned");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  RecordBootEntry();
  ParseLaunchArguments(argc, argv);

  // Also retains the module's early constructor when this main is linked from
  // the shim archive. The constructor handles TLS used before main starts.
  kartpad_sync_clang_tls();
  consoleInit(nullptr);  // Text console for early messages; the renderer takes over.
  InitServices();
  kartpad_boot_log_phase("services_init");

  // Diagnostic: capture the process memory map to its own SD file up front.
  // Gated behind perf.cfg toggle (boot_dump_memmap or dump_memmap, default 0) so normal boots don't stall.
  if (kartpad_opt("boot_dump_memmap", kartpad_opt("dump_memmap", 0)) != 0) {
    DumpProcessMemoryMap("preruntime");
    kartpad_boot_log_phase("preruntime_memmap_dumped");
  } else {
    kartpad_boot_log_phase("preruntime_memmap_skipped");
  }

  if (!KartPadMobileEnsureGameDataAvailable()) {
    std::printf("[kartpad] Game data unavailable. Extract your dump and set dvd_root first.\n");
    std::printf("Press + to exit.\n");
    WaitForPlusOnConsole();
    ShutdownServices();
    consoleExit(nullptr);
    return 0;
  }
  kartpad_boot_log_phase("gamedata_verified");

  // Player 1 needs a controller: the system controller applet when none is assigned yet.
  KartPadEnsureControllerAtStart();
  kartpad_boot_log_phase("controllers_checked");

  const char* profile = KartPadMobileSelectedRuntimeProfile();
  std::printf("[kartpad] Launching profile: %s\n", profile);
  consoleUpdate(nullptr);

  int code = 0;
  if (KartPadRuntimeMain) {
    if (!RunGame(profile, code)) return 1;
  } else {
    std::printf("[kartpad] Runtime entry unavailable in this build. Press + to exit.\n");
    WaitForPlusOnConsole();
    consoleExit(nullptr);
  }

  kartpad_log_shutdown();
  ShutdownServices();
  LoadLauncherNext();
  return code;
}

extern "C" void kartpad_request_return_to_launcher() {
  // Every _Exit path (Quit to launcher, closing from HOME) runs this: stop the async log
  // writer first so it is not mid-write when the process tears down.
  kartpad_log_shutdown();
  LoadLauncherNext();
}

extern "C" bool kartpad_has_return_path() {
  return !mkw_launcher_args::g_returnPath.empty();
}

extern "C" const char* kartpad_get_variant() { return kBuiltVariant; }
