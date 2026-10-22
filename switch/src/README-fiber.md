# AArch64 cooperative fiber (`aarch64_fiber.*`)

Replacement for the host-fiber primitive the WiiCompiled guest scheduler needs
(`build/wiicompiled-fpscr/runtime/src/fiber_manager.cpp`). That file uses Win32
fibers on Windows and `ucontext` on Apple. **On Switch neither exists** — libnx
has no fiber API and devkitPro newlib ships no `getcontext`/`makecontext`/
`swapcontext` (verified: absent from `libc.a`). So this is a hand-written
AArch64 context switch.

- `aarch64_fiber.h` — the C API (`init_current`, `create`, `switch`, `current`,
  `destroy`).
- `aarch64_fiber_switch.S` — the callee-saved swap (x19-x30, sp, d8-d15) plus the
  first-entry trampoline.
- `aarch64_fiber.c` — create/init/destroy, stack priming, thread-local current,
  and `kartpad_fiber_finish` (uc_link-style: switch to the fiber's `link` when
  its entry returns).

Cooperative only, one fiber lineage per host thread — same contract as the
Win32/Apple backends it stands in for.

## Verification

Functionally proven under `qemu-aarch64` and compile-checked for the real Switch
ABI (devkitA64 gcc + newlib). Reproduce in the `kartpad-dawn` container:

```
apt-get update -qq && apt-get install -y -qq gcc-aarch64-linux-gnu qemu-user
aarch64-linux-gnu-gcc -O2 -static -std=gnu11 \
  switch/tests/aarch64_fiber_test.c switch/src/aarch64_fiber.c \
  switch/src/aarch64_fiber_switch.S -o /tmp/fiber_test
qemu-aarch64 /tmp/fiber_test        # -> log="1A2B3a4b5Z6"  RESULT: PASS

# Switch-ABI build check (no link):
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc -march=armv8-a -std=gnu11 \
  -c switch/src/aarch64_fiber.c -o /tmp/c.o
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc -march=armv8-a \
  -c switch/src/aarch64_fiber_switch.S -o /tmp/s.o
```

The test covers: switch ordering, callee-saved x19/x28 and float d8/d15
preservation across a yield, argument passing, and both explicit and
fall-off-the-end returns routing to `link`.

## Integration into `fiber_manager.cpp` (wired)

Done, as `switch/patches/wiicompiled-switch-runtime.patch` (the Switch analog of
the Apple patch). It adds a `__SWITCH__` host-fiber backend beside the Win32 one,
mirroring the Apple `__APPLE__` abstraction but swapping ucontext for
`kartpad_fiber_*`:

- scheduler fiber: `kartpad_fiber_init_current()` (adopts the main thread; not
  destroyed at shutdown — it owns no separate stack)
- guest fiber: `kartpad_fiber_create(256 KiB, &GuestFiberManager::FiberProc,
  threadAddr, scheduler)` (`link` = scheduler, so a returning entry falls back
  to it, matching the Win32 "return to scheduler" tail)
- `SwitchHostFiber(h)` → `kartpad_fiber_switch((kartpad_fiber*)h)`
- `CurrentHostFiber()` → `kartpad_fiber_current()`
- `DeleteHostFiber(h)` → `kartpad_fiber_destroy((kartpad_fiber*)h)`

The Win32 paths are untouched — the Win32 helpers wrap the same
`SwitchToFiber`/`GetCurrentFiber`/`DeleteFiber` calls, and `CreateFiber` /
`ConvertThreadToFiber` stay inside `#if defined(_WIN32)`.

Verification: the primitive (`.c` + `.S`) and the actual patched `fiber_manager.cpp` are part of the successfully linked full-game Switch product, and the guest scheduler now runs on Switch hardware all the way into interactive gameplay (PLAYABLE) — the fiber implementation is hardware-proven, not just link-integrated.
