#include <filesystem>
#include <fstream>
#include <iterator>
#include "switch.h"
#include "mkw/services.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <sodium.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdarg>

using namespace mkw;

// Mocks
unsigned char mock_key[32];
unsigned char mock_priv[64];

struct MockCurl {
    std::string url;
    void* write_data;
    size_t (*write_func)(void*, size_t, size_t, void*);
};
std::map<CURL*, MockCurl> curl_handles;

std::string mock_manifest_resp;
std::string mock_sig_resp;
std::string mock_nro_data;

int mock_curl_http_code = 200;
CURLcode mock_curl_res = CURLE_OK;
bool was_nro_downloaded = false;

extern "C" {
    CURL* curl_easy_init() {
        CURL* c = (CURL*)(new int(1)); // dummy ptr
        curl_handles[c] = MockCurl();
        return c;
    }
    CURLcode curl_easy_setopt(CURL* c, CURLoption opt, ...) {
        va_list ap;
        va_start(ap, opt);
        if (opt == CURLOPT_URL) curl_handles[c].url = va_arg(ap, char*);
        else if (opt == CURLOPT_WRITEFUNCTION) {
            typedef size_t (*WriteFunc)(void*, size_t, size_t, void*);
            curl_handles[c].write_func = va_arg(ap, WriteFunc);
        }
        else if (opt == CURLOPT_WRITEDATA) curl_handles[c].write_data = va_arg(ap, void*);
        va_end(ap);
        return CURLE_OK;
    }
    CURLcode curl_easy_perform(CURL* c) {
        if (mock_curl_res != CURLE_OK) return mock_curl_res;
        auto& handle = curl_handles[c];
        std::string resp;
        if (handle.url.find("manifest.json.sig") != std::string::npos) resp = mock_sig_resp;
        else if (handle.url.find("manifest.json") != std::string::npos) resp = mock_manifest_resp;
        else if (handle.url.find("mock_nro") != std::string::npos) {
            resp = mock_nro_data;
            was_nro_downloaded = true;
        }

        if (!resp.empty() && handle.write_func) {
            handle.write_func((void*)resp.data(), 1, resp.size(), handle.write_data);
        }
        return CURLE_OK;
    }
    CURLcode curl_easy_getinfo(CURL* c, CURLINFO info, ...) {
        va_list ap;
        va_start(ap, info);
        if (info == CURLINFO_RESPONSE_CODE) {
            typedef long* LongPtr;
            LongPtr code = va_arg(ap, LongPtr);
            *code = mock_curl_http_code;
        }
        va_end(ap);
        return CURLE_OK;
    }
    void curl_easy_cleanup(CURL* c) {
        curl_handles.erase(c);
        delete (int*)c;
    }
}

JobStatus WaitForUpdate() {
    while (true) {
        JobStatus status = Updates().Poll();
        if (status.state == JobStatus::State::Succeeded || status.state == JobStatus::State::Failed || status.state == JobStatus::State::Cancelled) {
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void sign_manifest() {
    unsigned char sig[64];
    crypto_sign_detached(sig, NULL, (const unsigned char*)mock_manifest_resp.data(), mock_manifest_resp.size(), mock_priv);
    char b64[256];
    sodium_bin2base64(b64, sizeof(b64), sig, sizeof(sig), sodium_base64_VARIANT_ORIGINAL);
    mock_sig_resp = b64;
}

void reset_mocks() {
    mock_manifest_resp = R"({"schema":1,"version":"1.0.1","changelog":"","components":[{"id":"launcher","file":"launcher.nro","size":5,"sha256":"65a8e27d8879283831b664bd8b7f0ad4","url":"http://mock/mock_nro"}]})";
    
    // Correct sha256 for "hello": 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
    // Wait, let's just make sure the mock size/hash matches the mock NRO data
    mock_nro_data = "hello";
    mock_manifest_resp = R"({"schema":1,"version":"1.0.1","changelog":"","components":[{"id":"launcher","file":"launcher.nro","size":5,"sha256":"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824","url":"http://mock/mock_nro"}]})";
    sign_manifest();

    mock_curl_http_code = 200;
    mock_curl_res = CURLE_OK;
    was_nro_downloaded = false;
    
    remove("sdmc:/switch/mkw-launcher/update/mkw-launcher.nro.part");
}

int main() {
    if (sodium_init() < 0) return 1;
    std::filesystem::create_directories("sdmc:/switch/mkw-launcher/update");
    remove("sdmc:/switch/mkw-launcher.nro");
    crypto_sign_keypair(mock_key, mock_priv);
    
    // Test A: valid manifest + detached sig
    std::cout << "Test A: Valid manifest\n";
    reset_mocks();
    Updates().StartCheck(true);
    if (WaitForUpdate().state != JobStatus::State::Succeeded || !Updates().Latest().available) {
        std::cout << "Test A Failed\n";
        return 1;
    }
    
    // Test B: one flipped byte in manifest.json -> Signature error
    std::cout << "Test B: Flipped byte in manifest\n";
    reset_mocks();
    mock_manifest_resp[10] ^= 1; // flip a bit
    Updates().StartCheck(true);
    if (WaitForUpdate().state != JobStatus::State::Failed) {
        std::cout << "Test B Failed (should have failed sig check)\n";
        return 1;
    }
    
    // Test C: bad base64 / wrong length sig -> Failed
    std::cout << "Test C: Bad base64\n";
    reset_mocks();
    mock_sig_resp = "!!!notbase64!!!";
    Updates().StartCheck(true);
    if (WaitForUpdate().state != JobStatus::State::Failed) {
        std::cout << "Test C Failed\n";
        return 1;
    }
    
    // Test D: download sha mismatch -> failed, .part deleted
    std::cout << "Test D: SHA mismatch\n";
    reset_mocks();
    // Valid check first
    Updates().StartCheck(true);
    WaitForUpdate();
    // Tamper the download data
    mock_nro_data = "tampered";
    Updates().StartDownloadAndInstall();
    if (WaitForUpdate().state != JobStatus::State::Failed) {
        std::cout << "Test D Failed (should have failed sha verification)\n";
        return 1;
    }
    struct stat st;
    if (stat("sdmc:/switch/mkw-launcher/update/mkw-launcher.nro.part", &st) == 0) {
        std::cout << "Test D Failed (part file was not deleted)\n";
        return 1;
    }
    
    // Check happy path download
    std::cout << "Test E: Happy path download\n";
    reset_mocks();
    Updates().StartCheck(true);
    WaitForUpdate();
    // Ensure dir exists
    std::filesystem::create_directories("sdmc:/switch/mkw-launcher/update");
    FILE* dummy = fopen("sdmc:/switch/mkw-launcher.nro", "w");
    if(dummy) { fwrite("old", 1, 3, dummy); fclose(dummy); }
    
    Updates().StartDownloadAndInstall();
    JobStatus status = WaitForUpdate();
    if (status.state != JobStatus::State::Succeeded) {
        std::cout << "Test E Failed (failed to download and install). Reason: " << status.message << "\n";
        return 1;
    }

    {
        std::ifstream in("sdmc:/switch/mkw-launcher.nro");
        std::string installed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (installed != "hello") {
            std::cout << "Test E Failed (installed file content wrong)\n";
            return 1;
        }
    }

    // Test F: the installed launcher already matches the release -> no update offered.
    std::cout << "Test F: Up to date\n";
    reset_mocks();
    Updates().StartCheck(true);
    if (WaitForUpdate().state != JobStatus::State::Succeeded || Updates().Latest().available) {
        std::cout << "Test F Failed (offered an update identical to the installed launcher)\n";
        return 1;
    }

    // Test G: first install with no launcher file at the live path.
    std::cout << "Test G: First install\n";
    reset_mocks();
    remove("sdmc:/switch/mkw-launcher.nro");
    remove("sdmc:/switch/mkw-launcher/update/mkw-launcher.nro.prev");
    Updates().StartCheck(true);
    WaitForUpdate();
    Updates().StartDownloadAndInstall();
    status = WaitForUpdate();
    if (status.state != JobStatus::State::Succeeded) {
        std::cout << "Test G Failed. Reason: " << status.message << "\n";
        return 1;
    }

    std::cout << "Manifest tests passed!\n";
    return 0;
}
