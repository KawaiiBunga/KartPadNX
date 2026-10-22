#include "mkw/services.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <sodium.h>
#include <switch.h>
#include <sys/stat.h>
#include <utime.h>
#include <cstdio>
#include <thread>
#include <mutex>
#include <atomic>

using json = nlohmann::json;

#if defined(NDEBUG) && (defined(MKW_UPDATER_ALLOW_UNSIGNED) || defined(MKW_UPDATER_TEST_MOCKS))
#error "Release builds must verify update signatures with the pinned release key."
#endif

#ifndef MKW_UPDATER_ALLOW_UNSIGNED
#ifdef MKW_UPDATER_TEST_MOCKS
extern unsigned char mock_key[32];
#define kReleasePublicKey mock_key
#else
// Pinned public key for signature verification
#include "release_key.h"
#endif
#endif

namespace mkw {

class UpdateServiceImpl : public UpdateService {
private:
    std::mutex m_mutex;
    JobStatus m_status;
    std::thread m_worker;
    std::atomic<bool> m_cancel{false};
    UpdateInfo m_latest;
    std::string m_latest_url;
    std::string m_latest_sha256;
    bool m_socket_init_ok = false;

    void SetStatus(JobStatus::State state, float fraction, const std::string& stage, const std::string& msg = "") {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.state = state;
        m_status.fraction = fraction;
        m_status.stage = stage;
        m_status.message = msg;
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    static size_t WriteFileCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
        FILE* stream = (FILE*)userdata;
        return fwrite(ptr, size, nmemb, stream);
    }

    static int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
        UpdateServiceImpl* self = (UpdateServiceImpl*)clientp;
        if (self->m_cancel.load()) return 1; // Abort
        if (dltotal > 0) {
            self->SetStatus(JobStatus::State::Running, (float)dlnow / dltotal, "Downloading update...");
        }
        return 0;
    }

    // Lower-case hex SHA-256 of a file, or "" if it cannot be read.
    static std::string FileSha256(const std::string& path, uint64_t* sizeOut) {
        FILE* fp = fopen(path.c_str(), "rb");
        if (!fp) return {};
        Sha256Context ctx;
        sha256ContextCreate(&ctx);
        std::vector<char> buffer(1 << 20);
        size_t n;
        uint64_t total = 0;
        while ((n = fread(buffer.data(), 1, buffer.size(), fp)) > 0) {
            sha256ContextUpdate(&ctx, buffer.data(), n);
            total += n;
        }
        const bool ok = ferror(fp) == 0;
        fclose(fp);
        if (!ok) return {};
        uint8_t hash[SHA256_HASH_SIZE];
        sha256ContextGetHash(&ctx, hash);
        char hex[SHA256_HASH_SIZE * 2 + 1];
        for (int i = 0; i < SHA256_HASH_SIZE; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
        if (sizeOut) *sizeOut = total;
        return hex;
    }

    void CheckWorker(bool force) {
        SetStatus(JobStatus::State::Running, -1.0f, "Checking for updates...");

        // Check throttle unless force
        if (!force) {
            struct stat st;
            if (stat(std::string(kUpdateDir).c_str(), &st) == 0) {
                // If checked in last 6 hours
                if (time(NULL) - st.st_mtime < 6 * 3600) {
                    SetStatus(JobStatus::State::Succeeded, 1.0f, "Up to date (cached)");
                    return;
                }
            }
        }

        mkdir(kLauncherDir, 0777);
        mkdir(kUpdateDir, 0777);

        if (!m_socket_init_ok) {
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Networking is unavailable");
            return;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Failed to init curl");
            return;
        }

        std::string manifest_url = "https://github.com/KawaiiBunga/Kartpad-NX/releases/latest/download/manifest.json";
        std::string sig_url = "https://github.com/KawaiiBunga/Kartpad-NX/releases/latest/download/manifest.json.sig";
        
        auto fetch_url = [&](const std::string& url, std::string& out_str) -> CURLcode {
            out_str.clear();
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_str);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "MKW-Launcher/1.0");
            return curl_easy_perform(curl);
        };

        std::string response_string;
        CURLcode res = fetch_url(manifest_url, response_string);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res != CURLE_OK || http_code != 200) {
            curl_easy_cleanup(curl);
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Failed to fetch manifest");
            return;
        }

        std::string sig_string;
        res = fetch_url(sig_url, sig_string);
        http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res != CURLE_OK || http_code != 200) {
            curl_easy_cleanup(curl);
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Failed to fetch manifest signature");
            return;
        }

        // Verify signature of EXACT raw bytes
#ifndef MKW_UPDATER_ALLOW_UNSIGNED
        if (sodium_init() < 0) {
            curl_easy_cleanup(curl);
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "libsodium init failed");
            return;
        }

        // Trim trailing whitespace from sig_string (e.g. newline) before decoding
        size_t sig_len = sig_string.find_last_not_of(" \t\n\r");
        if (sig_len != std::string::npos) sig_string.erase(sig_len + 1);

        unsigned char sig_bin[64];
        size_t sig_bin_len;
        
        // base64 decode (libsodium supports base64)
        if (sodium_base642bin(sig_bin, sizeof(sig_bin), sig_string.c_str(), sig_string.length(), 
            NULL, &sig_bin_len, NULL, sodium_base64_VARIANT_ORIGINAL) != 0 || sig_bin_len != 64) {
            curl_easy_cleanup(curl);
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Invalid signature format");
            return;
        }

        if (crypto_sign_verify_detached(sig_bin, (const unsigned char*)response_string.c_str(), response_string.length(), kReleasePublicKey) != 0) {
            curl_easy_cleanup(curl);
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Signature verification failed");
            return;
        }
#endif

        UpdateInfo latest;
        std::string url, sha;
        try {
            json j = json::parse(response_string);
            if (j["schema"] != 1) throw std::runtime_error("Unsupported manifest schema");
            const json* launcher = nullptr;
            for (const auto& c : j["components"]) {
                if (c.value("id", "") == "launcher") launcher = &c;
            }
            if (!launcher) throw std::runtime_error("Manifest has no launcher component");
            latest.version = j.value("version", "");
            latest.changelog = j.value("changelog", "");
            latest.downloadSize = (*launcher)["size"].get<uint64_t>();
            url = (*launcher)["url"].get<std::string>();
            sha = (*launcher)["sha256"].get<std::string>();
        } catch (const std::exception& e) {
            curl_easy_cleanup(curl);
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", e.what());
            return;
        }

        curl_easy_cleanup(curl);

        // The version string never changes, so "newer" means "different bytes from what is installed".
        SetStatus(JobStatus::State::Running, -1.0f, "Comparing with the installed launcher...");
        latest.available = FileSha256(LauncherSelfPath(), nullptr) != sha;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_latest = latest;
            m_latest_url = url;
            m_latest_sha256 = sha;
        }

        // Touch dir to reset throttle
        utime(kUpdateDir, NULL);

        SetStatus(JobStatus::State::Succeeded, 1.0f, "Done",
                  latest.available ? "Update available: " + latest.version : "MKW Launcher is up to date.");
    }

    void DownloadWorker() {
        SetStatus(JobStatus::State::Running, 0.0f, "Starting download...");

        std::string url;
        std::string expected_sha256;
        uint64_t expected_size;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_latest.available) {
                SetStatus(JobStatus::State::Failed, -1.0f, "Error", "No update available");
                return;
            }
            url = m_latest_url;
            expected_sha256 = m_latest_sha256;
            expected_size = m_latest.downloadSize;
        }

        std::string part_path = std::string(kUpdateDir) + "/mkw-launcher.nro.part";
        
        FILE* fp = fopen(part_path.c_str(), "wb");
        if (!fp) {
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Failed to open part file");
            return;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            fclose(fp);
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Failed to init curl");
            return;
        }
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "MKW-Launcher/1.0");

        CURLcode res = curl_easy_perform(curl);
        const bool closed = fclose(fp) == 0;
        curl_easy_cleanup(curl);
        if (!closed && res == CURLE_OK) res = CURLE_WRITE_ERROR;

        if (m_cancel.load()) {
            remove(part_path.c_str());
            SetStatus(JobStatus::State::Cancelled, 0.0f, "Cancelled", "Download cancelled");
            return;
        }

        if (res != CURLE_OK) {
            remove(part_path.c_str());
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Download failed");
            return;
        }

        SetStatus(JobStatus::State::Running, 1.0f, "Verifying update...");

        uint64_t total_read = 0;
        const std::string hex = FileSha256(part_path, &total_read);
        if (expected_sha256 != hex || total_read != expected_size) {
            remove(part_path.c_str());
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Update verification failed");
            return;
        }

        // Install: rename current to .prev, .part to live
        const std::string live_path = LauncherSelfPath();
        const std::string prev_path = std::string(kUpdateDir) + "/mkw-launcher.nro.prev";

        // Ensure .prev is removed, ignore error if it didn't exist
        remove(prev_path.c_str());

        struct stat liveSt;
        const bool hadLive = stat(live_path.c_str(), &liveSt) == 0;
        if (hadLive && rename(live_path.c_str(), prev_path.c_str()) != 0) {
            remove(part_path.c_str());
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Failed to backup current version");
            return;
        }

        if (rename(part_path.c_str(), live_path.c_str()) != 0) {
            // Rollback
            if (hadLive && rename(prev_path.c_str(), live_path.c_str()) != 0) {
                SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Failed to install and rollback failed");
                return;
            }
            remove(part_path.c_str());
            SetStatus(JobStatus::State::Failed, -1.0f, "Error", "Failed to install update");
            return;
        }

        SetStatus(JobStatus::State::Succeeded, 1.0f, "Update installed");
    }

public:
    UpdateServiceImpl() {
        m_socket_init_ok = R_SUCCEEDED(socketInitializeDefault());
    }

    ~UpdateServiceImpl() {
        m_cancel.store(true);
        if (m_worker.joinable()) m_worker.join();
        if (m_socket_init_ok) {
            socketExit();
        }
    }

    bool StartCheck(bool force) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_status.state == JobStatus::State::Running) return false;
        
        if (m_worker.joinable()) m_worker.join();

        m_cancel.store(false);
        m_status.state = JobStatus::State::Running;
        m_worker = std::thread(&UpdateServiceImpl::CheckWorker, this, force);
        return true;
    }

    bool StartDownloadAndInstall() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_status.state == JobStatus::State::Running) return false;
        
        if (m_worker.joinable()) m_worker.join();

        m_cancel.store(false);
        m_status.state = JobStatus::State::Running;
        m_worker = std::thread(&UpdateServiceImpl::DownloadWorker, this);
        return true;
    }

    JobStatus Poll() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_status;
    }

    UpdateInfo Latest() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_latest;
    }

    bool RelaunchIntoNewVersion() override {
        if (!envHasNextLoad()) return false;
        const std::string self = LauncherSelfPath();
        Result rc = envSetNextLoad(self.c_str(), ("\"" + self + "\"").c_str());
        return R_SUCCEEDED(rc);
    }

    void Cancel() override {
        m_cancel.store(true);
    }
};

UpdateService& Updates() {
    static UpdateServiceImpl instance;
    return instance;
}

} // namespace mkw
