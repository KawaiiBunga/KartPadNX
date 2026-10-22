#include <string>
#include <cstdint>
#include "../../switch/src/runtime/disc_extractor.h"
#include "../../switch/src/runtime/wii_crypto.h"

namespace kartpad::disc {
bool makeDirs(const std::string& path) { return true; }
ExtractResult extractDataPartition(const std::string& discPath,
                                   const std::string& dvdRootOut,
                                   const ExtractOptions& opts,
                                   AesCbcDecryptFn aes) {
    ExtractResult res;
    res.ok = true;
    res.message = "Stubbed extraction successful.";
    return res;
}
AesCbcDecryptFn platformAesCbcDecrypt() { return nullptr; }
}
