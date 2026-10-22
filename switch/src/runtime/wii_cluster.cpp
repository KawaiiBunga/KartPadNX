#include "wii_cluster.h"

#include <algorithm>

namespace kartpad::disc {

std::vector<ClusterRun> wiiClusterRuns(std::uint64_t logicalOffset, std::uint64_t length) {
    std::vector<ClusterRun> runs;
    std::uint64_t remaining = length;
    std::uint64_t cur = logicalOffset;

    while (remaining > 0) {
        const std::uint64_t cluster = cur / kWiiClusterDataSize;
        const std::uint64_t inPayload = cur % kWiiClusterDataSize;
        const std::uint64_t take =
            std::min<std::uint64_t>(remaining, kWiiClusterDataSize - inPayload);

        ClusterRun run;
        run.clusterIndex = cluster;
        run.encClusterOffset = cluster * kWiiClusterSize;
        run.payloadOffset = inPayload;
        run.length = take;
        run.logicalOffset = cur;
        runs.push_back(run);

        cur += take;
        remaining -= take;
    }
    return runs;
}

}  // namespace kartpad::disc
