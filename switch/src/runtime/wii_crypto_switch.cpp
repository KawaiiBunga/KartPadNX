// Switch AES-128-CBC primitive for Wii decryption, backed by libnx's crypto.
// Kept separate from wii_crypto.cpp so the Wii-specific logic there stays
// platform-free and off-device testable.
#include "wii_crypto.h"

#include <switch.h>

namespace kartpad::disc {
namespace {

void switchAesCbcDecrypt(const uint8_t key[16], const uint8_t iv[16], uint8_t* data,
                         std::size_t size) {
    Aes128CbcContext ctx;
    aes128CbcContextCreate(&ctx, key, iv, /*is_encryptor=*/false);
    aes128CbcDecrypt(&ctx, data, data, size);  // in place
}

}  // namespace

AesCbcDecryptFn platformAesCbcDecrypt() { return &switchAesCbcDecrypt; }

}  // namespace kartpad::disc
