#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace kartpad {
// ArchiveMgr::Wait can run other guest threads while archive jobs complete.
// Its incoming register file must reflect the caller's resident locals before
// it saves/restores guest frames. This publishes state; it never repairs a
// return value or restores registers after the SDK call.
inline void PublishArchiveWaitCheckpoint(
    uint32_t* registers, std::initializer_list<std::pair<unsigned, uint32_t>> locals,
    uint32_t returnAddress) {
    uint32_t changed = 0;
    const uint32_t oldSp = registers[1], oldR30 = registers[30], oldR31 = registers[31];
    for (auto [index, value] : locals) {
        if (registers[index] != value) changed |= 1u << index;
        registers[index] = value;
    }
    static std::atomic<unsigned> reports{0};
    if ((changed & 0xffffe006u) && reports.fetch_add(1, std::memory_order_relaxed) < 16)
        std::fprintf(stderr,
            "[archive-checkpoint] return=%08x published=%08x sp=%08x->%08x "
            "r30=%08x->%08x r31=%08x->%08x\n", returnAddress, changed,
            oldSp, registers[1], oldR30, registers[30], oldR31, registers[31]);
}
} // namespace kartpad
