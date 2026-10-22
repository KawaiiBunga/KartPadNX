// perf.cfg experiment knobs and thread placement (ported from melee-nx).
//
// melee-nx's decisive recovery fix was keeping the pipeline compiler off the
// game thread's core: a cold vkCreateGraphicsPipeline occupies its core for
// hundreds of ms, and KartPad ran three compile workers across every
// application core, next to a guest thread that was itself allowed to migrate
// between cores 1 and 2. The tuned profile gives the guest thread core 1 alone
// and puts every other thread on the remaining application cores.
//
// sdmc:/switch/kartpad/perf.cfg, one `key value` per line, `#` comments; values accept decimal or
// 0x hex; read once at startup. Every key the code reads is listed, with its default, in
// docs/PERF-CFG-KEYS.md (builder/list-config-keys.py). The placement keys read here:
//   profile tuned|legacy   legacy = the previous placement, for A/B runs
//   guest_mask 0x2         core mask for the guest (main) thread
//   worker_mask 0x5        core mask for everything else (default: allowed & ~guest)

#include "perf_switch.h"

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
std::atomic<int> kartpad_indirect_profile_enabled{0};
}

namespace {

constexpr const char* kConfigPath = "sdmc:/switch/kartpad/perf.cfg";
constexpr int kMaxEntries = 64;

struct Entry {
  char key[32];
  long value;
};

struct Config {
  Entry entries[kMaxEntries];
  int count = 0;
  bool legacy = false;
  bool loaded = false;
} g_cfg;

std::once_flag g_once;

// A sampled, bounded callsite/target histogram for finding monomorphic guest
// virtual calls. Disabled by default; one in 128 indirect calls reaches this
// table when perf.cfg has indirect_profile 1.
struct IndirectProfileSlot { u64 key = 0; u32 count = 0; };
constexpr size_t kIndirectProfileSlots = 8192;
IndirectProfileSlot g_indirectSlots[kIndirectProfileSlots];
std::mutex g_indirectMutex;
u32 g_indirectSamples = 0;
u32 g_indirectDropped = 0;
u32 g_indirectReportCalls = 0;

// perf.cfg eabi_verify (b73): InvokeIndirectCpuEabiChecked (abi_bridge.h) reports every checked
// indirect call here. A violation means some callee returned with r1, r2 or r13-r31 changed, which
// translated callers no longer reload; the first kEabiLogLimit distinct sites are logged.
std::atomic<u64> g_eabiChecks{0};
u64 g_eabiBudget = 0;  // checks before the check turns itself off (perf.cfg eabi_verify N = N million)
std::atomic<u64> g_eabiViolations{0};
constexpr int kEabiLogLimit = 64;
std::mutex g_eabiMutex;
u64 g_eabiLogged[kEabiLogLimit];
int g_eabiLoggedCount = 0;

void report_eabi_verify() {
  if ((kartpad_indirect_profile_enabled.load(std::memory_order_relaxed) & 2) == 0) return;
  std::fprintf(stderr, "[eabi] checked=%llu violations=%llu sites=%d\n",
               static_cast<unsigned long long>(g_eabiChecks.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(g_eabiViolations.load(std::memory_order_relaxed)),
               g_eabiLoggedCount);
}

void report_indirect_profile() {
  if (++g_indirectReportCalls % 10u == 0u) report_eabi_verify();
  if ((kartpad_indirect_profile_enabled.load(std::memory_order_relaxed) & 1) == 0 ||
      g_indirectReportCalls % 10u != 0u) return;
  IndirectProfileSlot top[16]{};
  u32 occupied = 0;
  u32 samples = 0;
  u32 dropped = 0;
  {
    std::lock_guard lock(g_indirectMutex);
    samples = g_indirectSamples;
    dropped = g_indirectDropped;
    for (const auto& slot : g_indirectSlots) {
      if (slot.key == 0) continue;
      ++occupied;
      if (slot.count <= top[15].count) continue;
      int index = 15;
      while (index > 0 && slot.count > top[index - 1].count) {
        top[index] = top[index - 1];
        --index;
      }
      top[index] = slot;
    }
  }
  std::fprintf(stderr, "[indirect] sampled=%u pairs=%u dropped=%u rate=1/128\n",
               samples, occupied, dropped);
  for (const auto& slot : top) {
    if (slot.count == 0) break;
    std::fprintf(stderr, "[indirect] caller=%08x target=%08x samples=%u\n",
                 static_cast<u32>(slot.key >> 32), static_cast<u32>(slot.key), slot.count);
  }
}

void load_config() {
  std::FILE* f = std::fopen(kConfigPath, "r");
  if (f == nullptr) {
    std::fprintf(stderr, "[perfcfg] %s absent; using defaults (profile tuned)\n", kConfigPath);
    g_cfg.loaded = true;
    return;
  }
  char line[128];
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    char* hash = std::strchr(line, '#');
    if (hash != nullptr) *hash = '\0';
    char key[32];
    char value[64];
    if (std::sscanf(line, "%31s %63s", key, value) != 2) continue;
    if (std::strcmp(key, "profile") == 0) {
      g_cfg.legacy = std::strcmp(value, "legacy") == 0;
      std::fprintf(stderr, "[perfcfg] profile %s\n", g_cfg.legacy ? "legacy" : "tuned");
      continue;
    }
    if (g_cfg.count == kMaxEntries) break;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 0);
    if (end == value) continue;
    Entry& e = g_cfg.entries[g_cfg.count++];
    std::snprintf(e.key, sizeof(e.key), "%s", key);
    e.value = parsed;
    std::fprintf(stderr, "[perfcfg] %s %ld\n", e.key, e.value);
  }
  std::fclose(f);
  for (int i = 0; i < g_cfg.count; ++i) {
    if (std::strcmp(g_cfg.entries[i].key, "indirect_profile") == 0 &&
        g_cfg.entries[i].value != 0) {
      kartpad_indirect_profile_enabled.fetch_or(1, std::memory_order_relaxed);
      std::fprintf(stderr, "[indirect] callsite profiler enabled (1/128 calls)\n");
    }
    if (std::strcmp(g_cfg.entries[i].key, "eabi_verify") == 0 && g_cfg.entries[i].value > 0) {
      g_eabiBudget = static_cast<u64>(g_cfg.entries[i].value) * 1000000ull;
      kartpad_indirect_profile_enabled.fetch_or(2, std::memory_order_relaxed);
      std::fprintf(stderr, "[eabi] indirect-call nonvolatile GPR check enabled for %ld million calls\n",
                   g_cfg.entries[i].value);
    }
  }
  g_cfg.loaded = true;
}

const Config& config() {
  std::call_once(g_once, load_config);
  return g_cfg;
}

u64 allowed_cores() {
  static const u64 mask = [] {
    u64 m = 0;
    if (R_FAILED(svcGetInfo(&m, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || m == 0) m = 0x7;
    return m;
  }();
  return mask;
}

int popcount(u64 v) {
  int n = 0;
  for (; v != 0; v &= v - 1) ++n;
  return n;
}

int lowest_core(u64 mask) {
  for (int i = 0; i < 64; ++i)
    if (mask & (u64{1} << i)) return i;
  return 0;
}

int highest_core(u64 mask) {
  for (int i = 63; i >= 0; --i)
    if (mask & (u64{1} << i)) return i;
  return 0;
}

u64 guest_mask() {
  const u64 allowed = allowed_cores();
  u64 m = static_cast<u64>(kartpad_perf_int("guest_mask", 0x2)) & allowed;
  if (m == 0) m = u64{1} << lowest_core(allowed);
  return m;
}

u64 worker_mask() {
  const u64 allowed = allowed_cores();
  u64 m = static_cast<u64>(kartpad_perf_int("worker_mask", 0)) & allowed;
  if (m == 0) m = allowed & ~guest_mask();
  if (m == 0) m = allowed;  // single-core process: nothing to separate
  return m;
}

// Compile workers alternate from the highest worker core downwards so the
// first one lands away from the frame worker/presenter on the lowest core.
int compiler_core(u64 workers, int index) {
  const int n = popcount(workers);
  if (n <= 1) return lowest_core(workers);
  int skip = index % n;
  for (int i = 63; i >= 0; --i) {
    if (!(workers & (u64{1} << i))) continue;
    if (skip-- == 0) return i;
  }
  return highest_core(workers);
}

const char* role_name(int role) {
  switch (role) {
    case KARTPAD_ROLE_GUEST: return "guest";
    case KARTPAD_ROLE_FRAME_WORKER: return "frame_worker";
    case KARTPAD_ROLE_PRESENTER: return "presenter";
    case KARTPAD_ROLE_COMPILER: return "compiler";
    case KARTPAD_ROLE_AUDIO_MIX: return "audio_mix";
    case KARTPAD_ROLE_GX_WORKER: return "gx_worker";

    case KARTPAD_ROLE_GX_HLE: return "gx_hle";
    case KARTPAD_ROLE_SND_GUEST: return "snd_guest";
    default: return "default";
  }
}

// Threads seen by kartpad_thread_apply, for the per-thread CPU% report.
struct TrackedThread {
  int role;
  int index;
  Handle handle;
  u64 lastTicks;
  bool used;
};
constexpr int kMaxTracked = 48;
TrackedThread g_tracked[kMaxTracked];
std::mutex g_trackedMutex;
u64 g_lastReportTick = 0;

u64 thread_ticks(Handle h, bool* ok) {
  u64 ticks = 0;
  *ok = R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, static_cast<u64>(-1)));
  return ticks;
}

void track_current_thread(int role, int index) {
  const Handle h = threadGetCurHandle();
  bool ok = false;
  const u64 ticks = thread_ticks(h, &ok);
  std::lock_guard lock(g_trackedMutex);
  for (auto& t : g_tracked) {
    if (t.used && t.handle == h) {
      t.role = role;
      t.index = index;
      return;
    }
  }
  for (auto& t : g_tracked) {
    if (!t.used) {
      t = {role, index, h, ok ? ticks : 0, true};
      return;
    }
  }
}

}  // namespace

extern "C" void kartpad_eabi_record(u32 target, u32 returnAddress, u32 changedMask, const u32* before,
                                    const u32* after) {
  const u64 checks = g_eabiChecks.fetch_add(1, std::memory_order_relaxed) + 1;
  if (checks == g_eabiBudget) {
    kartpad_indirect_profile_enabled.fetch_and(~2, std::memory_order_relaxed);
    std::fprintf(stderr, "[eabi] check finished: %llu calls, %llu violations\n",
                 static_cast<unsigned long long>(checks),
                 static_cast<unsigned long long>(g_eabiViolations.load(std::memory_order_relaxed)));
  }
  if (changedMask == 0) return;
  g_eabiViolations.fetch_add(1, std::memory_order_relaxed);
  const u64 key = (static_cast<u64>(returnAddress) << 32) | target;
  std::lock_guard lock(g_eabiMutex);
  for (int i = 0; i < g_eabiLoggedCount; ++i)
    if (g_eabiLogged[i] == key) return;
  if (g_eabiLoggedCount == kEabiLogLimit) return;
  g_eabiLogged[g_eabiLoggedCount++] = key;
  const int reg = __builtin_ctz(changedMask);
  std::fprintf(stderr, "[eabi] VIOLATION target=%08x return=%08x changed=%08x r%d %08x->%08x\n", target,
               returnAddress, changedMask, reg, before[reg], after[reg]);
}

extern "C" void kartpad_indirect_profile_record(u32 caller, u32 target) {
  const u64 key = (static_cast<u64>(caller) << 32) | target;
  if (key == 0) return;
  u64 hash = key ^ (key >> 33);
  hash *= 0xff51afd7ed558ccdULL;
  hash ^= hash >> 33;
  std::lock_guard lock(g_indirectMutex);
  ++g_indirectSamples;
  const size_t start = static_cast<size_t>(hash) & (kIndirectProfileSlots - 1);
  for (size_t probe = 0; probe < 32; ++probe) {
    auto& slot = g_indirectSlots[(start + probe) & (kIndirectProfileSlots - 1)];
    if (slot.key == 0) slot.key = key;
    if (slot.key == key) {
      ++slot.count;
      return;
    }
  }
  ++g_indirectDropped;
}

namespace {
// clkrst sessions shared by the 1 Hz clock report and the CPU boost (two threads).
struct Clocks {
  std::mutex mutex;
  int state = 0;  // 0 untried, 1 ready, -1 unavailable
  ClkrstSession cpu{}, gpu{}, emc{};
  bool ready() {  // caller holds mutex
    if (state == 0) {
      state = (R_SUCCEEDED(clkrstInitialize()) &&
               R_SUCCEEDED(clkrstOpenSession(&cpu, PcvModuleId_CpuBus, 3)) &&
               R_SUCCEEDED(clkrstOpenSession(&gpu, PcvModuleId_GPU, 3)) &&
               R_SUCCEEDED(clkrstOpenSession(&emc, PcvModuleId_EMC, 3)))
                  ? 1
                  : -1;
    }
    return state == 1;
  }
};
Clocks& clocks() {
  static Clocks c;
  return c;
}
constexpr u32 kBoostCpuHz = 1785000000u;
std::atomic<int> g_cpuBoost{-1};  // -1 never set, 0 off, 1 on
std::atomic<bool> g_bootBoost{false};
u32 g_cpuRestoreHz = 0;            // under clocks().mutex: the rate before the boost

void apply_cpu_boost_locked(Clocks& c) {
  int want = g_cpuBoost.load(std::memory_order_relaxed);
  if (g_bootBoost.load(std::memory_order_relaxed)) want = 1;
  else if (want < 0 && g_cpuRestoreHz != 0) want = 0;  // the boot boost ended; no port-menu setting
  if (want < 0 || !c.ready()) return;
  u32 hz = 0;
  if (R_FAILED(clkrstGetClockRate(&c.cpu, &hz))) return;
  if (want == 1) {
    // Never lower a faster rate someone else (sys-clk) chose.
    if (hz < kBoostCpuHz) {
      if (g_cpuRestoreHz == 0) g_cpuRestoreHz = hz;
      clkrstSetClockRate(&c.cpu, kBoostCpuHz);
    }
  } else if (g_cpuRestoreHz != 0) {
    if (hz == kBoostCpuHz) clkrstSetClockRate(&c.cpu, g_cpuRestoreHz);
    g_cpuRestoreHz = 0;
  }
}
}  // namespace

extern "C" void kartpad_set_cpu_boost(int on) {
  const int previous = g_cpuBoost.exchange(on ? 1 : 0, std::memory_order_relaxed);
  Clocks& c = clocks();
  std::lock_guard lock(c.mutex);
  apply_cpu_boost_locked(c);
  if (previous != (on ? 1 : 0)) {
    std::fprintf(stderr, "[clk] cpu boost %s (clkrst %s)\n", on ? "on: 1785 MHz" : "off",
                 c.state == 1 ? "ok" : "unavailable");
  }
}

extern "C" void kartpad_set_boot_cpu_boost(int on) {
  g_bootBoost.store(on != 0, std::memory_order_relaxed);
  Clocks& c = clocks();
  std::lock_guard lock(c.mutex);
  apply_cpu_boost_locked(c);
  std::fprintf(stderr, "[clk] boot preload boost %s (clkrst %s)\n", on ? "on: 1785 MHz" : "off",
               c.state == 1 ? "ok" : "unavailable");
}

// Writes "name#i=NN% ..." for every tracked thread since the previous call.
// Default-role threads (IO writers, Dawn/NVK helpers) are listed only above 2%.
extern "C" int kartpad_perf_thread_cpu(char* buf, int size) {
  if (buf == nullptr || size <= 0) return 0;
  buf[0] = '\0';
  report_indirect_profile();
  const u64 now = armGetSystemTick();
  std::lock_guard lock(g_trackedMutex);
  const u64 elapsed = g_lastReportTick ? now - g_lastReportTick : 0;
  g_lastReportTick = now;
  int used = 0;
  for (auto& t : g_tracked) {
    if (!t.used) continue;
    bool ok = false;
    const u64 ticks = thread_ticks(t.handle, &ok);
    if (!ok) {
      t.used = false;  // thread exited; its handle is gone
      continue;
    }
    const u64 delta = ticks - t.lastTicks;
    t.lastTicks = ticks;
    if (elapsed == 0) continue;
    const double pct = 100.0 * static_cast<double>(delta) / static_cast<double>(elapsed);
    if (t.role == KARTPAD_ROLE_DEFAULT && pct < 2.0) continue;
    const int n = std::snprintf(buf + used, static_cast<size_t>(size - used), "%s%s#%d=%.0f%%",
                                used ? " " : "", role_name(t.role), t.index, pct);
    if (n < 0 || n >= size - used) break;
    used += n;
  }
  // Clocks, so every log records whether an overclock was active (a 1.7x
  // CPU clock difference once masqueraded as a regression). The CPU boost is re-applied here.
  Clocks& c = clocks();
  std::lock_guard clockLock(c.mutex);
  apply_cpu_boost_locked(c);
  u32 cpuHz = 0, gpuHz = 0, emcHz = 0;
  if (c.ready() && R_SUCCEEDED(clkrstGetClockRate(&c.cpu, &cpuHz)) &&
      R_SUCCEEDED(clkrstGetClockRate(&c.gpu, &gpuHz)) && R_SUCCEEDED(clkrstGetClockRate(&c.emc, &emcHz))) {
    const int n = std::snprintf(buf + used, static_cast<size_t>(size - used), " | clk MHz cpu=%u gpu=%u emc=%u",
                                cpuHz / 1000000u, gpuHz / 1000000u, emcHz / 1000000u);
    if (n > 0 && n < size - used) used += n;
  }
  return used;
}

extern "C" int kartpad_perf_int(const char* key, int fallback) {
  const Config& c = config();
  for (int i = 0; i < c.count; ++i)
    if (std::strcmp(c.entries[i].key, key) == 0) return static_cast<int>(c.entries[i].value);
  return fallback;
}

extern "C" int kartpad_perf_tuned(void) { return config().legacy ? 0 : 1; }

// Optimization switches (docs/PERF-60FPS-ROADMAP.md, "Optimization switches"). An explicit
// perf.cfg entry wins; otherwise `opt_safe 1` forces 0 (the pre-optimization path) and the
// caller's default applies. Every key is logged once, the first time it is read, so a crash log
// always shows which optimizations were live.
extern "C" int kartpad_opt(const char* key, int fallback) {
  const Config& c = config();
  int value = fallback;
  const char* source = "default";
  bool explicitValue = false;
  for (int i = 0; i < c.count; ++i) {
    if (std::strcmp(c.entries[i].key, key) == 0) {
      value = static_cast<int>(c.entries[i].value);
      source = "cfg";
      explicitValue = true;
      break;
    }
  }
  if (!explicitValue && kartpad_perf_int("opt_safe", 0) != 0) {
    value = 0;
    source = "opt_safe";
  }
  static std::mutex logMutex;
  static char logged[kMaxEntries][32];
  static int loggedCount = 0;
  std::lock_guard lock(logMutex);
  for (int i = 0; i < loggedCount; ++i)
    if (std::strcmp(logged[i], key) == 0) return value;
  if (loggedCount < kMaxEntries) std::snprintf(logged[loggedCount++], sizeof(logged[0]), "%s", key);
  std::fprintf(stderr, "[opt] %s=%d (%s, default %d)\n", key, value, source, fallback);
  return value;
}

extern "C" int kartpad_thread_priority(int role, int fallback) {
  if (!kartpad_perf_tuned()) return fallback;
  switch (role) {
    case KARTPAD_ROLE_GUEST: return 0x2C;         // libnx main-thread default
    // Below gx_hle and the GX/frame workers (b71). With hosted AX ticks (default) only the hosted
    // sound thread joins the mix, outside the interrupt lock, from the other worker core (the split
    // helper is off then, ax_mix.cpp), so mix latency never stalls the game core; at 0x24 it took
    // 20-25% of gx_hle's core at a higher priority (b70: the game waited 2% at GXDrawDone). The
    // ~30 ms audio queue absorbs the delay. perf.cfg mix_prio (0x24 = before b71).
    case KARTPAD_ROLE_AUDIO_MIX: return kartpad_perf_int("mix_prio", 0x2C);
    case KARTPAD_ROLE_PRESENTER: return 0x2A;
    case KARTPAD_ROLE_FRAME_WORKER: return 0x2B;
    case KARTPAD_ROLE_GX_WORKER: return 0x2B;     // the guest joins it at frame end

    case KARTPAD_ROLE_GX_HLE: return 0x29;        // the guest joins it at GXDrawDone, scheduled faster than GX decode worker
    case KARTPAD_ROLE_COMPILER: return 0x2E;      // on-demand; background drops lower
    // Above the GX/frame workers, pinned to its own core (b71): it holds the interrupt lock in its EE=0
    // sections and AX callbacks, and the game core waits whenever it needs the lock then. At 0x2D
    // (b70) the workers preempted it mid-hold: il guest rose to 26-66 ms/s. perf.cfg snd_prio.
    case KARTPAD_ROLE_SND_GUEST: return kartpad_perf_int("snd_prio", 0x28);
    default: return fallback;
  }
}

extern "C" void kartpad_prof_register_thread(int role);

extern "C" int kartpad_thread_apply(int role, int index) {
  track_current_thread(role, index);
  kartpad_prof_register_thread(role);
  if (!kartpad_perf_tuned()) return 0;

  u64 mask = 0;
  int ideal = 0;
  switch (role) {
    case KARTPAD_ROLE_GUEST:
      mask = guest_mask();
      ideal = lowest_core(mask);
      break;
    case KARTPAD_ROLE_COMPILER: {
      // One core per compiler, never the guest's: melee-nx's recovery fix.
      const u64 workers = worker_mask();
      ideal = compiler_core(workers, index);
      mask = u64{1} << ideal;
      break;
    }
    case KARTPAD_ROLE_GX_HLE:
      // The highest worker core: the frame worker, GX decode worker and presenter take the lowest,
      // and r42 showed the highest one nearly idle (audio mix ~8%).
      mask = worker_mask();
      ideal = highest_core(mask);
      mask = u64{1} << ideal;
      break;
    case KARTPAD_ROLE_SND_GUEST: {
      // The lowest worker core, with the frame/GX decode workers: gx_hle on the highest is on the
      // guest's GXDrawDone critical path (b59: 44 ms/s of guest waits), so it must not be
      // preempted. Both worker cores stay allowed unless perf.cfg snd_pin is set; snd_core picks
      // the ideal core.
      mask = worker_mask();
      const int core = kartpad_perf_int("snd_core", -1);
      ideal = (core >= 0 && core < 64 && (mask & (u64{1} << core))) ? core : lowest_core(mask);
      // Pinned by default from b71, so it never migrates onto gx_hle's core (perf.cfg snd_pin 0).
      if (kartpad_perf_int("snd_pin", 1) != 0) mask = u64{1} << ideal;
      break;
    }
    case KARTPAD_ROLE_AUDIO_MIX:
      // Index 0 (the mix worker) takes the highest worker core, index 1 (the ax_mix_split helper)
      // the lowest, so the two halves of a mix run in parallel.
      mask = worker_mask();
      ideal = index == 0 ? highest_core(mask) : lowest_core(mask);
      mask = u64{1} << ideal;
      break;
    default:
      mask = worker_mask();
      ideal = lowest_core(mask);
      break;
  }

  const Result rcMask = svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal, static_cast<u32>(mask));
  Result rcPrio = 0;
  const int prio = kartpad_thread_priority(role, -1);
  if (prio >= 0) rcPrio = svcSetThreadPriority(CUR_THREAD_HANDLE, prio);

  s32 effIdeal = -1;
  u64 effMask = 0;
  svcGetThreadCoreMask(&effIdeal, &effMask, CUR_THREAD_HANDLE);
  s32 effPrio = -1;
  svcGetThreadPriority(&effPrio, CUR_THREAD_HANDLE);
  // Default-role threads are numerous (Dawn/NVK/IO); log them briefly.
  std::fprintf(stderr,
               "[THREAD] role=%s#%d handle=%08x mask=0x%llx ideal=%d prio=%d rc=%08x/%08x eff_mask=0x%llx "
               "eff_ideal=%d eff_prio=%d\n",
               role_name(role), index, threadGetCurHandle(), static_cast<unsigned long long>(mask), ideal, prio,
               rcMask, rcPrio, static_cast<unsigned long long>(effMask), effIdeal, effPrio);
  return 1;
}

// Called by the pthread_create trampoline (clang_tls_switch.c) for every new
// thread before its entry point runs, so no stray thread lands on the guest core.
extern "C" void kartpad_thread_default_placement(void) {
  static std::atomic<int> s_index{0};
  kartpad_thread_apply(KARTPAD_ROLE_DEFAULT, s_index.fetch_add(1, std::memory_order_relaxed));
}
