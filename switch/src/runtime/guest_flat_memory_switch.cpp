// Switch (libnx) backing for the flat 4 GiB guest address space.
//
// The Windows build reserves a fixed 16 TiB base and aliases MEM1/MEM2's
// cached/uncached/physical mirrors onto one file-mapping via MapViewOfFile3; the
// Apple iOS build does the same with an fd and several MAP_SHARED maps. Horizon
// has neither, but the on-console probe (switch/tests/memprobe) proved that a
// single shared-memory object maps at several VAs at once with coherent RW. So
// each backing "section" is one svcCreateSharedMemory object, mapped once as a
// plain host alias (what native code writes through) and once per guest mirror
// inside a dynamically reserved 4 GiB flat range.
//
// Not implemented: page-protection interception. Deferred (EFB) reads and the
// executable-write guard use the checked path (runtime/src/memory.cpp) instead,
// so the trap hooks here are no-ops and
// HandleAccessViolation returns false. Depends only on guest_flat_memory.h +
// libnx, so it builds and is checked independently of the translated ISA headers.
// The defining TU of GuestFlat::gFlatGuestBase sees it mutable (guest_flat_memory.h).
#define MKW_FLAT_GUEST_BASE_STORAGE 1
#include "guest_flat_memory.h"

#include <switch.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" int kartpad_opt(const char* key, int fallback) __attribute__((weak));
extern "C" void kartpad_boot_log_phase(const char* phase) __attribute__((weak));

namespace GuestFlat {

// Published base for MKW_FLAT_GUEST_BASE (see guest_flat_memory.h __SWITCH__). Only this TU
// (MKW_FLAT_GUEST_BASE_STORAGE) sees it mutable; everyone else reads it as a const.
__attribute__((visibility("hidden"))) uint8_t* gFlatGuestBase = nullptr;

namespace {

constexpr uint64_t kAllocationGranularity = 0x10000;  // 64 KiB, matches Win build
constexpr uint64_t kReservationGuard = kAllocationGranularity;

uint8_t* g_base = nullptr;
VirtmemReservation* g_reservation = nullptr;
bool g_initialized = false;
std::vector<RegionRequest> g_activeRegions;
std::mutex g_stateMutex;

std::atomic<uint32_t> g_countUnmapped{0};

struct SectionKey {
    Backing backing = Backing::Owned;
    uint32_t ownedBase = 0;
    bool operator==(const SectionKey& o) const {
        return backing == o.backing && ownedBase == o.ownedBase;
    }
};
struct SectionKeyHash {
    size_t operator()(const SectionKey& k) const {
        return (static_cast<size_t>(k.ownedBase) << 3) ^ static_cast<size_t>(k.backing);
    }
};
struct Section {
    Handle handle = INVALID_HANDLE;
    uint64_t size = 0;
    uint8_t* hostView = nullptr;
};
struct MappedRegion {
    uint32_t guestBase = 0;
    uint64_t guestSize = 0;
    uint64_t sectionOffset = 0;
    uint8_t* hostView = nullptr;
};

std::unordered_map<SectionKey, Section, SectionKeyHash> g_sections;
std::vector<MappedRegion> g_mappedRegions;

uint64_t RoundUp(uint64_t v, uint64_t a) { return (v + a - 1u) & ~(a - 1u); }

uint64_t SectionOffsetFor(const RegionRequest& region) {
    // Each MEM bank is split into individual shared-memory slices. The region
    // already identifies one slice, so its offset within that object is zero.
    (void)region;
    return 0;
/*
    switch (region.backing) {
    case Backing::Mem1: return region.base & 0x01FFFFFFu;  // 32 MiB MEM1 window
    case Backing::Mem2: return region.base & 0x0FFFFFFFu;  // 256 MiB MEM2 window
    case Backing::Owned:
    default: return 0;
    }
*/
}
SectionKey KeyFor(const RegionRequest& region) {
    SectionKey k;
    k.backing = region.backing;
    switch (region.backing) {
    case Backing::Mem1: k.ownedBase = region.base & 0x01FFFFFFu; break;
    case Backing::Mem2: k.ownedBase = region.base & 0x0FFFFFFFu; break;
    case Backing::Owned: default: k.ownedBase = region.base; break;
    }
    return k;
}

[[noreturn]] void Fail(const std::string& what, Result rc) {
    std::ostringstream oss;
    oss << what << " (rc=0x" << std::hex << rc << ")";
    throw std::runtime_error(oss.str());
}

// The shared-memory pool that backs guest RAM turned out to be capped at only a
// few MiB on hardware. Whether that is the applet-mode memory limit, a pool
// already spent by Aurora/NVK before Memory::Init runs, or a dedicated kernel
// resource region decides the fix (launch as an application vs. re-architect the
// backing), so surface the process memory environment in the failure artifact.
std::string EnvInfo() {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    const u64 freeMem = (total > used) ? (total - used) : 0;
    std::ostringstream oss;
    oss << " {env applet=" << static_cast<int>(appletGetAppletType())
        << " totalMem=0x" << std::hex << total
        << " usedMem=0x" << used << std::dec
        << " freeMem=" << (freeMem >> 20) << "MiB}";
    return oss.str();
}

bool SameLayout(const std::vector<RegionRequest>& a, const std::vector<RegionRequest>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].base != b[i].base || a[i].size != b[i].size || a[i].backing != b[i].backing)
            return false;
    }
    return true;
}

void ZeroSections() {
    for (auto& [key, s] : g_sections) {
        (void)key;
        if (s.hostView && s.size) std::memset(s.hostView, 0, static_cast<size_t>(s.size));
    }
}

// perf.cfg `flat_align` (default 1). Horizon maps a shared-memory page group with the
// largest block whose size divides both the virtual and the physical address (4 KiB
// page, 64 KiB contiguous run, 2 MiB block, 32 MiB contiguous block). virtmemFindAslr
// returns a random 4 KiB-aligned address, so the old placement left every guest view on
// 4 KiB pages: ~88 MiB of guest RAM against a 1024-entry L2 TLB (4 MiB reach). The race
// profile is dominated by dependent guest loads, each paying a page walk on a TLB miss.
// Aligning every view to 32 MiB lets the kernel use block mappings wherever the
// allocator handed out large physically aligned blocks; offsets keep their meaning.
constexpr uint64_t kHugeAlign = 0x2000000;  // 32 MiB: L2 contiguous block, covers 2 MiB

bool FlatAlignEnabled() {
    static const bool enabled = kartpad_opt == nullptr || kartpad_opt("flat_align", 1) != 0;
    return enabled;
}

// Caller holds virtmemLock. Returns an `align`-aligned address with `size` free bytes.
void* FindAslrAligned(uint64_t size, uint64_t align, uint64_t skew = 0) {
    void* raw = virtmemFindAslr(size + align + skew, 0);
    if (!raw) return nullptr;
    const uint64_t a = RoundUp(reinterpret_cast<uint64_t>(raw), align) + skew;
    return reinterpret_cast<void*>(a);
}

void ReserveFlatSpace() {
    if (g_base) return;
    // Guards on both sides of the 4 GiB: base + displacement accesses (memory_access.h
    // Flat*At, |d| < 64 KiB) of a garbage pointer near 0 or 4 GiB land in reserved, unmapped
    // space and fault, as the wrapping 32-bit address did.
    const uint64_t span = kReservationGuard + kGuestSpaceSize + kReservationGuard;
    const uint64_t align = FlatAlignEnabled() ? kHugeAlign : kAllocationGranularity;
    virtmemLock();
    // [raw, raw + span + align) is free; the base is the first aligned address at least one guard
    // above raw, so [base - guard, base + 4 GiB + guard) lies inside it.
    void* found = nullptr;
    if (void* raw = virtmemFindAslr(span + align, 0)) {
        const uint64_t base = RoundUp(reinterpret_cast<uint64_t>(raw) + kReservationGuard, align);
        found = reinterpret_cast<void*>(base);
        g_reservation = virtmemAddReservation(reinterpret_cast<void*>(base - kReservationGuard), span);
    }
    virtmemUnlock();
    if (!found || !g_reservation) {
        throw std::runtime_error(
            "Unable to reserve the 4 GiB flat guest address space (virtmemFindAslr). The "
            "translated code addresses guest memory through this single base, so it cannot "
            "fall back to a smaller region without a rebuild.");
    }
    g_base = static_cast<uint8_t*>(found);
    gFlatGuestBase = g_base;
}

// perf.cfg `flat_tlb_probe` (default 1): one-shot dependent-load latency probe run right after
// the first mapping, before any guest data exists. It chases a random cycle of cache lines, one
// per distinct random 4 KiB page of the largest section, through two views of the SAME physical
// memory: the real flat guest view and a temporary alias deliberately skewed by 4 KiB (which
// forces 4 KiB page entries). Same data, same caches; only the translation granule differs, so
// the difference is the TLB cost the guest loads pay. "small" fits the data cache but not a
// 4 KiB-page TLB; "large" also misses the cache.
struct ProbeResult {
    double flatNs = 0.0;
    double skewNs = 0.0;
};

double ChaseNs(const uint8_t* view, uint32_t start, uint32_t steps) {
    uint32_t p = start;
    for (uint32_t i = 0; i < steps / 8u; ++i) {  // warm: TLB and caches as steady state allows
        p = *reinterpret_cast<const volatile uint32_t*>(view + p);
    }
    const u64 t0 = armGetSystemTick();
    for (uint32_t i = 0; i < steps; ++i) {
        p = *reinterpret_cast<const volatile uint32_t*>(view + p);
    }
    const u64 t1 = armGetSystemTick();
    asm volatile("" ::"r"(p));
    return static_cast<double>(armTicksToNs(t1 - t0)) / static_cast<double>(steps);
}

ProbeResult ProbeOne(uint8_t* host, const uint8_t* flat, const uint8_t* skew, uint64_t size,
                     uint32_t nodes, uint32_t seed) {
    const uint32_t pages = static_cast<uint32_t>(size >> 12);
    nodes = std::min(nodes, pages);
    uint32_t state = seed;
    const auto next = [&state]() {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return state;
    };
    std::vector<uint32_t> order(pages);
    for (uint32_t i = 0; i < pages; ++i) order[i] = i;
    for (uint32_t i = 0; i < nodes; ++i) std::swap(order[i], order[i + next() % (pages - i)]);
    std::vector<uint32_t> offsets(nodes);
    std::vector<uint32_t> saved(nodes);
    for (uint32_t i = 0; i < nodes; ++i) offsets[i] = (order[i] << 12) | ((next() & 63u) << 6);
    for (uint32_t i = 0; i < nodes; ++i) {
        uint32_t* slot = reinterpret_cast<uint32_t*>(host + offsets[i]);
        saved[i] = *slot;
        *slot = offsets[(i + 1) % nodes];
    }
    const uint32_t steps = std::max<uint32_t>(nodes * 4u, 32768u);
    ProbeResult r;
    r.flatNs = ChaseNs(flat, offsets[0], steps);
    r.skewNs = ChaseNs(skew, offsets[0], steps);
    r.flatNs = std::min(r.flatNs, ChaseNs(flat, offsets[0], steps));  // order-independent
    for (uint32_t i = 0; i < nodes; ++i) {
        *reinterpret_cast<uint32_t*>(host + offsets[i]) = saved[i];
    }
    return r;
}

// Profiling builds (FullApp.cmake KARTPAD_PROFILE_LINES) run it by default: one line in the log
// says whether the guest views got block mappings.
#ifndef KARTPAD_TLB_PROBE_DEFAULT
#define KARTPAD_TLB_PROBE_DEFAULT 0
#endif

void RunTlbProbe(std::string& out) {
    if (kartpad_opt == nullptr || kartpad_opt("flat_tlb_probe", KARTPAD_TLB_PROBE_DEFAULT) == 0) return;
    const Section* best = nullptr;
    uint32_t bestGuest = 0;
    for (const auto& region : g_mappedRegions) {
        for (const auto& [key, s] : g_sections) {
            (void)key;
            if (s.hostView == region.hostView && s.size == RoundUp(region.guestSize, kAllocationGranularity) &&
                (best == nullptr || s.size > best->size)) {
                best = &s;
                bestGuest = region.guestBase;
            }
        }
    }
    if (best == nullptr || best->size < (4u << 20)) return;
    const uint8_t* flat = g_base + bestGuest;

    virtmemLock();
    uint8_t* skew = static_cast<uint8_t*>(FindAslrAligned(best->size, kHugeAlign, 0x1000));
    Result rc = skew ? svcMapSharedMemory(best->handle, skew, best->size, Perm_Rw) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    if (R_FAILED(rc)) {
        virtmemUnlock();
        out = "[mem] tlb probe skipped: skewed alias map failed";
        return;
    }
    const ProbeResult small = ProbeOne(best->hostView, flat, skew, best->size, 2048, 0x9e3779b9u);
    const ProbeResult large = ProbeOne(best->hostView, flat, skew, best->size, 8192, 0x85ebca6bu);
    svcUnmapSharedMemory(best->handle, skew, best->size);
    virtmemUnlock();

    char line[384];
    std::snprintf(line, sizeof(line),
                  "[mem] tlb probe guest=0x%08x size=%lluMiB flat_align=%d flat%%32M=0x%llx host%%32M=0x%llx | "
                  "small(2048 pages) flat=%.1fns skew4k=%.1fns | large(8192 pages) flat=%.1fns skew4k=%.1fns",
                  bestGuest, static_cast<unsigned long long>(best->size >> 20), FlatAlignEnabled() ? 1 : 0,
                  static_cast<unsigned long long>(reinterpret_cast<uint64_t>(flat) & (kHugeAlign - 1)),
                  static_cast<unsigned long long>(reinterpret_cast<uint64_t>(best->hostView) & (kHugeAlign - 1)),
                  small.flatNs, small.skewNs, large.flatNs, large.skewNs);
    out = line;
}

}  // namespace

bool IsActive() { return g_initialized; }

void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (g_initialized) {
        if (!SameLayout(g_activeRegions, regions)) {
            throw std::runtime_error(
                "The flat guest address space is mapped once per process; a second Memory::Init "
                "requested a different layout. Restart instead of remapping.");
        }
        ZeroSections();  // pristine re-init; no protections to restore in bring-up
        return;
    }

    ReserveFlatSpace();

    // Size each section from the highest byte any of its regions reaches.
    std::unordered_map<SectionKey, uint64_t, SectionKeyHash> sizes;
    for (const auto& r : regions) {
        if (r.size == 0) continue;
        if ((r.base % kAllocationGranularity) != 0) {
            std::ostringstream oss;
            oss << "Guest region base 0x" << std::hex << r.base
                << " is not 64 KiB aligned; the flat mapping cannot place it.";
            throw std::runtime_error(oss.str());
        }
        uint64_t& cur = sizes[KeyFor(r)];
        cur = std::max(cur, SectionOffsetFor(r) + r.size);
    }

    // Create one shared-memory object per section and map its plain host alias.
    // Track how far we get so a KernelError_OutOfResource failure names the
    // failing object index, its size, and the physical committed so far - the
    // datum that separates a per-object size limit (fails at index 0) from a
    // cumulative resource budget (fails after N MiB), which the bare rc cannot.
    const size_t sectionCount = sizes.size();
    size_t sectionIndex = 0;
    uint64_t committedBytes = 0;
    const auto sectionTag = [&](const SectionKey& key, uint64_t rounded) {
        std::ostringstream oss;
        oss << " [section " << sectionIndex << "/" << sectionCount
            << " backing=" << static_cast<int>(key.backing)
            << " ownedBase=0x" << std::hex << key.ownedBase
            << " size=0x" << rounded
            << " committedSoFar=0x" << committedBytes << std::dec << "]";
        return oss.str();
    };
    for (auto& [key, size] : sizes) {
        const uint64_t rounded = RoundUp(size, kAllocationGranularity);
        Section s;
        s.size = rounded;
        Result rc = svcCreateSharedMemory(&s.handle, rounded, Perm_Rw, Perm_Rw);
        if (R_FAILED(rc))
            Fail("svcCreateSharedMemory for guest RAM" + sectionTag(key, rounded) + EnvInfo(), rc);

        // Find and map under one lock (as libnx shmemMap does) so the aligned range cannot be
        // taken between the search and the map.
        virtmemLock();
        void* hostVa = FlatAlignEnabled() ? FindAslrAligned(rounded, kHugeAlign) : virtmemFindAslr(rounded, 0);
        if (!hostVa) {
            virtmemUnlock();
            svcCloseHandle(s.handle);
            throw std::runtime_error("virtmemFindAslr for the host guest-RAM alias failed" +
                                     sectionTag(key, rounded));
        }
        rc = svcMapSharedMemory(s.handle, hostVa, rounded, Perm_Rw);
        virtmemUnlock();
        if (R_FAILED(rc)) {
            svcCloseHandle(s.handle);
            Fail("svcMapSharedMemory for the host guest-RAM alias" + sectionTag(key, rounded), rc);
        }
        s.hostView = static_cast<uint8_t*>(hostVa);
        std::memset(s.hostView, 0, static_cast<size_t>(rounded));
        g_sections[key] = s;
        committedBytes += rounded;
        ++sectionIndex;
    }

    // Map each guest region as an aliased view inside the flat reservation. Every
    // section offset is 0 for the MKW layout (cached/uncached/physical mirrors all
    // mask to 0), so each view maps the whole object at its guest base.
    for (const auto& r : regions) {
        if (r.size == 0) continue;
        const Section& s = g_sections[KeyFor(r)];
        const uint64_t offset = SectionOffsetFor(r);
        if (offset != 0) {
            // svcMapSharedMemory has no sub-object offset; a non-zero offset would
            // need a separate object. The MKW layout never hits this.
            std::ostringstream oss;
            oss << "Guest region 0x" << std::hex << r.base
                << " needs a non-zero section offset (0x" << offset
                << "); unsupported on the shared-memory backing.";
            throw std::runtime_error(oss.str());
        }
        const uint64_t mappedSize =
            std::min<uint64_t>(RoundUp(r.size, kAllocationGranularity), s.size);
        Result rc = svcMapSharedMemory(s.handle, g_base + r.base, mappedSize, Perm_Rw);
        if (R_FAILED(rc)) {
            std::ostringstream oss;
            oss << "mapping guest region 0x" << std::hex << r.base << " (+0x" << mappedSize
                << ") into the flat reservation";
            Fail(oss.str(), rc);
        }
        g_mappedRegions.push_back(MappedRegion{r.base, r.size, offset, s.hostView});
    }

    g_activeRegions = regions;
    g_initialized = true;

    std::string probeLine;
    RunTlbProbe(probeLine);

    std::fprintf(stderr,
                 "[mem] guest space reserved at %p (%zu regions, %zu backing stores, "
                 "%llu MiB committed, flat_align=%d)\n",
                 static_cast<void*>(g_base), g_mappedRegions.size(), g_sections.size(),
                 static_cast<unsigned long long>(committedBytes >> 20), FlatAlignEnabled() ? 1 : 0);
    if (!probeLine.empty()) std::fprintf(stderr, "%s\n", probeLine.c_str());

    // Diagnostic: write the guest flat layout to its own SD file, closed
    // immediately so it survives any later hard crash. Gated behind opt-in
    // perf toggle (boot_dump_guestmap, default 0) so normal boots don't stall on SD writes.
    if (kartpad_opt != nullptr && kartpad_opt("boot_dump_guestmap", kartpad_opt("dump_guestmap", 0)) != 0) {
        if (std::FILE* gm = std::fopen("sdmc:/switch/kartpad/guestmap.txt", "w")) {
            u64 total = 0, used = 0;
            svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
            svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            std::fprintf(gm,
                         "[guestmap] flatBase=%p reservationSpan=4GiB regions=%zu sections=%zu "
                         "committed=%lluMiB free=%lluMiB\n",
                         static_cast<void*>(g_base), g_mappedRegions.size(), g_sections.size(),
                         static_cast<unsigned long long>(committedBytes >> 20),
                         static_cast<unsigned long long>((total > used) ? (total - used) >> 20 : 0));
            for (const auto& r : g_mappedRegions) {
                const u64 flatVa = reinterpret_cast<u64>(g_base) + r.guestBase;
                std::fprintf(gm,
                             "  guest[0x%08llx +0x%08llx] -> flat[0x%011llx +0x%08llx] host=%p secOff=0x%llx\n",
                             static_cast<unsigned long long>(r.guestBase),
                             static_cast<unsigned long long>(r.guestSize),
                             static_cast<unsigned long long>(flatVa),
                             static_cast<unsigned long long>(r.guestSize),
                             static_cast<void*>(r.hostView),
                             static_cast<unsigned long long>(r.sectionOffset));
            }
            if (!probeLine.empty()) std::fprintf(gm, "%s\n", probeLine.c_str());
            std::fprintf(gm, "[guestmap] end\n");
            std::fflush(gm);
            fsync(fileno(gm));
            std::fclose(gm);
        }
    }

    if (kartpad_boot_log_phase) {
        kartpad_boot_log_phase("guest_flat_memory_initialized");
    }
}

uint8_t* HostPointer(uint32_t guestAddress) {
    if (!g_initialized) return nullptr;
    for (const auto& region : g_mappedRegions) {
        if (guestAddress < region.guestBase) continue;
        const uint64_t offset = static_cast<uint64_t>(guestAddress) - region.guestBase;
        if (offset >= region.guestSize) continue;
        return region.hostView + region.sectionOffset + offset;
    }
    return nullptr;
}

// Bring-up: no page-protection interception. The checked path handles deferred
// reads and executable writes, so these are intentionally no-ops for now.
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}

FaultCounters Counters() {
    FaultCounters c;
    c.unmapped = g_countUnmapped.load(std::memory_order_relaxed);
    return c;
}

void LogFaultSummary() noexcept {
    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_relaxed)) return;
    const uint32_t unmapped = g_countUnmapped.load(std::memory_order_relaxed);
    if (unmapped != 0) {
        std::fprintf(stderr, "[mem] shutdown summary: %u unmapped guest touches\n", unmapped);
    }
}

// No vectored/exception handler is installed on Switch during bring-up.
bool HandleAccessViolation(void*) noexcept { return false; }

}  // namespace GuestFlat
