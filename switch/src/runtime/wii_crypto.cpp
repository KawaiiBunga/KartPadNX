#include "wii_crypto.h"

#include "wii_cluster.h"  // kWiiClusterHeaderSize / kWiiClusterDataSize

#include <cstring>

namespace kartpad::disc {

void unwrapTitleKey(AesCbcDecryptFn aes, const uint8_t commonKey[16],
                    const uint8_t encryptedTitleKey[16], const uint8_t titleKeyIv[16],
                    uint8_t outTitleKey[16]) {
    std::memcpy(outTitleKey, encryptedTitleKey, 16);
    aes(commonKey, titleKeyIv, outTitleKey, 16);
}

uint8_t* decryptClusterData(AesCbcDecryptFn aes, const uint8_t titleKey[16], uint8_t* cluster) {
    uint8_t iv[16];
    std::memcpy(iv, cluster + kWiiClusterDataIvOffset, sizeof(iv));
    aes(titleKey, iv, cluster + kWiiClusterHeaderSize, kWiiClusterDataSize);
    return cluster + kWiiClusterHeaderSize;
}

}  // namespace kartpad::disc
