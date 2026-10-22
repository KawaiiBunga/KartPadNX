#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// libnx types
typedef int Result;
#define R_FAILED(res) ((res) != 0)
#define R_SUCCEEDED(res) ((res) == 0)

// libnx SHA256
#define SHA256_HASH_SIZE 32

typedef struct {
    void* dummy[32]; // size for openssl or mbedtls
} Sha256Context;

void sha256ContextCreate(Sha256Context* ctx);
void sha256ContextUpdate(Sha256Context* ctx, const void* src, size_t size);
void sha256ContextGetHash(Sha256Context* ctx, void* dst);

// libnx env
bool envHasNextLoad(void);
Result envSetNextLoad(const char* path, const char* argv);

// libnx socket
Result socketInitializeDefault(void);
void socketExit(void);

#ifdef __cplusplus
}
#endif
