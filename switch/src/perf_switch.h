#pragma once

// Runtime performance experiments for Horizon, selected by
// sdmc:/switch/kartpad/perf.cfg (see perf_switch.cpp for the keys).

#ifdef __cplusplus
extern "C" {
#endif

enum kartpad_thread_role {
  KARTPAD_ROLE_GUEST = 0,     // main thread: translated game + fibers
  KARTPAD_ROLE_FRAME_WORKER,  // Aurora frame encode/submit
  KARTPAD_ROLE_PRESENTER,     // Aurora surface present
  KARTPAD_ROLE_COMPILER,      // Aurora pipeline compile workers
  KARTPAD_ROLE_AUDIO_MIX,     // AX/DSP voice mix worker
  KARTPAD_ROLE_DEFAULT,       // any other pthread (IO writers, Dawn/NVK)
  KARTPAD_ROLE_GX_WORKER,     // Aurora GX decode worker (perf.cfg gx_thread); Aurora passes 6
  KARTPAD_ROLE_GX_HLE,        // GX HLE thread (perf.cfg gx_hle_thread); runtime passes 7
  KARTPAD_ROLE_SND_GUEST,     // hosted guest sound thread (perf.cfg snd_host_thread); runtime passes 8
  KARTPAD_ROLE_COUNT
};

// Integer setting from perf.cfg (parsed once, base auto-detected), or fallback.
int kartpad_perf_int(const char* key, int fallback);
// Optimization switch: perf.cfg `key` if present, else 0 under `opt_safe 1`, else `fallback`.
// Logged once per key as "[opt] key=value (source, default N)". Cache the result in a static.
int kartpad_opt(const char* key, int fallback);
// True unless perf.cfg selects `profile legacy` (the pre-2026-09-23 placement).
int kartpad_perf_tuned(void);
// Places the calling thread for its role and logs a [THREAD] record. Returns
// nonzero when this build/profile owns the role's placement; callers keep their
// own legacy placement when it returns 0.
int kartpad_thread_apply(int role, int index);
// Per-thread CPU% since the previous call, formatted into buf (perf report).
int kartpad_perf_thread_cpu(char* buf, int size);
// Horizon priority for a role (lower value = higher priority).
int kartpad_thread_priority(int role, int fallback);
// Port-menu CPU boost (Config.toml cpu_boost): nonzero raises the CPU to 1785 MHz (the clock of
// Nintendo's boost mode) and keeps it there (re-applied each second, since docking and sleep reset
// clocks); zero restores the rate seen before boosting.
void kartpad_set_cpu_boost(int on);
// The same clock while the boot shader preload runs (Aurora's pipeline_cache.cpp), whatever the
// port-menu setting; zero hands the clock back to that setting.
void kartpad_set_boot_cpu_boost(int on);

#ifdef __cplusplus
}
#endif
