// Preview stand-in: disc extraction never runs in the host preview.
#include "../../../switch/src/runtime/wii_crypto.h"

namespace kartpad::disc {
AesCbcDecryptFn platformAesCbcDecrypt() { return nullptr; }
}  // namespace kartpad::disc
