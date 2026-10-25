// Sampling profiler for the guest thread on Horizon.
//
// Started/stopped manually from the Port Menu. A high-priority thread
// on a worker core pauses the guest thread N times per second (plus any thread
// whose role bit is set in `prof_roles`, e.g. 16 = audio mix, 64 = GX worker),
// tags each record with the role in bits 8..15 of `frames`, reads its
// registers with svcGetThreadContext3, walks any intact frame-pointer chain
// inside the guest stack mapping, and resumes it. Records go through a ring
// and are appended to sdmc:/switch/kartpad/prof.bin every second, so a run is
// covered end to end (~64 KB/s per sampled thread at 1 kHz). prof_hz selects
// the manual rate (0/absent = 750 Hz); launch timers never start a capture.
// builder/prof-report.py symbolizes the file against kartpad_full.elf.
//
// Nothing here touches the guest's locks or the heap while the guest is
// paused: the pause window is register read + bounded stack reads only.

#include "perf_switch.h"
#include "prof_control.h"
#include "clang_tls_switch.h"

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* kProfPath = "sdmc:/switch/kartpad/prof.bin";
constexpr int kFrames = 5;
constexpr size_t kRingRecords = 65536;  // 4 MiB ring, ~20 s of 3 threads at 1 kHz
constexpr u64 kFlushNs = 1'000'000'000ull;

struct Record {
  u32 tMs;
  u32 frames;  // bits 0..7 frame count, bits 8..15 thread role (0 = guest)
  u64 pc;
  u64 lr;
  u64 fr[kFrames];
};
static_assert(sizeof(Record) == 64, "record layout is read by prof-report.py");

struct Header {
  char magic[8];     // "KPPROF1\0"
  u64 anchor;        // runtime address of kartpad_prof_anchor
  u32 hz;
  u32 recordSize;
  char stamp[48];
};

Record* g_records = nullptr;
u64 g_count = 0;    // records produced (ring index = count % kRingRecords)
u64 g_written = 0;  // records appended to the file
u64 g_dropped = 0;
struct Target {
  Handle handle = INVALID_HANDLE;
  u32 role = 0;
  u64 stackLo = 0, stackHi = 0;
};
constexpr int kMaxTargets = KARTPAD_ROLE_COUNT;
Target g_targets[kMaxTargets];  // [0] is the guest thread
std::atomic<int> g_targetCount{0};
std::atomic<u32> g_roleMask{0};
std::atomic<bool> g_targetBusy{false};
Thread g_thread{};
u32 g_hz = 0;
UEvent g_command{};
std::atomic<int> g_state{KARTPAD_PROF_UNAVAILABLE};
std::atomic<bool> g_requested{false};
std::atomic<u64> g_startedNs{0}, g_elapsedNs{0}, g_samples{0}, g_dropCount{0};

u64 now_ns() { return armTicksToNs(armGetSystemTick()); }

bool stack_range(Target& t, u64 sp) {
  if (sp >= t.stackLo && sp < t.stackHi) return true;
  MemoryInfo info{};
  u32 page = 0;
  if (R_FAILED(svcQueryMemory(&info, &page, sp))) return false;
  if ((info.perm & Perm_R) == 0) return false;
  t.stackLo = info.addr;
  t.stackHi = info.addr + info.size;
  return true;
}

void sample(Target& t, u64 t0) {
  if (R_FAILED(svcSetThreadActivity(t.handle, ThreadActivity_Paused))) return;
  ThreadContext ctx;
  const Result rc = svcGetThreadContext3(&ctx, t.handle);
  Record r{};
  u32 frames = 0;
  if (R_SUCCEEDED(rc)) {
    r.tMs = static_cast<u32>((now_ns() - t0) / 1'000'000ull);
    r.pc = ctx.pc.x;
    r.lr = ctx.lr;
    u64 fp = ctx.fp;
    if (stack_range(t, ctx.sp)) {
      // Translated code is built with -fomit-frame-pointer, so the chain is
      // only trusted while each record lies inside the stack and ascends.
      u64 prev = ctx.sp;
      // Compare against hi - 16, never fp + 16: garbage fp near 2^64 wraps.
      while (frames < kFrames && (fp & 7) == 0 && fp >= prev && fp >= t.stackLo &&
             t.stackHi - t.stackLo >= 16 && fp <= t.stackHi - 16) {
        const u64* rec = reinterpret_cast<const u64*>(fp);
        r.fr[frames++] = rec[1];
        prev = fp + 16;
        fp = rec[0];
      }
    }
  }
  svcSetThreadActivity(t.handle, ThreadActivity_Runnable);
  r.frames = frames | (t.role << 8);
  if (R_SUCCEEDED(rc)) g_records[g_count++ % kRingRecords] = r;
}

bool flush() {
  if (g_count - g_written > kRingRecords) {  // the ring lapped an unwritten stretch
    g_dropped += g_count - g_written - kRingRecords;
    g_written = g_count - kRingRecords;
  }
  if (g_written == g_count) return true;
  FILE* f = std::fopen(kProfPath, "ab");
  if (f == nullptr) return false;
  while (g_written < g_count) {
    const size_t at = static_cast<size_t>(g_written % kRingRecords);
    const u64 left = g_count - g_written;
    const size_t n = static_cast<size_t>(left < kRingRecords - at ? left : kRingRecords - at);
    const size_t written = std::fwrite(g_records + at, sizeof(Record), n, f);
    g_written += written;
    if (written != n) { std::fclose(f); return false; }
  }
  return std::fclose(f) == 0;
}

bool begin_capture() {
  g_count = g_written = g_dropped = 0;
  g_samples.store(0, std::memory_order_relaxed);
  g_dropCount.store(0, std::memory_order_relaxed);
  g_elapsedNs.store(0, std::memory_order_relaxed);
  g_records = static_cast<Record*>(std::malloc(kRingRecords * sizeof(Record)));
  if (g_records == nullptr) return false;
  FILE* f = std::fopen(kProfPath, "wb");
  if (f == nullptr) return false;
  Header h{};
  std::memcpy(h.magic, "KPPROF1", 8);
  extern const char* build_stamp();
  extern u64 anchor_address();
  h.anchor = anchor_address();
  h.hz = g_hz;
  h.recordSize = sizeof(Record);
  std::snprintf(h.stamp, sizeof(h.stamp), "%s", build_stamp());
  const bool written = std::fwrite(&h, sizeof(h), 1, f) == 1;
  const bool closed = std::fclose(f) == 0;
  return written && closed;
}

void prof_main(void*) {
  kartpad_sync_clang_tls();
  const u64 periodNs = 1'000'000'000ull / g_hz;
  for (;;) {
    while (!g_requested.load(std::memory_order_acquire)) {
      if (g_state.load(std::memory_order_acquire) == KARTPAD_PROF_STOPPING)
        g_state.store(KARTPAD_PROF_IDLE, std::memory_order_release);
      waitSingle(waiterForUEvent(&g_command), UINT64_MAX);
    }
    bool okay = begin_capture();
    const u64 t0 = now_ns();
    g_startedNs.store(t0, std::memory_order_relaxed);
    int starting = KARTPAD_PROF_STARTING;
    g_state.compare_exchange_strong(starting, KARTPAD_PROF_RECORDING, std::memory_order_release);
    std::fprintf(stderr, "[prof] manual capture at %u Hz -> %s\n", g_hz, kProfPath);
    u64 next = t0 + periodNs;
    u64 lastFlush = t0;
    while (okay && g_requested.load(std::memory_order_acquire)) {
      const u64 now = now_ns();
      if (next > now) waitSingle(waiterForUEvent(&g_command), next - now);
      if (!g_requested.load(std::memory_order_acquire)) break;
      next = now_ns() + periodNs; // Never burst to catch up after SD writes.
      const int targets = g_targetCount.load(std::memory_order_acquire);
      for (int i = 0; i < targets; ++i) {
        const u32 role = g_targets[i].role;
        if (role == 0 || (g_roleMask.load(std::memory_order_relaxed) & (1u << role)))
          sample(g_targets[i], t0);
      }
      g_samples.store(g_count, std::memory_order_relaxed);
      if (now_ns() - lastFlush >= kFlushNs) {
        okay = flush();
        g_dropCount.store(g_dropped, std::memory_order_relaxed);
        lastFlush = now_ns();
      }
    }
    g_state.store(KARTPAD_PROF_STOPPING, std::memory_order_release);
    if (okay) okay = flush();
    g_samples.store(g_count, std::memory_order_relaxed);
    g_dropCount.store(g_dropped, std::memory_order_relaxed);
    g_elapsedNs.store(now_ns() - t0, std::memory_order_relaxed);
    std::free(g_records);
    g_records = nullptr;
    g_requested.store(false, std::memory_order_release);
    g_state.store(okay ? KARTPAD_PROF_SAVED : KARTPAD_PROF_ERROR, std::memory_order_release);
    std::fprintf(stderr, "[prof] %s after %llu samples (%llu dropped)\n",
                 okay ? "saved" : "file/buffer error",
                 static_cast<unsigned long long>(g_count), static_cast<unsigned long long>(g_dropped));
  }
}

}  // namespace

extern "C" const char kartpad_build_stamp[] __attribute__((weak));

// Address anchor for symbolization: the report subtracts this symbol's ELF
// address from the recorded runtime address to get the NRO load bias.
extern "C" __attribute__((noinline, used)) void kartpad_prof_anchor(void) { __asm__ volatile(""); }

namespace {
const char* build_stamp() { return &kartpad_build_stamp[0] ? kartpad_build_stamp : "unknown"; }
u64 anchor_address() { return reinterpret_cast<u64>(&kartpad_prof_anchor); }
}

// A profiling build (FullApp.cmake KARTPAD_PROF_RACE_SECONDS) records its first race seconds without
// a perf.cfg, sampling every thread that runs game, GX, audio or renderer code (roles 1, 4, 6, 7, 8).
#ifdef KARTPAD_PROF_RACE_SECONDS
constexpr int kDefaultRaceSeconds = KARTPAD_PROF_RACE_SECONDS;
constexpr int kDefaultRoles = (1 << KARTPAD_ROLE_FRAME_WORKER) | (1 << KARTPAD_ROLE_AUDIO_MIX) |
                              (1 << KARTPAD_ROLE_GX_WORKER) | (1 << KARTPAD_ROLE_GX_HLE) |
                              (1 << KARTPAD_ROLE_SND_GUEST);
#else
constexpr int kDefaultRaceSeconds = 0;
constexpr int kDefaultRoles = 192;
#endif

// Called on the guest thread once its placement is final.
extern "C" void kartpad_prof_start_guest(void) {
  const int configuredHz = kartpad_perf_int("prof_hz", 750);
  const int hz = configuredHz > 0 ? configuredHz : 750;
  g_hz = static_cast<u32>(hz > 4000 ? 4000 : hz);
  while (g_targetBusy.exchange(true, std::memory_order_acquire)) {}
  g_targets[0].handle = threadGetCurHandle();
  g_targets[0].role = 0;
  if (g_targetCount.load(std::memory_order_relaxed) == 0)
    g_targetCount.store(1, std::memory_order_release);
  g_targetBusy.store(false, std::memory_order_release);
  g_roleMask.store(static_cast<u32>(kartpad_perf_int("prof_roles", kDefaultRoles)) & ~1u, std::memory_order_relaxed);
  ueventCreate(&g_command, true);

  // Core 0 (a worker core; the guest owns core 1 under the tuned profile),
  // above every game thread so sample timing is not skewed by them.
  if (R_FAILED(threadCreate(&g_thread, prof_main, nullptr, nullptr, 64 * 1024, 0x20, 0))) {
    std::fprintf(stderr, "[prof] thread start failed\n");
    return;
  }
  if (R_FAILED(threadStart(&g_thread))) { threadClose(&g_thread); return; }
  g_state.store(KARTPAD_PROF_IDLE, std::memory_order_release);
  std::fprintf(stderr, "[prof] manual controls ready (%u Hz); launch timers disabled\n", g_hz);
}

// Called from kartpad_thread_apply on every placed thread: adds it as a sampling
// target even before the guest initializes or a manual capture is requested.
extern "C" void kartpad_prof_register_thread(int role) {
  if (role <= 0 || role >= KARTPAD_ROLE_COUNT) return;
  while (g_targetBusy.exchange(true, std::memory_order_acquire)) {}
  const int n = g_targetCount.load(std::memory_order_relaxed);
  bool known = false;
  for (int i = 1; i < n; ++i) known |= g_targets[i].role == static_cast<u32>(role);
  const int slot = n > 0 ? n : 1; // Reserve slot zero for the guest.
  if (!known && slot < kMaxTargets) {
    g_targets[slot].handle = threadGetCurHandle();
    g_targets[slot].role = static_cast<u32>(role);
    g_targetCount.store(slot + 1, std::memory_order_release);
  }
  g_targetBusy.store(false, std::memory_order_release);
}

extern "C" int kartpad_prof_set_enabled(int enabled) {
  int state = g_state.load(std::memory_order_acquire);
  if (enabled) {
    if (state != KARTPAD_PROF_IDLE && state != KARTPAD_PROF_SAVED && state != KARTPAD_PROF_ERROR) return 0;
    if (!g_state.compare_exchange_strong(state, KARTPAD_PROF_STARTING, std::memory_order_acq_rel)) return 0;
    g_requested.store(true, std::memory_order_release);
  } else {
    if (state != KARTPAD_PROF_STARTING && state != KARTPAD_PROF_RECORDING) return 0;
    if (!g_state.compare_exchange_strong(state, KARTPAD_PROF_STOPPING, std::memory_order_acq_rel)) return 0;
    g_requested.store(false, std::memory_order_release);
  }
  ueventSignal(&g_command);
  return 1;
}

// perf.cfg `prof_race_seconds N` (b73, default 0 = off): perf_diag calls this once per second while a
// race is running. The capture starts by itself on the 5th race second and stops after N more race
// seconds, so every test run yields a profile (PGO refresh, hot-function analysis) without the Port menu.
extern "C" void kartpad_prof_race_second(void) {
  static int configured = -1;
  static int raceSeconds = 0;
  static int recorded = 0;
  static bool done = false;
  if (configured < 0) configured = kartpad_perf_int("prof_race_seconds", kDefaultRaceSeconds);
  if (configured <= 0 || done) return;
  ++raceSeconds;
  const int state = g_state.load(std::memory_order_acquire);
  if (raceSeconds == 5) {
    if (kartpad_prof_set_enabled(1))
      std::fprintf(stderr, "[prof] race auto capture started (%d race seconds)\n", configured);
    else
      done = true;  // a manual capture is running; leave it alone
    return;
  }
  if (state != KARTPAD_PROF_RECORDING && state != KARTPAD_PROF_STARTING) return;
  if (++recorded >= configured) {
    kartpad_prof_set_enabled(0);
    done = true;
  }
}

extern "C" void kartpad_prof_get_status(kartpad_prof_status* status) {
  if (!status) return;
  status->state = g_state.load(std::memory_order_acquire);
  status->hz = status->state == KARTPAD_PROF_UNAVAILABLE ? 0 : g_hz;
  const u64 elapsed = status->state == KARTPAD_PROF_RECORDING
      ? now_ns() - g_startedNs.load(std::memory_order_relaxed) : g_elapsedNs.load(std::memory_order_relaxed);
  status->elapsed_seconds = static_cast<u32>(elapsed / 1'000'000'000ull);
  status->samples = g_samples.load(std::memory_order_relaxed);
  status->dropped = g_dropCount.load(std::memory_order_relaxed);
}
