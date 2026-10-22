// Asynchronous runtime log sink for Horizon.
//
// stdout/stderr used to point straight at an SD file, and the runtime leaves
// both streams unbuffered, so every `std::cerr << a << b` issued one SD write
// per operand on whichever thread logged -- including the guest thread -- and
// concurrent writers interleaved mid-line. melee-nx measured 30-687 ms frame
// stalls from the same pattern.
//
// Here stdout/stderr are rebound to a "klog:" devoptab whose write is a memcpy
// into a bounded RAM buffer. A low-priority writer thread moves full buffers to
// the SD file and fsyncs at most once per second. When the buffer is full the
// bytes are dropped and counted rather than blocking the producer. Exit paths
// call kartpad_log_flush_sync() so the tail of the log still reaches the card.

#include "log_switch.h"
#include "clang_tls_switch.h"

#include <switch.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <cstdlib>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr size_t kBufferBytes = 512 * 1024;
constexpr size_t kWakeBytes = 32 * 1024;
constexpr u64 kWakeNs = 250'000'000ull;
constexpr u64 kSyncNs = 1'000'000'000ull;
constexpr size_t kWriterStack = 64 * 1024;
constexpr int kWriterPriority = 0x3B;

struct Buffer {
  char* data = nullptr;
  size_t used = 0;
};

struct State {
  Mutex mutex;         // guards active buffer, dropped counter
  Mutex fileMutex;     // serializes SD writes (writer thread vs sync flush)
  CondVar cv;
  Buffer buffers[2];
  int active = 0;
  u64 droppedBytes = 0;
  u64 droppedTotal = 0;
  int fd = -1;
  bool stop = false;
  bool ready = false;
  u64 lastSyncTick = 0;
  bool dirty = false;
  Thread writer{};
} g;

u64 now_ns() { return armTicksToNs(armGetSystemTick()); }

// Caller holds fileMutex.
void write_all(const char* data, size_t size) {
  while (size != 0 && g.fd >= 0) {
    const ssize_t n = ::write(g.fd, data, size);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return;
    }
    data += n;
    size -= static_cast<size_t>(n);
    g.dirty = true;
  }
}

// Caller holds fileMutex.
void maybe_sync(bool force) {
  if (g.fd < 0 || !g.dirty) return;
  const u64 now = now_ns();
  if (!force && now - g.lastSyncTick < kSyncNs) return;
  ::fsync(g.fd);
  g.lastSyncTick = now;
  g.dirty = false;
}

// Swaps out the active buffer and writes it. Caller holds fileMutex, not mutex.
void drain_once(bool forceSync) {
  Buffer* full = nullptr;
  u64 dropped = 0;
  mutexLock(&g.mutex);
  if (g.buffers[g.active].used != 0 || g.droppedBytes != 0) {
    full = &g.buffers[g.active];
    g.active ^= 1;
    g.buffers[g.active].used = 0;
    dropped = g.droppedBytes;
    g.droppedBytes = 0;
  }
  mutexUnlock(&g.mutex);

  if (full != nullptr) {
    if (dropped != 0) {
      char line[96];
      const int n = std::snprintf(line, sizeof(line), "\n[klog] dropped %llu bytes (buffer full)\n",
                                  static_cast<unsigned long long>(dropped));
      if (n > 0) write_all(line, static_cast<size_t>(n));
    }
    write_all(full->data, full->used);
    full->used = 0;
  }
  maybe_sync(forceSync);
}

void writer_main(void*) {
  // Raw libnx thread (not the pthread wrap): set up clang TLS ourselves.
  kartpad_sync_clang_tls();
  for (;;) {
    mutexLock(&g.mutex);
    if (!g.stop && g.buffers[g.active].used < kWakeBytes) {
      condvarWaitTimeout(&g.cv, &g.mutex, kWakeNs);
    }
    const bool stop = g.stop;
    mutexUnlock(&g.mutex);

    mutexLock(&g.fileMutex);
    drain_once(false);
    mutexUnlock(&g.fileMutex);
    if (stop) return;
  }
}

void append(const char* ptr, size_t len) {
  if (len == 0) return;
  bool wake = false;
  mutexLock(&g.mutex);
  Buffer& buf = g.buffers[g.active];
  if (kBufferBytes - buf.used >= len) {
    std::memcpy(buf.data + buf.used, ptr, len);
    buf.used += len;
    wake = buf.used >= kWakeBytes;
  } else {
    g.droppedBytes += len;
    g.droppedTotal += len;
    wake = true;
  }
  mutexUnlock(&g.mutex);
  if (wake) condvarWakeOne(&g.cv);
}

int klog_open(struct _reent*, void*, const char*, int, int) { return 0; }
int klog_close(struct _reent*, void*) { return 0; }
ssize_t klog_write(struct _reent*, void*, const char* ptr, size_t len) {
  append(ptr, len);
  return static_cast<ssize_t>(len);
}
ssize_t klog_read(struct _reent* r, void*, char*, size_t) {
  r->_errno = EBADF;
  return -1;
}
int klog_fstat(struct _reent*, void*, struct stat* st) {
  std::memset(st, 0, sizeof(*st));
  st->st_mode = S_IFCHR;
  return 0;
}

const devoptab_t kKlogDevice = {
    .name = "klog",
    .structSize = sizeof(int),
    .open_r = klog_open,
    .close_r = klog_close,
    .write_r = klog_write,
    .read_r = klog_read,
    .fstat_r = klog_fstat,
};

}  // namespace

extern "C" bool kartpad_log_redirect_stdio(const char* path) {
  if (g.ready) return true;
  // Keep the previous run's log for comparison instead of appending forever.
  char prev[256];
  std::snprintf(prev, sizeof(prev), "%s.prev", path);
  ::unlink(prev);
  ::rename(path, prev);

  g.fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (g.fd < 0) return false;
  g.buffers[0].data = static_cast<char*>(std::malloc(kBufferBytes));
  g.buffers[1].data = static_cast<char*>(std::malloc(kBufferBytes));
  if (g.buffers[0].data == nullptr || g.buffers[1].data == nullptr) {
    ::close(g.fd);
    g.fd = -1;
    return false;
  }
  mutexInit(&g.mutex);
  mutexInit(&g.fileMutex);
  condvarInit(&g.cv);
  g.lastSyncTick = now_ns();

  const int device = AddDevice(&kKlogDevice);
  if (device < 0) {
    ::close(g.fd);
    g.fd = -1;
    return false;
  }

  std::fflush(stdout);
  std::fflush(stderr);
  // Same single-freopen + dup2 rule as the old SD redirect: rebinding stdout's
  // FILE once and duplicating its descriptor onto stderr is what works with
  // newlib's pre-existing console FILE objects on Horizon.
  if (std::freopen("klog:/", "w", stdout) == nullptr) return false;
  if (::dup2(STDOUT_FILENO, STDERR_FILENO) < 0) return false;
  std::clearerr(stdout);
  std::clearerr(stderr);
  kartpad_log_set_line_buffered();

  if (R_FAILED(threadCreate(&g.writer, writer_main, nullptr, nullptr, kWriterStack, kWriterPriority, -2)) ||
      R_FAILED(threadStart(&g.writer))) {
    // No writer: the buffers still fill, and exit paths flush synchronously.
    std::fprintf(stderr, "[klog] writer thread unavailable; log flushes only on exit\n");
  }
  g.ready = true;
  return true;
}

extern "C" void kartpad_log_set_line_buffered(void) {
  static char outBuf[4096];
  static char errBuf[4096];
  std::setvbuf(stdout, outBuf, _IOLBF, sizeof(outBuf));
  std::setvbuf(stderr, errBuf, _IOLBF, sizeof(errBuf));
}

extern "C" void kartpad_log_flush_sync(void) {
  if (!g.ready) return;
  std::fflush(stdout);
  std::fflush(stderr);
  mutexLock(&g.fileMutex);
  drain_once(true);
  mutexUnlock(&g.fileMutex);
}

// Stops the writer thread and closes the log before the process exits. _Exit tears down the
// devoptab/fs state underneath a writer that is mid-write, which faulted in _write_r on
// "Quit to launcher" (crash reports 2026-10-05). Safe to call more than once.
extern "C" void kartpad_log_shutdown(void) {
  if (!g.ready) return;
  std::fflush(stdout);
  std::fflush(stderr);
  mutexLock(&g.mutex);
  const bool hadWriter = g.writer.handle != 0 && !g.stop;
  g.stop = true;
  condvarWakeAll(&g.cv);
  mutexUnlock(&g.mutex);
  if (hadWriter) {
    threadWaitForExit(&g.writer);
    threadClose(&g.writer);
  }
  mutexLock(&g.fileMutex);
  drain_once(true);
  if (g.fd >= 0) {
    ::fsync(g.fd);
    ::close(g.fd);
    g.fd = -1;
  }
  mutexUnlock(&g.fileMutex);
}

extern "C" unsigned long long kartpad_log_dropped_bytes(void) {
  mutexLock(&g.mutex);
  const u64 dropped = g.droppedTotal;
  mutexUnlock(&g.mutex);
  return dropped;
}
