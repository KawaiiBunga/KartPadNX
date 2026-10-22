// Wii partition decryption logic (title key + cluster payload).
//
// The DATA partition's clusters are AES-128-CBC encrypted with a title key that
// is itself AES-128-CBC encrypted (in the ticket) under the Wii common key. This
// module holds the Wii-specific *logic* — which key, which IV, which byte range —
// and takes the AES primitive as a parameter so it is unit-testable off-device
// with a mock cipher. On Switch the primitive is libnx's aes_cbc (see
// wii_crypto_switch.cpp); the common key comes from wii_common_key.h (built in,
// selected by the ticket) or an explicit caller override.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kartpad::disc {

// AES-128-CBC decrypt in place: `size` bytes at `data` (a multiple of 16) with
// `key` and starting `iv`. Provided by the platform.
using AesCbcDecryptFn = void (*)(const uint8_t key[16], const uint8_t iv[16],
                                 uint8_t* data, std::size_t size);

// The 16-byte per-cluster data IV lives at this offset in the encrypted cluster.
inline constexpr std::size_t kWiiClusterDataIvOffset = 0x3D0;

// Decrypt the 16-byte encrypted title key (from the ticket) with the common key
// and the title-id IV, writing the plaintext title key to `outTitleKey`.
void unwrapTitleKey(AesCbcDecryptFn aes, const uint8_t commonKey[16],
                    const uint8_t encryptedTitleKey[16], const uint8_t titleKeyIv[16],
                    uint8_t outTitleKey[16]);

// Decrypt one 0x8000-byte cluster's data in place: the IV is the 16 bytes at
// offset 0x3D0 of the (encrypted) cluster, and the 0x7C00-byte payload at +0x400
// is decrypted with the title key. Returns a pointer to the decrypted payload
// (cluster + 0x400). The 0x400 hash header is left as-is (not needed to extract
// files).
uint8_t* decryptClusterData(AesCbcDecryptFn aes, const uint8_t titleKey[16], uint8_t* cluster);

// The platform AES-128-CBC decrypt (libnx on Switch). Declared here; defined in
// wii_crypto_switch.cpp so the pure-logic part stays platform-free.
AesCbcDecryptFn platformAesCbcDecrypt();

}  // namespace kartpad::disc
