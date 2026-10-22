#include "aarch64_fiber.h"

#include <stdint.h>
#include <stdlib.h>

// Implemented in aarch64_fiber_switch.S.
extern void kartpad_fiber_asm_switch(void** from_sp, void* to_sp);
extern void kartpad_fiber_trampoline(void);

// Saved-frame offsets (bytes) inside a fiber stack, matching the .S layout.
enum {
  kFrameBytes = 160,
  kOffX19 = 0,   // entry function
  kOffX20 = 8,   // entry argument
  kOffX29 = 80,  // frame pointer
  kOffX30 = 88,  // return address (trampoline on first entry)
};

struct kartpad_fiber {
  void* sp;               // saved stack pointer (0 while this fiber is running)
  void* stack_alloc;      // malloc base to free (NULL for the adopted thread)
  void (*entry)(void*);
  void* arg;
  kartpad_fiber* link;    // switched to if entry() returns
};

#if defined(__SWITCH__)
static kartpad_fiber* t_current = NULL;
#else
static _Thread_local kartpad_fiber* t_current = NULL;
#endif

kartpad_fiber* kartpad_fiber_current(void) { return t_current; }

kartpad_fiber* kartpad_fiber_init_current(void) {
  if (t_current) {
    return t_current;
  }
  kartpad_fiber* f = (kartpad_fiber*)calloc(1, sizeof(*f));
  if (!f) {
    return NULL;
  }
  // The adopted thread has no primed frame; its sp is captured on the first
  // switch away from it. Nothing else needs initializing.
  t_current = f;
  return f;
}

kartpad_fiber* kartpad_fiber_create(size_t stack_size, void (*entry)(void*),
                                    void* arg, kartpad_fiber* link) {
  if (!entry) {
    return NULL;
  }
  if (stack_size < 64u * 1024u) {
    stack_size = 64u * 1024u;  // floor; guest threads can recurse
  }
  stack_size = (stack_size + 15u) & ~(size_t)15u;

  kartpad_fiber* f = (kartpad_fiber*)calloc(1, sizeof(*f));
  if (!f) {
    return NULL;
  }
  // 16-byte-align the stack region itself so the top is aligned too.
  void* base = NULL;
  if (posix_memalign(&base, 16, stack_size) != 0 || !base) {
    free(f);
    return NULL;
  }
  f->stack_alloc = base;
  f->entry = entry;
  f->arg = arg;
  f->link = link;

  // Top of stack, then carve one saved frame. The stored sp points at the
  // frame; the first asm_switch restores from it and `add sp,#160` lands the
  // fiber on a 16-aligned running stack (top - 0).
  uint8_t* top = (uint8_t*)base + stack_size;
  uint8_t* frame = top - kFrameBytes;
  for (size_t i = 0; i < kFrameBytes; ++i) {
    frame[i] = 0;
  }
  *(void**)(frame + kOffX19) = (void*)entry;
  *(void**)(frame + kOffX20) = arg;
  *(void**)(frame + kOffX29) = NULL;
  *(void**)(frame + kOffX30) = (void*)&kartpad_fiber_trampoline;
  f->sp = frame;
  return f;
}

void kartpad_fiber_switch(kartpad_fiber* to) {
  kartpad_fiber* from = t_current;
  if (!to || from == to) {
    return;
  }
  t_current = to;
  kartpad_fiber_asm_switch(&from->sp, to->sp);
  // Resumed: whoever switched back to us set t_current to us already.
}

// Called from the trampoline when a fiber's entry() returns.
void kartpad_fiber_finish(void) {
  kartpad_fiber* self = t_current;
  kartpad_fiber* link = (self && self->link) ? self->link : NULL;
  if (!link) {
    abort();  // no continuation; matches "fibers must not fall off the end"
  }
  kartpad_fiber_switch(link);
  abort();  // a finished fiber must never be resumed
}

void kartpad_fiber_destroy(kartpad_fiber* f) {
  if (!f || f == t_current) {
    return;
  }
  free(f->stack_alloc);
  free(f);
}
