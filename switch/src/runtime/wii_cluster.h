// Wii encrypted-partition cluster geometry.
//
// A Wii DATA partition's encrypted region is a sequence of 0x8000-byte clusters,
// each = a 0x400 hash header followed by 0x7C00 bytes of AES-encrypted payload.
// The decrypted *logical* data (what the FST addresses) is the concatenation of
// the 0x7C00 payloads, so reading a logical byte range means visiting one or more
// clusters, decrypting each, and skipping the per-cluster hash headers.
//
// This is pure geometry — no crypto, no keys, no I/O — so the extraction stage
// (which does need the user's Wii common key) can rely on a unit-tested mapping.
#pragma once

#include <cstdint>
#include <vector>

namespace kartpad::disc {

inline constexpr std::uint64_t kWiiClusterSize = 0x8000;        // physical, on disc
inline constexpr std::uint64_t kWiiClusterHeaderSize = 0x400;   // hash block
inline constexpr std::uint64_t kWiiClusterDataSize = 0x7C00;    // decrypted payload

// One contiguous run of wanted payload bytes that lives inside a single cluster.
struct ClusterRun {
    std::uint64_t clusterIndex = 0;      // 0-based cluster in the partition data
    std::uint64_t encClusterOffset = 0;  // byte offset of the cluster (its header)
                                         // within the encrypted partition-data region
    std::uint64_t payloadOffset = 0;     // offset inside the 0x7C00 decrypted payload
    std::uint64_t length = 0;            // payload bytes taken from this cluster
    std::uint64_t logicalOffset = 0;     // logical offset these bytes map to
};

// Split a logical [logicalOffset, logicalOffset+length) request into per-cluster
// runs, in order. Each run's length is at most kWiiClusterDataSize.
std::vector<ClusterRun> wiiClusterRuns(std::uint64_t logicalOffset, std::uint64_t length);

}  // namespace kartpad::disc
