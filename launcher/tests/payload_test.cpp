#include "switch.h"
#include "mkw/services.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <zstd.h>
#include <vector>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cassert>

using namespace mkw;

bool force_short_write = false;

extern "C" {
    size_t __real_fwrite(const void *ptr, size_t size, size_t count, FILE *stream);
    size_t __wrap_fwrite(const void *ptr, size_t size, size_t count, FILE *stream) {
        if (force_short_write) {
            return count / 2; // return less
        }
        return __real_fwrite(ptr, size, count, stream);
    }
}

JobStatus WaitForUnpack() {
    for (int i = 0; i < 200; ++i) {
        JobStatus status = Payloads().Poll();
        if (status.state == JobStatus::State::Succeeded ||
            status.state == JobStatus::State::Failed ||
            status.state == JobStatus::State::Cancelled) {
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    JobStatus timeout;
    timeout.state = JobStatus::State::Failed;
    timeout.message = "Timed out waiting for unpack";
    return timeout;
}

int main() {
    // Generate valid test romfs payload
    mkdir("test_romfs", 0777);
    mkdir("test_romfs/payload", 0777);
    std::filesystem::create_directories("sdmc:/switch/mkw-launcher/.games");
    
    std::string raw_data = "Hello World!!";
    size_t bound = ZSTD_compressBound(raw_data.size());
    std::vector<char> comp(bound);
    size_t cSize = ZSTD_compress(comp.data(), bound, raw_data.data(), raw_data.size(), 1);
    
    FILE* f = fopen("test_romfs/payload/test.nro.zst", "wb");
    if (f) { fwrite(comp.data(), 1, cSize, f); fclose(f); }
    
    remove("sdmc:/switch/mkw-launcher/.games/vanilla.nro.part");
    remove("sdmc:/switch/mkw-launcher/.games/vanilla.nro");
    remove("sdmc:/switch/mkw-launcher/.games/state.json");
    remove("sdmc:/switch/mkw-launcher/session.json");
    
    // Test E: payload unpack short write
    std::cout << "Test E: Short write\n";
    force_short_write = true;
    Payloads().StartUnpack(Variant::Vanilla);
    if (WaitForUnpack().state != JobStatus::State::Failed) {
        std::cout << "Test E Failed (should have failed on short write)\n";
        return 1;
    }
    
    struct stat st;
    if (stat("sdmc:/switch/mkw-launcher/.games/vanilla.nro.part", &st) == 0) {
        std::cout << "Test E Failed (part file was not deleted)\n";
        return 1;
    }
    if (stat("sdmc:/switch/mkw-launcher/.games/state.json", &st) == 0) {
        std::cout << "Test E Failed (state.json was written)\n";
        return 1;
    }
    
    // Test F: payload happy path
    std::cout << "Test F: Happy path\n";
    force_short_write = false;
    Payloads().StartUnpack(Variant::Vanilla);
    JobStatus status = WaitForUnpack();
    if (status.state != JobStatus::State::Succeeded) {
        std::cout << "Test F Failed (unpack failed). Reason: " << status.message << "\n";
        return 1;
    }
    
    if (stat("sdmc:/switch/mkw-launcher/.games/vanilla.nro", &st) != 0) {
        std::cout << "Test F Failed (.nro not created)\n";
        return 1;
    }
    if (stat("sdmc:/switch/mkw-launcher/.games/state.json", &st) != 0) {
        std::cout << "Test F Failed (state.json not created)\n";
        return 1;
    }
    
    std::ifstream state_in("sdmc:/switch/mkw-launcher/.games/state.json");
    nlohmann::json state_j;
    state_in >> state_j;
    if (state_j["vanilla"]["sha256"] != "096c0a72c31f9a2d65126d8e8a401a2ab2f2e21d0a282a6ffe6642bbef65ffd9") {
        std::cout << "Test F Failed (hash mismatch in state.json)\n";
        return 1;
    }
    
    // Test G: Fast warm-cache start path
    std::cout << "Test G: Warm-cache fast path\n";
    bool start_res = Payloads().StartUnpack(Variant::Vanilla);
    if (!start_res) {
        std::cout << "Test G Failed (StartUnpack returned false)\n";
        return 1;
    }
    JobStatus warm_status = Payloads().Poll();
    if (warm_status.state != JobStatus::State::Succeeded) {
        std::cout << "Test G Failed (warm cache Poll() not immediately Succeeded)\n";
        return 1;
    }
    std::cout << "Test G Passed (immediate warm start without background worker)\n";

    // Test H: Launch handoff instrumentation and session.json verification
    std::cout << "Test H: Launch handoff instrumentation\n";
    std::string launch_err;
    if (!Payloads().LaunchGame(Variant::Vanilla, "profile_alpha", &launch_err)) {
        std::cout << "Test H Failed: LaunchGame failed: " << launch_err << "\n";
        return 1;
    }
    
    std::ifstream session_in("sdmc:/switch/mkw-launcher/session.json");
    if (!session_in.is_open()) {
        std::cout << "Test H Failed: session.json not found\n";
        return 1;
    }
    nlohmann::json sess;
    session_in >> sess;
    if (sess.value("variant", "") != "vanilla" ||
        sess.value("profile", "") != "profile_alpha" ||
        sess.value("launch_phase", "") != "handoff" ||
        sess.value("launcher_version", "") != "1.0.0" ||
        sess.value("launch_timestamp_ms", (uint64_t)0) == 0 ||
        sess.value("launched_at", (uint64_t)0) == 0) {
        std::cout << "Test H Failed: session.json fields invalid: " << sess.dump() << "\n";
        return 1;
    }
    std::cout << "Test H Passed (session.json verified: " << sess.dump() << ")\n";

    // Test I: List() cache query consistency
    std::cout << "Test I: List() cache consistency\n";
    auto list = Payloads().List();
    bool found_unpacked = false;
    for (const auto& info : list) {
        if (info.variant == Variant::Vanilla && info.unpacked) {
            found_unpacked = true;
            break;
        }
    }
    if (!found_unpacked) {
        std::cout << "Test I Failed: List() did not report Vanilla as unpacked\n";
        return 1;
    }
    std::cout << "Test I Passed (List() correctly reports unpacked=true)\n";

    // Test J: Cache invalidation on file tampering
    std::cout << "Test J: Cache invalidation on file size tamper\n";
    FILE* ftamper = fopen("sdmc:/switch/mkw-launcher/.games/vanilla.nro", "ab");
    if (ftamper) {
        fputc('X', ftamper);
        fclose(ftamper);
    }
    auto list_tampered = Payloads().List();
    bool tampered_unpacked = true;
    for (const auto& info : list_tampered) {
        if (info.variant == Variant::Vanilla) {
            tampered_unpacked = info.unpacked;
            break;
        }
    }
    if (tampered_unpacked) {
        std::cout << "Test J Failed: List() reported tampered file as unpacked\n";
        return 1;
    }
    std::cout << "Test J Passed (tampered file correctly rejected as unpacked=false)\n";

    std::cout << "All payload unit tests passed!\n";
    return 0;
}
