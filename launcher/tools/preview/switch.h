// Host stand-in for <switch.h> so the launcher UI can be rendered on a PC (launcher/tools/preview).
// Only the calls the launcher makes are provided; they behave like a console with nothing attached.
#pragma once

#include <cstddef>
#include <cstdint>

typedef std::uint32_t Result;
typedef std::uint32_t u32;
typedef std::uint64_t u64;
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)

// romfs / applet / env / sockets
inline Result romfsInit() { return 0; }
inline Result romfsExit() { return 0; }
enum AppletType { AppletType_Application = 0, AppletType_LibraryApplet = 2 };
AppletType appletGetAppletType();
inline bool envHasNextLoad() { return true; }
inline Result envSetNextLoad(const char*, const char*) { return 0; }
inline Result socketInitializeDefault() { return 0; }
inline void socketExit() {}

// SHA-256 (OpenSSL-backed in preview_stubs.cpp)
#define SHA256_HASH_SIZE 32
typedef struct {
    void* dummy[32];
} Sha256Context;
void sha256ContextCreate(Sha256Context* ctx);
void sha256ContextUpdate(Sha256Context* ctx, const void* src, std::size_t size);
void sha256ContextGetHash(Sha256Context* ctx, void* dst);

// Software keyboard: always cancelled in the preview.
typedef struct {
    int unused;
} SwkbdConfig;
inline Result swkbdCreate(SwkbdConfig*, int) { return 1; }
inline void swkbdConfigMakePresetDefault(SwkbdConfig*) {}
inline void swkbdConfigSetHeaderText(SwkbdConfig*, const char*) {}
inline void swkbdConfigSetInitialText(SwkbdConfig*, const char*) {}
inline void swkbdConfigSetStringLenMax(SwkbdConfig*, u32) {}
inline void swkbdConfigSetStringLenMin(SwkbdConfig*, u32) {}
inline Result swkbdShow(SwkbdConfig*, char*, std::size_t) { return 1; }
inline void swkbdClose(SwkbdConfig*) {}
