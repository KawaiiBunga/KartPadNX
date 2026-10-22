// Wii FST (filesystem table) parser.
//
// The DATA partition's FST is a flat array of 12-byte entries followed by a
// string table. Once a partition is decrypted, walking the FST yields the file
// tree the extraction stage writes to `dvd_root` (`sys/fst.bin` + `files/…`).
//
// This operates purely on an already-decrypted, in-memory FST blob — no disc
// container, no crypto, no keys — so it is unit-tested off-device against a
// synthetic FST. File offsets are returned in bytes (Wii stores them >>2).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kartpad::disc {

struct FstNode {
    std::string path;          // absolute within the partition, e.g. "/Race/x.szs"
    bool isDir = false;
    std::uint64_t offset = 0;  // byte offset of file data within the partition (files)
    std::uint64_t size = 0;    // file size in bytes (files)
};

// Parse a raw Wii FST blob. On success fills `out` (directories and files, in
// FST order, excluding the root) and returns true. On malformed input returns
// false and sets `err`.
bool parseWiiFst(const std::uint8_t* fst, std::size_t len,
                 std::vector<FstNode>& out, std::string& err);

}  // namespace kartpad::disc
