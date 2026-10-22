// Disc extraction driver: user dump (WBFS/ISO) -> extracted DATA directory.
//
// Stitches the key-free ingestion library (disc_reader, wii_cluster, wii_fst)
// and the decryption layer (wii_crypto) into the one operation the DVD HLE
// actually needs: turn the user's encrypted PAL RMCP01 rev0 dump into an
// extracted `dvd_root` on SD --
//
//     dvd_root/
//       sys/  boot.bin, bi2.bin, apploader.img, main.dol, fst.bin
//       files/  <the FST tree>
//
// which is exactly what build/wiicompiled-fpscr/runtime/src/hle/storage/dvd.cpp
// consumes (it gates on sys/fst.bin + files/, maps files/ -> "/" and sys/ ->
// "/sys/", and reads fst.bin verbatim). The FST and file bytes are written
// byte-for-byte from the decrypted partition so the runtime's own FST parser
// sees the original offsets.
//
// Pure C++ + POSIX (mkdir) + std::ofstream + the injected AES primitive, so it
// is unit-tested off-device with a mock cipher and a synthetic encrypted disc.
// The Wii common key is supplied by the caller at runtime; none ships here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "wii_crypto.h"  // AesCbcDecryptFn

namespace kartpad::disc {

struct ExtractProgress {
    const char* stage = "";        // short human-readable phase label
    std::uint64_t filesDone = 0;   // files written so far
    std::uint64_t filesTotal = 0;  // total files to write (0 until known)
    std::uint64_t bytesDone = 0;   // file bytes written so far
    std::uint64_t bytesTotal = 0;  // total file bytes to write (0 until known)
};

// Called periodically during extraction. May be null. Return false to abort.
using ExtractProgressFn = bool (*)(const ExtractProgress& p, void* user);

struct ExtractOptions {
    // Optional 16-byte common-key override. When null (the default), the driver
    // uses the embedded Wii common key selected by the ticket's common_key_index
    // (see wii_common_key.h), so the end user supplies nothing.
    const std::uint8_t* commonKey = nullptr;
    ExtractProgressFn progress = nullptr;
    void* progressUser = nullptr;
    bool writeSysExtras = true;  // boot.bin, bi2.bin, apploader.img, main.dol
    // Reserved for a future source-identity-aware resume implementation. This
    // driver currently rewrites files even when true, so partial/corrupt output
    // is never accepted merely because its length matches.
    bool skipExisting = false;
};

struct ExtractResult {
    bool ok = false;
    std::string message;            // human-readable, safe to display
    std::uint64_t filesExtracted = 0;
    std::uint64_t bytesExtracted = 0;
    std::uint64_t fstSize = 0;
    std::uint64_t dolSize = 0;
};

// Full driver. Opens `discPath` (WBFS or ISO), validates it is MKW PAL rev0,
// unwraps the title key with opts.commonKey, then decrypts the DATA partition
// and materializes `dvdRootOut`. `aes` is the AES-128-CBC primitive
// (platformAesCbcDecrypt() on Switch; a mock in tests). Returns ok=false with a
// message on any failure; partial output may exist and can be re-run.
ExtractResult extractDataPartition(const std::string& discPath,
                                   const std::string& dvdRootOut,
                                   const ExtractOptions& opts,
                                   AesCbcDecryptFn aes);

// Recursively create `path` (like `mkdir -p`). Exposed for the NRO front-end.
bool makeDirs(const std::string& path);

}  // namespace kartpad::disc
