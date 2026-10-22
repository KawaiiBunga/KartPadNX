#pragma once

// Asynchronous stdout/stderr sink for Horizon (see log_switch.cpp).

#ifdef __cplusplus
extern "C" {
#endif

// Rebinds stdout/stderr to a RAM-buffered device drained to `path` by a
// low-priority writer thread. The previous log is kept as `<path>.prev`.
bool kartpad_log_redirect_stdio(const char* path);
// Re-applies line buffering; the runtime's transcript setup sets both streams
// unbuffered, which splits each log line into several device writes.
void kartpad_log_set_line_buffered(void);
// Drains everything buffered so far to the SD card and fsyncs. Exit paths only.
void kartpad_log_flush_sync(void);
void kartpad_log_shutdown(void);
unsigned long long kartpad_log_dropped_bytes(void);

#ifdef __cplusplus
}
#endif
