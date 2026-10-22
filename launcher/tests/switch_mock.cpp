#include "switch.h"
#include <openssl/sha.h>
#include <iostream>

void sha256ContextCreate(Sha256Context* ctx) {
    SHA256_Init((SHA256_CTX*)ctx);
}

void sha256ContextUpdate(Sha256Context* ctx, const void* src, size_t size) {
    SHA256_Update((SHA256_CTX*)ctx, src, size);
}

void sha256ContextGetHash(Sha256Context* ctx, void* dst) {
    SHA256_Final((unsigned char*)dst, (SHA256_CTX*)ctx);
}

bool envHasNextLoad(void) {
    return true;
}

Result envSetNextLoad(const char* path, const char* argv) {
    std::cout << "[MOCK] envSetNextLoad called: " << path << " args: " << argv << "\n";
    return 0;
}

Result socketInitializeDefault(void) {
    std::cout << "[MOCK] socketInitializeDefault called\n";
    return 0;
}

void socketExit(void) {
    std::cout << "[MOCK] socketExit called\n";
}
