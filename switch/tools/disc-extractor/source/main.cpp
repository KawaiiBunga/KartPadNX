// Standalone, key-free-in-source front end for the KartPad disc extractor.
#include <switch.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "disc_extractor.h"

namespace {
constexpr const char* kDefaultWbfs = "sdmc:/iso/RMCP01.wbfs";
constexpr const char* kDefaultIso = "sdmc:/iso/RMCP01.iso";
constexpr const char* kOutputRoot = "sdmc:/switch/kartpad/data";
constexpr const char* kCommonKeyPath = "sdmc:/switch/kartpad/common-key.bin";
constexpr const char* kLogPath = "sdmc:/kartpad-disc-extractor.log";

FILE* gLog = nullptr;
PadState gPad;
bool gCancelled = false;

void durableLog(const char* fmt, ...) {
    char line[768];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    std::printf("%s\n", line);
    if (gLog) {
        std::fputs(line, gLog);
        std::fputc('\n', gLog);
        std::fflush(gLog);
        fsync(fileno(gLog));
        fsdevCommitDevice("sdmc");
    }
}

// NIST SP 800-38A AES-128-CBC decrypt vector. This checks the actual libnx
// primitive on the console without involving a Wii key or disc data.
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

bool textInput(const char* header, const char* guide, const char* initial, char* output,
               std::size_t outputSize, bool password) {
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0))) return false;
    if (password) swkbdConfigMakePresetPassword(&keyboard);
    else swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, header);
    swkbdConfigSetGuideText(&keyboard, guide);
    swkbdConfigSetInitialText(&keyboard, initial);
    swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(outputSize - 1));
    swkbdConfigSetOkButtonText(&keyboard, "Use");
    const Result rc = swkbdShow(&keyboard, output, outputSize);
    swkbdClose(&keyboard);
    return R_SUCCEEDED(rc) && output[0] != '\0';
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

void waitForExit() {
    std::printf("\nPress + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&gPad);
        if (padGetButtonsDown(&gPad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
}
}  // namespace

int main(int, char**) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&gPad);
    gLog = std::fopen(kLogPath, "a");
    durableLog("KartPad Disc Extractor started (no key is logged)");
    char discPath[512] = {};
    std::uint8_t commonKey[16] = {};
    kartpad::disc::ExtractOptions options;
    kartpad::disc::ExtractResult result;
    if (!verifyPlatformAes()) {
        durableLog("FATAL: libnx AES-128-CBC self-test failed");
        std::printf("AES self-test failed. Do not extract; update libnx or report the log.\n");
        waitForExit();
        goto done;
    }
    durableLog("libnx AES-128-CBC self-test passed");

    std::printf("KartPad Disc Extractor\n\n");
    std::printf("This reads your own PAL RMCP01 rev0 WBFS/ISO and writes:\n%s\n\n", kOutputRoot);
    std::printf("No key needed: the Wii common key is built in.\n\n");
    std::printf("A opens the default WBFS path. B opens the default ISO path.\n");
    std::printf("X lets you enter another SD path. + exits.\n");
    consoleUpdate(nullptr);

    while (appletMainLoop() && discPath[0] == '\0') {
        padUpdate(&gPad);
        const u64 down = padGetButtonsDown(&gPad);
        if (down & HidNpadButton_Plus) goto done;
        if (down & HidNpadButton_A) std::snprintf(discPath, sizeof(discPath), "%s", kDefaultWbfs);
        if (down & HidNpadButton_B) std::snprintf(discPath, sizeof(discPath), "%s", kDefaultIso);
        if (down & HidNpadButton_X) {
            textInput("Dump path", "Example: sdmc:/iso/RMCP01.wbfs", kDefaultWbfs, discPath,
                      sizeof(discPath), false);
        }
        consoleUpdate(nullptr);
    }
    if (!discPath[0]) goto done;

    // The Wii common key is built in (selected per disc by the ticket), so the
    // end user supplies nothing. An optional common-key.bin overrides it.
    if (loadCommonKeyFile(commonKey)) {
        durableLog("Using common-key override from %s (contents are not logged)", kCommonKeyPath);
        options.commonKey = commonKey;
    } else {
        durableLog("Using the built-in Wii common key");
        options.commonKey = nullptr;
    }

    durableLog("Extraction requested: source=%s output=%s", discPath, kOutputRoot);
    options.progress = progress;
    result = kartpad::disc::extractDataPartition(
        discPath, kOutputRoot, options, kartpad::disc::platformAesCbcDecrypt());
    std::memset(commonKey, 0, sizeof(commonKey));
    if (result.ok) {
        durableLog("Extraction complete: files=%llu bytes=%llu fst=%llu dol=%llu",
                   (unsigned long long)result.filesExtracted, (unsigned long long)result.bytesExtracted,
                   (unsigned long long)result.fstSize, (unsigned long long)result.dolSize);
        std::printf("\nComplete. The game data is at:\n%s\n", kOutputRoot);
    } else {
        durableLog("Extraction stopped: %s", result.message.c_str());
        std::printf("\nStopped: %s\nPartial files may remain; rerun after fixing the issue.\n",
                    result.message.c_str());
    }
    waitForExit();

done:
    std::memset(commonKey, 0, sizeof(commonKey));
    durableLog("KartPad Disc Extractor exited");
    if (gLog) std::fclose(gLog);
    consoleExit(nullptr);
    return 0;
}
