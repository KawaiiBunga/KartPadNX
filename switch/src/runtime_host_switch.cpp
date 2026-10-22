// Switch implementation of the KartPad mobile runtime-host control surface
// (game-data gate, profile selection).
//
// Interface: switch/include/kartpad_mobile_runtime_host.h
//
// Game-data gate: when sdmc:/switch/kartpad/data has no completed extraction, the user's own disc
// dump (see kAcceptedDumpNames) is extracted there after a console prompt. Normally the launcher
// has already done this. The profile is fixed per NRO; there is no in-game chooser.

#include "kartpad_mobile_runtime_host.h"

#include <switch.h>
#include <unistd.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "runtime/disc_extractor.h"
#include "runtime/wii_crypto.h"


namespace fs = std::filesystem;

namespace {

constexpr const char* kRoot = "sdmc:/switch/kartpad";
constexpr const char* kDataDir = "sdmc:/switch/kartpad/data";
constexpr const char* kCommonKeyPath = "sdmc:/switch/kartpad/common-key.bin";
constexpr const char* kCompletionManifest = ".kartpad-extraction-complete";
constexpr const char* kManifestMagic = "KARTPAD-DVD-EXTRACT-V1";
const char* kAcceptedDumpNames[] = {"RMCP01.wbfs", "RMCP01.iso", "game.wbfs", "game.iso"};
std::string g_selected_profile = "base";

PadState gPad;
bool gCancelled = false;

bool verifyPlatformAes() {
    const std::uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                  0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    const std::uint8_t iv[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    std::uint8_t data[16] = {0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
                             0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d};
    const std::uint8_t plain[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
                                    0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
    kartpad::disc::platformAesCbcDecrypt()(key, iv, data, sizeof(data));
    return std::memcmp(data, plain, sizeof(data)) == 0;
}

bool loadCommonKeyFile(std::uint8_t out[16]) {
    FILE* file = std::fopen(kCommonKeyPath, "rb");
    if (!file) return false;
    const std::size_t bytes = std::fread(out, 1, 16, file);
    const int extra = std::fgetc(file);
    const bool exact = bytes == 16 && extra == EOF && !std::ferror(file);
    std::fclose(file);
    if (!exact) std::memset(out, 0, 16);
    return exact;
}

bool progress(const kartpad::disc::ExtractProgress& p, void*) {
    if (!appletMainLoop()) {
        gCancelled = true;
        return false;
    }
    padUpdate(&gPad);
    if (padGetButtonsDown(&gPad) & HidNpadButton_Plus) gCancelled = true;
    static u64 lastDraw = 0;
    const u64 now = armGetSystemTick();
    if (lastDraw && armTicksToNs(now - lastDraw) < 250000000ULL) return !gCancelled;
    lastDraw = now;
    std::printf("\x1b[2J\x1b[1;1H");
    std::printf("KartPad Disc Extractor\n\n%s\n", p.stage);
    if (p.filesTotal) std::printf("Files: %llu / %llu\n", (unsigned long long)p.filesDone,
                                  (unsigned long long)p.filesTotal);
    if (p.bytesTotal) std::printf("Data:  %llu / %llu MiB\n",
                                  (unsigned long long)(p.bytesDone >> 20),
                                  (unsigned long long)(p.bytesTotal >> 20));
    std::printf("\n+ cancels safely at the next extraction checkpoint.\n");
    consoleUpdate(nullptr);
    return !gCancelled;
}


bool DataTreeLooksReady() {
  // Authoritative readiness = the extractor's completion manifest (magic on its
  // first line) plus the two structures the DVD HLE itself gates on: the
  // verbatim sys/fst.bin it parses, and the files/ tree it maps to "/". The
  // manifest is what makes this fail-closed against a partial extraction; a
  // per-file hash re-verification (the manifest also carries FNV hashes) is
  // deliberately not done every launch — that is a multi-second pass over ~2.7
  // GiB, and the atomically-published manifest already proves completeness.
  std::error_code ec;
  const fs::path root(kDataDir);
  if (!fs::is_directory(root / "files", ec)) return false;
  if (!fs::is_regular_file(root / "sys" / "fst.bin", ec)) return false;

  std::ifstream manifest((root / kCompletionManifest).string(), std::ios::binary);
  if (!manifest) return false;
  std::string firstLine;
  std::getline(manifest, firstLine);
  if (!firstLine.empty() && firstLine.back() == '\r') firstLine.pop_back();
  return firstLine == kManifestMagic;
}

std::string FindUserDump() {
  std::error_code ec;
  for (const char* name : kAcceptedDumpNames) {
    fs::path p = fs::path(kRoot) / name;
    if (fs::exists(p, ec)) return p.string();
  }
  return {};
}

}  // namespace

extern "C" {

bool KartPadMobileEnsureGameDataAvailable() {
  std::error_code ec;
  fs::create_directories(kDataDir, ec);

  if (DataTreeLooksReady()) return true;

  const std::string dump = FindUserDump();
  if (dump.empty()) {
    std::printf(
        "[kartpad] No game data at %s. No WBFS/ISO found.\n"
        "          Please place your RMCP01.wbfs in %s and launch again.\n",
        kDataDir, kRoot);
    return false;
  }

  std::printf("[kartpad] Found dump %s but no completed extraction.\n", dump.c_str());
  std::printf("Press A to extract it now. Press + to cancel.\n");
  consoleUpdate(nullptr);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&gPad);

  bool wantsExtract = false;
  while (appletMainLoop()) {
      padUpdate(&gPad);
      const u64 down = padGetButtonsDown(&gPad);
      if (down & HidNpadButton_Plus) return false;
      if (down & HidNpadButton_A) {
          wantsExtract = true;
          break;
      }
      consoleUpdate(nullptr);
  }

  if (!wantsExtract) return false;

  if (!verifyPlatformAes()) {
      std::printf("FATAL: AES self-test failed! Cannot extract.\n");
      return false;
  }

  std::uint8_t commonKey[16] = {};
  kartpad::disc::ExtractOptions options;
  if (loadCommonKeyFile(commonKey)) {
      options.commonKey = commonKey;
  } else {
      options.commonKey = nullptr;
  }
  options.progress = progress;
  gCancelled = false;

  kartpad::disc::ExtractResult result = kartpad::disc::extractDataPartition(
      dump, kDataDir, options, kartpad::disc::platformAesCbcDecrypt());
  
  std::memset(commonKey, 0, sizeof(commonKey));
  
  if (result.ok) {
      std::printf("\nExtraction complete! The game data is at:\n%s\n", kDataDir);
      std::printf("Press A to start the game.\n");
      consoleUpdate(nullptr);
      while (appletMainLoop()) {
          padUpdate(&gPad);
          if (padGetButtonsDown(&gPad) & HidNpadButton_A) break;
          consoleUpdate(nullptr);
      }
      return true;
  } else {
      std::printf("\nExtraction stopped: %s\n", result.message.c_str());
      return false;
  }
}

const char* KartPadMobileSelectedRuntimeProfile() {
  return g_selected_profile.c_str();
}

}  // extern "C"
