// Clang/libnx TLS reconciliation for Switch. See clang_tls_switch.c for why.
#ifndef KARTPAD_CLANG_TLS_SWITCH_H
#define KARTPAD_CLANG_TLS_SWITCH_H

#ifdef __cplusplus
extern "C" {
#endif

// Mirror libnx's ELF TLS base into tpidr_el0 for the *current* thread. Must run
// before any clang-compiled `thread_local` access on that thread. Idempotent.
void kartpad_sync_clang_tls(void);

#ifdef __cplusplus
}
#endif

#endif  // KARTPAD_CLANG_TLS_SWITCH_H
