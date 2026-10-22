// Minimal cooperative fiber (coroutine) primitive for AArch64.
//
// devkitPro/newlib ships no working ucontext (getcontext/makecontext/
// swapcontext are absent from libc.a), and libnx has no Win32-style fiber API,
// so the WiiCompiled guest scheduler — which is built on cooperative host
// fibers (see src/fiber_manager.cpp) — needs its own context switch on Switch.
//
// This is a classic callee-saved-register swap: kartpad_fiber_switch() saves
// x19-x30, sp and d8-d15 of the running fiber and restores the target's. It is
// cooperative only (no preemption, no signals), which is exactly what the guest
// scheduler wants. Not thread-safe across host threads: one fiber lineage per
// host thread, mirroring the Win32/Apple backends it replaces.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kartpad_fiber kartpad_fiber;

// Adopt the calling host thread as a fiber (the "scheduler" fiber). Must be the
// first call on a host thread that uses fibers; returns the current fiber.
kartpad_fiber* kartpad_fiber_init_current(void);

// Create a suspended fiber with its own stack. When resumed it runs
// entry(arg); if entry returns, control switches to `link` (like ucontext
// uc_link). `link` may be NULL, in which case a return is fatal (aborts).
kartpad_fiber* kartpad_fiber_create(size_t stack_size,
                                    void (*entry)(void* arg), void* arg,
                                    kartpad_fiber* link);

// Switch the calling fiber out and `to` in. Returns when someone switches back.
void kartpad_fiber_switch(kartpad_fiber* to);

// The fiber currently running on this host thread (NULL before init).
kartpad_fiber* kartpad_fiber_current(void);

// Free a fiber created by kartpad_fiber_create. Never free the running fiber or
// the init_current fiber of a live thread.
void kartpad_fiber_destroy(kartpad_fiber* f);

#ifdef __cplusplus
}
#endif
