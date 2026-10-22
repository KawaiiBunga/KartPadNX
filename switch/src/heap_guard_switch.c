// Host heap guard (diagnostic): finds the writer behind the Retro WFC heap
// corruption (docs/RETRO-WFC-HEAP-GUARD-RR6133-2026-10-17.md). Two crashes read
// freed or reused host blocks overwritten with small integer pairs; neither
// report can name the writer.
//
// Small malloc blocks (<= kGuardMax bytes) get a 64-byte header (owner tag,
// size, allocation call chain) and a 16-byte tail canary. free() checks the
// canary, fills the first kPoisonBytes of the payload with kPoison and parks
// the block in a FIFO quarantine instead of releasing it. When a block leaves
// the quarantine those bytes must still be kPoison; otherwise something wrote
// through a dangling pointer, and the report names the block's original
// allocation chain (that object's owner is the culprit's target). Corrupted
// blocks are leaked, never reused. Larger blocks and every
// memalign/aligned_alloc/posix_memalign block pass straight through.
//
// Telling them apart must never touch memory outside the freed block's own
// page: a heap neighbour can be an 8 MiB thread stack whose pages libnx has
// remapped as the stack mirror (unreadable). So a guarded payload always
// starts >= kHeader bytes into its page, the tag is read only for such
// pointers, and the tag is wiped before a block goes back to newlib.
//
// Linked with -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
// (GraphicsLink.cmake, KARTPAD_HEAP_GUARD). No TLS: it runs before the clang
// TLS constructor. Reports are formatted under the lock and printed after it,
// so a malloc inside fprintf never re-enters the lock.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch/kernel/mutex.h>

void* __real_malloc(size_t);
void __real_free(void*);
void* __real_realloc(void*, size_t);
extern char _start[];

enum {
  kHeader = 64,
  kTail = 16,
  kGuardMax = 4096,
  kPoisonBytes = 256,
  kQuarantineSlots = 1 << 15,
  kMaxReports = 48,
};
static const uint64_t kQuarantineBytes = 16ull << 20;
static const uint64_t kSecret = 0x6b70486561704775ull;  // "kpHeapGu"
static const uint8_t kPoison = 0xDB, kCanary = 0xC5;
static const uint32_t kLive = 0xA110C8EDu, kFreed = 0xF4EED000u;

typedef struct {
  uint64_t tag;  // payload address ^ kSecret
  uint32_t size, state;
  uint64_t allocLr[4];
  uint64_t base;  // what __real_malloc returned
  uint64_t freeLr;
} Header;
_Static_assert(sizeof(Header) == kHeader, "header layout");

static Mutex g_lock;
static Header* g_ring[kQuarantineSlots];
static uint32_t g_head, g_count, g_reports;
static uint64_t g_bytes, g_frees;

static uintptr_t image_off(uint64_t lr) {
  return lr ? (uintptr_t)lr - (uintptr_t)_start : 0;
}

// Frame-pointer chain (clang keeps x29 frame records; GCC-built libstdc++
// frames without one are skipped transparently).
static void call_chain(uint64_t* out, int n) {
  uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
  const uintptr_t lo = fp, hi = fp + (8u << 20);
  for (int i = 0; i < n; ++i) {
    if (!fp || (fp & 15) || fp < lo || fp >= hi) {
      out[i] = 0;
      continue;
    }
    out[i] = ((const uint64_t*)fp)[1];
    const uintptr_t next = ((const uintptr_t*)fp)[0];
    fp = next > fp ? next : 0;
  }
}

static Header* header_of(void* p) {
  const uintptr_t a = (uintptr_t)p;
  if (!a || (a & 15) || (a & 0xFFF) < kHeader) return NULL;  // never read across a page
  Header* h = (Header*)(a - kHeader);
  return h->tag == (a ^ kSecret) ? h : NULL;
}

static uint32_t poison_span(const Header* h) {
  return h->size < kPoisonBytes ? h->size : kPoisonBytes;
}

static int poisoned(const uint8_t* p, uint32_t n, uint32_t* at) {
  const uint64_t word = 0x0101010101010101ull * kPoison;
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    memcpy(&v, p + i, 8);
    if (v != word) break;
  }
  for (; i < n; ++i)
    if (p[i] != kPoison) {
      *at = i;
      return 0;
    }
  return 1;
}

static int tail_ok(const Header* h) {
  const uint8_t* t = (const uint8_t*)h + kHeader + h->size;
  for (int i = 0; i < kTail; ++i)
    if (t[i] != kCanary) return 0;
  return 1;
}

static int format_block(char* out, size_t cap, const char* what, const Header* h, uint32_t at) {
  const uint8_t* p = (const uint8_t*)h + kHeader;
  uint64_t a = 0, b = 0;
  const uint32_t from = at & ~7u;
  if (from + 8 <= h->size + kTail) memcpy(&a, p + from, 8);
  if (from + 16 <= h->size + kTail) memcpy(&b, p + from + 8, 8);
  return snprintf(out, cap,
      "[heapguard] %s block=%p size=%u off=%u data=%016llx %016llx alloc=+%lx,+%lx,+%lx,+%lx free=+%lx "
      "(frees=%llu quarantined=%u)\n",
      what, (void*)p, h->size, at, (unsigned long long)a, (unsigned long long)b,
      (unsigned long)image_off(h->allocLr[0]), (unsigned long)image_off(h->allocLr[1]),
      (unsigned long)image_off(h->allocLr[2]), (unsigned long)image_off(h->allocLr[3]),
      (unsigned long)image_off(h->freeLr), (unsigned long long)g_frees, g_count);
}

static void report(const char* text) {
  if (text[0]) {
    fputs(text, stderr);
    fflush(stderr);
  }
}

static void release_block(Header* h) {
  void* base = (void*)(uintptr_t)h->base;
  h->tag = 0;
  h->state = 0;
  __real_free(base);
}

void* __wrap_malloc(size_t n) {
  if (n > kGuardMax) return __real_malloc(n);
  const uint32_t size = (uint32_t)((n + 15) & ~(size_t)15);
  // kHeader of slack keeps the payload >= kHeader bytes into its page.
  uint8_t* base = (uint8_t*)__real_malloc(2 * kHeader + size + kTail);
  if (!base) return NULL;
  uint8_t* p = base + kHeader;
  if (((uintptr_t)p & 0xFFF) < kHeader) p += kHeader;
  Header* h = (Header*)(p - kHeader);
  h->tag = (uintptr_t)p ^ kSecret;
  h->size = size;
  h->state = kLive;
  call_chain(h->allocLr, 4);
  h->base = (uintptr_t)base;
  h->freeLr = 0;
  memset(p + size, kCanary, kTail);
  return p;
}

void* __wrap_calloc(size_t count, size_t n) {
  if (n && count > SIZE_MAX / n) return NULL;
  void* p = __wrap_malloc(count * n);
  if (p) memset(p, 0, count * n);
  return p;
}

void __wrap_free(void* p) {
  Header* h = header_of(p);
  if (!h) {
    __real_free(p);
    return;
  }
  char text[640];
  text[0] = 0;
  Header* release = NULL;
  mutexLock(&g_lock);
  if (h->state != kLive) {
    if (g_reports < kMaxReports) {
      ++g_reports;
      format_block(text, sizeof text, "double free", h, 0);
    }
    mutexUnlock(&g_lock);
    report(text);
    return;
  }
  h->freeLr = (uint64_t)__builtin_return_address(0);
  if (!tail_ok(h) && g_reports < kMaxReports) {
    ++g_reports;
    format_block(text, sizeof text, "overflow past end", h, h->size);
  }
  h->state = kFreed;
  memset((uint8_t*)h + kHeader, kPoison, poison_span(h));
  ++g_frees;
  if (g_count == kQuarantineSlots || g_bytes > kQuarantineBytes) {
    const uint32_t tail = (g_head + kQuarantineSlots - g_count) % kQuarantineSlots;
    release = g_ring[tail];
    --g_count;
    g_bytes -= release->size;
    uint32_t at = 0;
    if (!poisoned((const uint8_t*)release + kHeader, poison_span(release), &at)) {
      if (g_reports < kMaxReports) {
        ++g_reports;
        const size_t used = strlen(text);
        format_block(text + used, sizeof text - used, "write after free", release, at);
      }
      release = NULL;  // leak: keep the evidence out of circulation
    }
  }
  g_ring[g_head] = h;
  g_head = (g_head + 1) % kQuarantineSlots;
  ++g_count;
  g_bytes += h->size;
  if (g_frees == 1)
    snprintf(text + strlen(text), sizeof text - strlen(text),
             "[heapguard] active v2: blocks <= %u bytes, first %u bytes checked, quarantine %u slots / %llu MiB\n",
             kGuardMax, kPoisonBytes, (unsigned)kQuarantineSlots,
             (unsigned long long)(kQuarantineBytes >> 20));
  mutexUnlock(&g_lock);
  if (release) release_block(release);
  report(text);
}

void* __wrap_realloc(void* p, size_t n) {
  if (!p) return __wrap_malloc(n);
  Header* h = header_of(p);
  if (!h) return __real_realloc(p, n);
  if (!n) {
    __wrap_free(p);
    return NULL;
  }
  void* q = __wrap_malloc(n);
  if (!q) return NULL;
  memcpy(q, p, n < h->size ? n : h->size);
  __wrap_free(p);
  return q;
}
