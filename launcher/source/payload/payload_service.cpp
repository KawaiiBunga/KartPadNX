#include "mkw/services.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <nlohmann/json.hpp>
#include <zstd.h>
#include <switch.h>
#include <sys/stat.h>
#include <cstdio>
#include <thread>
#include <mutex>
#include <atomic>

using json = nlohmann::json;
#ifndef ROMFS_PREFIX
#define ROMFS_PREFIX "romfs:/"
#endif

namespace mkw {

struct BundledPayloadMeta {
    Variant variant;
    std::string variantId;
    std::string filename;
    uint64_t rawSize = 0;
    std::string expectedSha256;
    std::string buildStamp;
};

class PayloadServiceImpl : public PayloadService {
private:
    std::mutex m_mutex;        // guards job lifecycle, status, and worker thread
    std::mutex m_cacheMutex;   // guards in-memory manifest, state caches, and timestamps
    JobStatus m_status;
    std::thread m_worker;
    std::atomic<bool> m_cancel{false};

    // Cached manifest from romfs:/payload/payloads.json
    bool m_manifestLoaded = false;
    time_t m_manifestMtime = 0;
    std::vector<BundledPayloadMeta> m_manifest;

    // Cached state.json content in memory
    bool m_stateLoaded = false;
    time_t m_stateMtime = 0;
    json m_cachedState;

    void SetStatus(JobStatus::State state, float fraction, const std::string& stage, const std::string& msg = "") {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.state = state;
        m_status.fraction = fraction;
        m_status.stage = stage;
        m_status.message = msg;
    }

    void EnsureManifestLoadedLocked() {
        std::string manifest_path = std::string(ROMFS_PREFIX "payload/payloads.json");
        struct stat st;
        time_t current_mtime = 0;
        if (stat(manifest_path.c_str(), &st) == 0) {
            current_mtime = st.st_mtime;
        }

        if (m_manifestLoaded && current_mtime == m_manifestMtime) {
            return;
        }

        m_manifest.clear();
        try {
            std::ifstream ifs(manifest_path);
            if (!ifs.is_open()) return;
            json j = json::parse(ifs, nullptr, false);
            if (!j.is_object() || j.value("schema", 0) != 1 || !j.contains("payloads") || !j["payloads"].is_array()) {
                return;
            }
            for (auto& p : j["payloads"]) {
                if (!p.is_object()) continue;
                if (!p.contains("variant") || !p["variant"].is_string()) continue;
                std::string var_id = p["variant"].get<std::string>();
                Variant v;
                if (var_id == "vanilla") v = Variant::Vanilla;
                else if (var_id == "rr") v = Variant::RetroRewind;
                else continue;

                BundledPayloadMeta meta;
                meta.variant = v;
                meta.variantId = var_id;
                meta.filename = (p.contains("file") && p["file"].is_string()) ? p["file"].get<std::string>() : "";
                meta.rawSize = (p.contains("raw_size") && p["raw_size"].is_number_unsigned()) ? p["raw_size"].get<uint64_t>() : 0;
                meta.expectedSha256 = (p.contains("raw_sha256") && p["raw_sha256"].is_string()) ? p["raw_sha256"].get<std::string>() : "";
                meta.buildStamp = (p.contains("build_stamp") && p["build_stamp"].is_string()) ? p["build_stamp"].get<std::string>() : "";
                m_manifest.push_back(meta);
            }
            m_manifestLoaded = true;
            m_manifestMtime = current_mtime;
        } catch (...) {
            m_manifest.clear();
        }
    }

    void EnsureStateLoadedLocked() {
        std::string state_path = std::string(kGamesDir) + "/state.json";
        struct stat st;
        time_t current_mtime = 0;
        bool file_exists = (stat(state_path.c_str(), &st) == 0);
        if (file_exists) {
            current_mtime = st.st_mtime;
        }

        if (m_stateLoaded && current_mtime == m_stateMtime && file_exists) {
            return;
        }

        m_cachedState = json::object();
        m_stateLoaded = false;
        m_stateMtime = current_mtime;

        if (file_exists) {
            std::ifstream state_in(state_path);
            if (state_in.is_open()) {
                json parsed = json::parse(state_in, nullptr, false);
                if (parsed.is_object()) {
                    m_cachedState = std::move(parsed);
                }
            }
        }
        m_stateLoaded = true;
    }

    bool IsVariantUnpackedLocked(const BundledPayloadMeta& meta) {
        std::string out_final = std::string(kGamesDir) + "/" + meta.variantId + ".nro";
        struct stat st;
        if (stat(out_final.c_str(), &st) != 0 || st.st_size != (off_t)meta.rawSize) {
            return false;
        }
        EnsureStateLoadedLocked();
        if (m_cachedState.contains(meta.variantId) && m_cachedState[meta.variantId].is_object()) {
            const auto& var_state = m_cachedState[meta.variantId];
            if (var_state.contains("sha256") && var_state["sha256"].is_string() &&
                var_state.contains("mtime") && var_state["mtime"].is_number_integer()) {
                std::string cached_sha = var_state["sha256"].get<std::string>();
                int64_t cached_mtime = var_state["mtime"].get<int64_t>();
                if (cached_sha == meta.expectedSha256 && cached_mtime == (int64_t)st.st_mtime) {
                    return true;
                }
            }
        }
        return false;
    }

    void UnpackWorker(Variant v) {
        BundledPayloadMeta meta;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            EnsureManifestLoadedLocked();
            for (const auto& m : m_manifest) {
                if (m.variant == v) {
                    meta = m;
                    found = true;
                    break;
                }
            }
            if (found && IsVariantUnpackedLocked(meta)) {
                SetStatus(JobStatus::State::Succeeded, 1.0f, "Ready");
                return;
            }
        }

        if (!found || meta.filename.empty()) {
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Payload not found in bundle");
            return;
        }

        std::string in_path = std::string(ROMFS_PREFIX "payload/") + meta.filename;
        std::string out_part = std::string(kGamesDir) + "/" + meta.variantId + ".nro.part";
        std::string out_final = std::string(kGamesDir) + "/" + meta.variantId + ".nro";

        mkdir(kLauncherDir, 0777);
        mkdir(kGamesDir, 0777);

        FILE* fin = fopen(in_path.c_str(), "rb");
        if (!fin) {
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Could not open bundled payload");
            return;
        }

        FILE* fout = fopen(out_part.c_str(), "wb");
        if (!fout) {
            fclose(fin);
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Could not create output file");
            return;
        }

        // Direct block write mode: bypass libc intermediate stream buffer
        setvbuf(fout, nullptr, _IONBF, 0);

        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        size_t buffInSize = ZSTD_DStreamInSize();
        void* buffIn = malloc(buffInSize);
        size_t buffOutSize = 1024 * 1024; // 1 MiB chunk buffer
        void* buffOut = malloc(buffOutSize);
        if (!dctx || !buffIn || !buffOut) {
            if (dctx) ZSTD_freeDCtx(dctx);
            free(buffIn);
            free(buffOut);
            fclose(fin);
            fclose(fout);
            remove(out_part.c_str());
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Out of memory while unpacking");
            return;
        }

        Sha256Context sha_ctx;
        sha256ContextCreate(&sha_ctx);

        size_t toRead = buffInSize;
        size_t read = 0;
        size_t total_out = 0;
        bool error = false;

        SetStatus(JobStatus::State::Running, 0.0f, "Extracting payload...");

        while ((read = fread(buffIn, 1, toRead, fin)) > 0) {
            ZSTD_inBuffer input = { buffIn, read, 0 };
            while (input.pos < input.size) {
                if (m_cancel.load()) {
                    error = true;
                    break;
                }
                ZSTD_outBuffer output = { buffOut, buffOutSize, 0 };
                size_t ret = ZSTD_decompressStream(dctx, &output, &input);
                if (ZSTD_isError(ret)) {
                    error = true;
                    break;
                }
                if (output.pos > 0) {
                    size_t written = fwrite(buffOut, 1, output.pos, fout);
                    if (written != output.pos) {
                        error = true;
                        break;
                    }
                    // Synchronously update SHA-256 context over decompressed block
                    sha256ContextUpdate(&sha_ctx, buffOut, written);
                    total_out += written;
                    float frac = meta.rawSize ? (float)total_out / meta.rawSize : -1.0f;
                    SetStatus(JobStatus::State::Running, frac, "Extracting payload...");
                }
            }
            if (error) break;
        }

        if (ferror(fin)) {
            error = true;
        }

        bool flush_out_ok = (fflush(fout) == 0);
        bool close_out_ok = (fclose(fout) == 0);
        fout = nullptr; // closed exactly once

        bool close_in_ok = (fclose(fin) == 0);
        fin = nullptr; // closed exactly once

        if (!flush_out_ok || !close_out_ok || !close_in_ok) {
            error = true;
        }

        ZSTD_freeDCtx(dctx);
        free(buffIn);
        free(buffOut);

        if (error) {
            remove(out_part.c_str());
            if (m_cancel.load()) {
                SetStatus(JobStatus::State::Cancelled, 0.0f, "Cancelled", "Extraction cancelled");
            } else {
                SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Decompression or write failed");
            }
            return;
        }

        SetStatus(JobStatus::State::Running, 1.0f, "Verifying checksum...");

        uint8_t hash[SHA256_HASH_SIZE];
        sha256ContextGetHash(&sha_ctx, hash);
        char hex[SHA256_HASH_SIZE * 2 + 1];
        for (int i = 0; i < SHA256_HASH_SIZE; i++) {
            snprintf(hex + i * 2, 3, "%02x", hash[i]);
        }
        hex[SHA256_HASH_SIZE * 2] = '\0';

        if (meta.expectedSha256 != hex || total_out != meta.rawSize) {
            remove(out_part.c_str());
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Checksum mismatch");
            return;
        }

        // Staged replacement: move .part to final destination
        remove(out_final.c_str());
        if (rename(out_part.c_str(), out_final.c_str()) != 0) {
            remove(out_part.c_str());
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Failed to finalize payload file");
            return;
        }

        // Update state.json staged into .tmp before renaming; ensure single fclose
        bool state_updated = false;
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            EnsureStateLoadedLocked();

            struct stat st;
            if (stat(out_final.c_str(), &st) == 0) {
                m_cachedState["schema"] = 1;
                m_cachedState[meta.variantId] = {
                    {"sha256", meta.expectedSha256},
                    {"mtime", st.st_mtime}
                };

                std::string state_path = std::string(kGamesDir) + "/state.json";
                std::string state_tmp = state_path + ".tmp";
                FILE* fstate = fopen(state_tmp.c_str(), "w");
                if (fstate) {
                    std::string serialized = m_cachedState.dump();
                    size_t sw = fwrite(serialized.data(), 1, serialized.size(), fstate);
                    bool write_ok = (sw == serialized.size());
                    bool flush_ok = (fflush(fstate) == 0);
                    bool close_ok = (fclose(fstate) == 0);
                    fstate = nullptr; // closed exactly once

                    if (write_ok && flush_ok && close_ok) {
                        remove(state_path.c_str());
                        if (rename(state_tmp.c_str(), state_path.c_str()) == 0) {
                            m_stateMtime = st.st_mtime;
                            state_updated = true;
                        } else {
                            remove(state_tmp.c_str());
                        }
                    } else {
                        remove(state_tmp.c_str());
                    }
                }
            }
        }

        if (!state_updated) {
            std::fprintf(stderr, "[payload] Warning: state.json cache persistence failed; payload installed on disk.\n");
        }

        SetStatus(JobStatus::State::Succeeded, 1.0f, "Ready");
    }

public:
    std::vector<PayloadInfo> List() override {
        std::vector<PayloadInfo> result;
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        EnsureManifestLoadedLocked();
        EnsureStateLoadedLocked();

        for (const auto& meta : m_manifest) {
            PayloadInfo info;
            info.variant = meta.variant;
            info.bundled = true;
            info.rawSize = meta.rawSize;
            info.buildStamp = meta.buildStamp;

            std::string out_final = std::string(kGamesDir) + "/" + meta.variantId + ".nro";
            struct stat st;
            if (stat(out_final.c_str(), &st) == 0 && st.st_size == (off_t)meta.rawSize) {
                if (m_cachedState.contains(meta.variantId) && m_cachedState[meta.variantId].is_object()) {
                    const auto& var_state = m_cachedState[meta.variantId];
                    if (var_state.contains("sha256") && var_state["sha256"].is_string() &&
                        var_state.contains("mtime") && var_state["mtime"].is_number_integer()) {
                        std::string cached_sha = var_state["sha256"].get<std::string>();
                        int64_t cached_mtime = var_state["mtime"].get<int64_t>();
                        if (cached_sha == meta.expectedSha256 && cached_mtime == (int64_t)st.st_mtime) {
                            info.unpacked = true;
                        }
                    }
                }
            }

            result.push_back(info);
        }

        return result;
    }

    bool StartUnpack(Variant v) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_status.state == JobStatus::State::Running) return false;

        if (m_worker.joinable()) {
            m_worker.join();
        }

        m_cancel.store(false);

        // Fast-path: if already unpacked and valid on disk, immediately mark Succeeded without thread spawn
        {
            std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
            EnsureManifestLoadedLocked();
            for (const auto& meta : m_manifest) {
                if (meta.variant == v) {
                    if (IsVariantUnpackedLocked(meta)) {
                        m_status.state = JobStatus::State::Succeeded;
                        m_status.fraction = 1.0f;
                        m_status.stage = "Ready";
                        m_status.message = "";
                        return true;
                    }
                    break;
                }
            }
        }

        m_status.state = JobStatus::State::Running;
        m_status.fraction = 0.0f;
        m_status.stage = "Starting unpack...";
        m_status.message = "";

        try {
            m_worker = std::thread(&PayloadServiceImpl::UnpackWorker, this, v);
        } catch (const std::exception& e) {
            m_status.state = JobStatus::State::Failed;
            m_status.fraction = -1.0f;
            m_status.stage = "Error";
            m_status.message = "Failed to start unpack thread";
            return false;
        }
        return true;
    }

    JobStatus Poll() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_status;
    }

    bool RemoveUnpacked() override {
        std::error_code ec;
        std::filesystem::remove_all(kGamesDir, ec);
        return !ec;
    }

    bool LaunchGame(Variant v, const std::string& profileId, std::string* errorOut) override {
        if (!envHasNextLoad()) {
            if (errorOut) *errorOut = "Firmware does not support envSetNextLoad";
            return false;
        }

        std::string gamePath = std::string(kGamesDir) + "/" + VariantId(v) + ".nro";
        if (gamePath.size() >= 512) {
            if (errorOut) *errorOut = "Game path exceeds envSetNextLoad 512-byte limit";
            return false;
        }

        mkdir(kLauncherDir, 0777);

        auto now = std::chrono::system_clock::now();
        uint64_t launch_ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        uint64_t epoch_s = launch_ts_ms / 1000;

        std::string argv = "\"" + gamePath + "\" --variant=" + VariantId(v) +
                           " --profile=" + profileId +
                           " --return=" + LauncherSelfPath() +
                           " --launch-ts=" + std::to_string(launch_ts_ms);

        if (argv.size() >= 2048) {
            if (errorOut) *errorOut = "Argv string exceeds envSetNextLoad 2048-byte limit";
            return false;
        }

        // Write session.json staged into .tmp before renaming; ensure single fclose
        try {
            json session;
            session["variant"] = VariantId(v);
            session["profile"] = profileId;
            session["launched_at"] = epoch_s;
            session["launch_timestamp_ms"] = launch_ts_ms;
            session["launch_phase"] = "handoff";
            session["launcher_version"] = kLauncherVersion;

            std::string session_path = std::string(kLauncherDir) + "/session.json";
            std::string session_tmp = session_path + ".tmp";
            FILE* fsess = fopen(session_tmp.c_str(), "w");
            if (!fsess) {
                if (errorOut) *errorOut = "Failed to create session.json.tmp";
                return false;
            }
            std::string s_data = session.dump();
            size_t sw = fwrite(s_data.data(), 1, s_data.size(), fsess);
            bool write_ok = (sw == s_data.size());
            bool flush_ok = (fflush(fsess) == 0);
            bool close_ok = (fclose(fsess) == 0);
            fsess = nullptr; // closed exactly once

            if (!write_ok || !flush_ok || !close_ok) {
                remove(session_tmp.c_str());
                if (errorOut) *errorOut = "Failed to write, flush, or close session.json.tmp";
                return false;
            }
            remove(session_path.c_str());
            if (rename(session_tmp.c_str(), session_path.c_str()) != 0) {
                remove(session_tmp.c_str());
                if (errorOut) *errorOut = "Failed to rename session.json";
                return false;
            }
        } catch (...) {
            if (errorOut) *errorOut = "Failed to write session.json";
            return false;
        }

        Result rc = envSetNextLoad(gamePath.c_str(), argv.c_str());
        if (R_FAILED(rc)) {
            if (errorOut) *errorOut = "envSetNextLoad failed";
            return false;
        }

        return true;
    }

    ~PayloadServiceImpl() {
        m_cancel.store(true);
        if (m_worker.joinable()) m_worker.join();
    }
};

PayloadService& Payloads() {
    static PayloadServiceImpl instance;
    return instance;
}

const char* VariantId(Variant v) {
    return v == Variant::Vanilla ? "vanilla" : "rr";
}

const char* VariantDisplayName(Variant v) {
    return v == Variant::Vanilla ? "Vanilla" : "Retro Rewind";
}

} // namespace mkw
