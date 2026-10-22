#pragma once

#include <cstdint>
#include <cerrno>
#include <cstdlib>

enum class HandoffStatus : int {
    NotProvided = 0,
    Valid = 1,
    Malformed = 2,
    RangeError = 3,
    FutureTimestamp = 4,
    ImplausiblyLow = 5,
    Stale = 6,
};

// Plausibility constraints for launcher handoff:
// - Lower bound: 1,700,000,000,000 ms (~Nov 2023). Values below indicate uninitialized/corrupt clock.
// - Upper bound: 600,000 ms (10 minutes). Handoffs exceeding 10 minutes indicate stale session files
//   or external clock tampering.
inline constexpr uint64_t kMinPlausibleEpochMs = 1700000000000ULL;
inline constexpr uint64_t kMaxPlausibleHandoffMs = 600000ULL;

inline bool ParseLaunchTimestamp(
    const char* str,
    uint64_t entryEpochMs,
    uint64_t& outTsMs,
    uint64_t& outLatencyMs,
    HandoffStatus& outStatus)
{
    outTsMs = 0;
    outLatencyMs = 0;

    if (!str || *str == '\0') {
        outStatus = HandoffStatus::Malformed;
        return false;
    }

    // Strict validation: ONLY ASCII digits '0'-'9'.
    // Reject any leading/trailing whitespace, signs (+ or -), decimals, or non-digit characters.
    for (const char* p = str; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            outStatus = HandoffStatus::Malformed;
            return false;
        }
    }

    // Parse unsigned decimal value with errno range checking.
    char* end = nullptr;
    errno = 0;
    const unsigned long long val = std::strtoull(str, &end, 10);
    if (errno == ERANGE || end == str || *end != '\0') {
        outStatus = HandoffStatus::RangeError;
        return false;
    }

    outTsMs = static_cast<uint64_t>(val);

    // Check plausibility floor.
    if (outTsMs < kMinPlausibleEpochMs) {
        outStatus = HandoffStatus::ImplausiblyLow;
        return false;
    }

    // Check future timestamp / clock skew.
    if (outTsMs > entryEpochMs) {
        outStatus = HandoffStatus::FutureTimestamp;
        return false;
    }

    // Check handoff latency.
    const uint64_t delta = entryEpochMs - outTsMs;
    if (delta > kMaxPlausibleHandoffMs) {
        outStatus = HandoffStatus::Stale;
        outLatencyMs = delta;
        return false;
    }

    outStatus = HandoffStatus::Valid;
    outLatencyMs = delta;
    return true;
}
