// Runtime Kamek loader for drop-in Retro Rewind (docs/RR-DROP-IN.md): loads Code.pul from the SD
// card into guest memory the way Pulsar's loader does on a Wii, instead of the translator baking it
// in. The module is placed at `guestBase` (0x81800000, the runtime's MEM1_KAMEK_OVERLAY) and every
// module-relative address resolves against it, so interpreted code and hooks agree on one address.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dropin {

// Host pointer for `size` bytes at guest `address`, or nullptr when unmapped.
using GuestPointer = uint8_t* (*)(uint32_t address, uint32_t size);

struct KamekWrite {
    uint32_t address;  // absolute guest address outside the module
    uint8_t size;      // 1, 2 or 4
    uint32_t value;    // value written (big-endian in guest memory)
    uint8_t command;   // Kamek command id
};

struct KamekLoadResult {
    bool ok = false;
    std::string error;
    uint32_t moduleStart = 0;    // guestBase
    uint32_t codeSize = 0;       // relocated code+data copied from the file
    uint32_t bssSize = 0;        // zeroed right after the code
    uint32_t ctorStart = 0;      // absolute guest addresses of the constructor table
    uint32_t ctorEnd = 0;
    uint8_t codeSha1[20] = {};   // SHA-1 of the unrelocated code blob (Pulsar checks it at 0x800017B0)
    std::vector<KamekWrite> writes;    // patches applied outside the module, in file order
    std::vector<uint32_t> skipped;     // absolute command addresses left alone (see `skip`)
};

struct KamekLoadOptions {
    char region = 'P';
    uint32_t guestBase = 0x81800000u;
    // Absolute command addresses never written: the WFC codehandler probe word stays clean, and
    // the Retro WFC legacy bootstrap hook is handled by the runtime (same as translate-mod).
    std::vector<uint32_t> skip = {0x80001920u, 0x800ED6E8u};
};

// Parses a combined Code.pul (or one raw Kamek v2/v3 chunk), copies the region's code to
// guestBase, zeroes its bss, applies relocations to the module and patches to the game, and
// writes the code SHA-1 to 0x800017B0. Does not run the constructors (the caller does, once the
// interpreter can). A malformed file changes nothing outside the module; a patch that fails part
// way (unmapped address, branch out of range) leaves the earlier ones applied, so the caller must
// treat any failure as fatal for this boot.
KamekLoadResult LoadKamek(const uint8_t* data, size_t size, GuestPointer guest, const KamekLoadOptions& options = {});

}  // namespace dropin
