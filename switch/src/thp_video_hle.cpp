#include "thp_video_decoder.h"
#include "isa/ppc_isa_context.h"
#include "memory.h"
#include "perf_switch.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <vector>

extern "C" void __real_func_801B3BAC(CpuContext* ctx);
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t address, uint32_t size);
extern "C" bool OS_HLE_WaitHostWork(CpuContext* ctx, const std::atomic<bool>* done);
extern "C" void __real_func_805FBB60(CpuContext* ctx);
extern "C" uint64_t GX_HLE_DrawDonesCompleted(void);
extern "C" void GX_HLE_WaitDrawn(void);
extern "C" int kartpad_opt(const char* key, int fallback) __attribute__((weak));

namespace {
constexpr uint32_t kPlayer = 0x809BEB00u;
constexpr uint32_t kDecodeReturn = 0x80552C2Cu;
constexpr uint32_t kRunningThread = 0x800000E4u;  // OSGetCurrentThread

// The movie's draw (0x805FBB60: three I8 plane textures, one quad) ends with GXDrawDone, so that the
// texture set it drew can go back to the decoder: a drain of the whole frame's GX work behind it,
// every movie frame (test 13: 16% of the game thread). The texture set is only ever written by the
// decode call below, which first makes sure a full GXDrawDone has completed since the movie's last
// draw (the frame's own one at 0x80219FB0 usually has); so that draw's wait is skipped (queued
// without the wait). perf.cfg `thp_drawdone_skip 0` restores it.
std::atomic<uint32_t> g_movieDrawThread{0};  // the guest thread inside the movie draw
// Fiber host only: the decode call and the movie draw both run there.
bool g_movieDrawOwed = false;
uint64_t g_movieDrawMark = 0;  // GX_HLE_DrawDonesCompleted() at the last skipped wait

bool MovieDrawSkipEnabled() {
    static const bool enabled = kartpad_opt == nullptr || kartpad_opt("thp_drawdone_skip", 1) != 0;
    return enabled;
}

// Before a decode call writes a texture set: a full GXDrawDone completed after the last skipped one.
void EnsureMovieDrawn() {
    if (!g_movieDrawOwed) return;
    if (GX_HLE_DrawDonesCompleted() == g_movieDrawMark) GX_HLE_WaitDrawn();
    g_movieDrawOwed = false;
}

bool RamRange(uint32_t address, size_t size) {
    const uint32_t physical = address & 0x3fffffffu;
    const uint64_t end = static_cast<uint64_t>(physical) + size;
    return address && size &&
        ((physical < Memory::kMem1Size && end <= Memory::kMem1Size) ||
         (physical >= Memory::kMem2PhysicalBase && end <= Memory::kMem2PhysicalEnd)) &&
        Memory::Contains(address, size);
}

// Every guest thread is a fiber on the one guest core, so the video thread's libjpeg decode took that
// core from the game (movie menus, tests 12-13: a third of the guest core, 31-35 FPS). Decoding the
// next filled read buffer ahead (rr-f4) rarely found one: the read thread hands each frame over just
// in time (test 13: 127 of 4,097 frames). So the call itself runs on a worker core while the video
// thread is parked as a guest thread (OS_HLE_WaitHostWork) and the guest core runs the game; the
// call then copies the planes into its texture set. The worker reads only its own copy of the packet
// and writes only host memory; guest memory is written by the guest's call alone. A job is claimed by
// its generation, so a call whose job was replaced (its thread cancelled, or a second caller) decodes
// on the guest core as before.
class Worker {
public:
    // Hands `packet` to the worker; its generation, or 0 when the worker is busy or cannot start.
    uint64_t Start(std::span<const uint8_t> packet, const ThpVideo::FrameInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Queued) return 0;  // the worker owns the buffers until it is done
        if (!started_) {
            // Started before any job is queued: without a worker, Finish must never wait for one.
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 1u << 20);  // libjpeg-turbo, as on the guest's host stack
            pthread_t thread;
            const int rc = pthread_create(&thread, &attr, [](void* self) -> void* {
                static_cast<Worker*>(self)->Run();
                return nullptr;
            }, this);
            pthread_attr_destroy(&attr);
            if (rc != 0) return 0;
            pthread_detach(thread);
            started_ = true;
        }
        packet_.assign(packet.begin(), packet.end());
        for (size_t c = 0; c < 3; ++c) planes_[c].resize(info.PlaneSize(c));
        done_.store(false, std::memory_order_relaxed);
        state_ = State::Queued;
        cv_.notify_all();
        return ++generation_;
    }

    const std::atomic<bool>* Done() const { return &done_; }

    // Job `generation`'s planes into `out`, waiting on the host for a decode still running (its
    // thread could not park). Unsupported when that job was replaced.
    ThpVideo::Status Finish(uint64_t generation, const std::array<ThpVideo::Plane, 3>& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return state_ != State::Queued; });
        if (state_ != State::Done || generation != generation_) return ThpVideo::Status::Unsupported;
        state_ = State::Idle;
        if (status_ == ThpVideo::Status::Success) {
            for (size_t c = 0; c < 3; ++c) std::memcpy(out[c].data, planes_[c].data(), out[c].size);
        }
        return status_;
    }

    uint64_t parked = 0, waited = 0, local = 0;

private:
    enum class State { Idle, Queued, Done };

    void Run() {
        kartpad_thread_apply(KARTPAD_ROLE_VIDEO_DECODE, 0);  // a worker core, never the guest's
        ThpVideo::Decoder decoder;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            cv_.wait(lock, [this] { return state_ == State::Queued; });
            lock.unlock();
            std::array<ThpVideo::Plane, 3> planes;
            for (size_t c = 0; c < 3; ++c) planes[c] = {planes_[c].data(), planes_[c].size()};
            ThpVideo::Status status = ThpVideo::Status::DecodeError;
            try {
                status = decoder.Decode(packet_, planes);
            } catch (const std::exception&) {
            }
            lock.lock();
            status_ = status;
            state_ = State::Done;
            done_.store(true, std::memory_order_release);
            cv_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool started_ = false;
    State state_ = State::Idle;
    uint64_t generation_ = 0;
    std::atomic<bool> done_{false};
    std::vector<uint8_t> packet_;
    std::array<std::vector<uint8_t>, 3> planes_;
    ThpVideo::Status status_ = ThpVideo::Status::Unsupported;
};

Worker& GetWorker() {
    static Worker* worker = new Worker;  // its thread never exits; never destroyed
    return *worker;
}

// The decode on the worker with this guest thread parked; false when the worker is busy or the
// job came back replaced or failed (the caller then decodes on the guest core).
bool DecodeOnWorker(CpuContext* ctx, std::span<const uint8_t> packet, const ThpVideo::FrameInfo& info,
                    const std::array<ThpVideo::Plane, 3>& planes) {
    Worker& worker = GetWorker();
    const uint64_t generation = worker.Start(packet, info);
    if (generation == 0) return false;
    // Parking switches threads: this call's registers come back as the caller left them, r3 aside.
    const CpuContext entry = *ctx;
    const bool parked = OS_HLE_WaitHostWork(ctx, worker.Done());
    ctx->gpr[0] = entry.gpr[0];
    for (int r = 2; r <= 12; ++r) ctx->gpr[r] = entry.gpr[r];
    ctx->gpr[1] = entry.gpr[1];
    ctx->lr = entry.lr;
    ctx->ctr = entry.ctr;
    ctx->cr = entry.cr;
    ctx->xer = entry.xer;
    EnsureMovieDrawn();
    if (worker.Finish(generation, planes) != ThpVideo::Status::Success) return false;
    ++(parked ? worker.parked : worker.waited);
    return true;
}

bool TryNativeDecode(CpuContext* ctx, const char*& reason) {
    // THPVideoDecode has no compressed-size argument. This PAL player call site
    // keeps the component-size cursor in r25 and the component index in r26.
    // Validate them against the frame/player layout instead of scanning arbitrary
    // guest memory for an EOI (FF D9 can occur inside THP entropy).
    reason = "unrecognized THP caller";
    if (ctx->lr != kDecodeReturn || ctx->gpr[30] != kPlayer ||
        !RamRange(kPlayer, 424) || !RamRange(ctx->gpr[23], 4)) return false;
    const uint32_t components = Memory::Read32(kPlayer + 108);
    const uint32_t index = ctx->gpr[26];
    const uint32_t frame = Memory::Read32(ctx->gpr[23]);
    const uint32_t capacity = Memory::Read32(kPlayer + 68);
    reason = "invalid THP frame bounds";
    if (!components || components > 16 || index >= components ||
        capacity > ThpVideo::kMaxPacketSize || capacity < 8 + components * 4 ||
        !RamRange(frame, capacity) || ctx->gpr[25] != frame + 8 + index * 4 ||
        Memory::Read8(kPlayer + 112 + index) != 0) return false;
    uint64_t offset = 8 + components * 4;
    for (uint32_t i = 0; i < index; ++i) offset += Memory::Read32(frame + 8 + i * 4);
    const uint32_t bytes = Memory::Read32(ctx->gpr[25]);
    if (!bytes || offset + bytes > capacity || frame + offset != ctx->gpr[3] ||
        ctx->gpr[24] != ctx->gpr[3]) return false;
    const std::span<const uint8_t> packet{static_cast<const uint8_t*>(Memory::GetPointer(ctx->gpr[3], bytes)), bytes};
    ThpVideo::FrameInfo info;
    const auto inspected = ThpVideo::InspectFrame(packet, info);
    reason = ThpVideo::StatusName(inspected);
    if (inspected != ThpVideo::Status::Success) return false;

    reason = "THP dimensions or output buffers disagree with player";
    if (info.width != Memory::Read32(kPlayer + 128) || info.height != Memory::Read32(kPlayer + 132))
        return false;
    // The player's three YUV buffers are allocated from these dimensions.
    // Accept only the exact buffer popped by the existing guest video thread.
    const uint32_t textureSet = ctx->gpr[27];
    if (textureSet < kPlayer + 376 || textureSet > kPlayer + 408 ||
        ((textureSet - kPlayer - 376) % 16)) return false;
    std::array<ThpVideo::Plane, 3> planes;
    for (size_t c = 0; c < 3; ++c) {
        const uint32_t address = ctx->gpr[4 + c];
        if (address != Memory::Read32(textureSet + c * 4) || !RamRange(address, info.PlaneSize(c)))
            return false;
        planes[c] = {static_cast<uint8_t*>(Memory::GetPointer(address, info.PlaneSize(c))), info.PlaneSize(c)};
    }
    for (size_t c = 0; c < 3; ++c) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(planes[c].data);
        for (size_t d = c + 1; d < 3; ++d) {
            const uintptr_t other = reinterpret_cast<uintptr_t>(planes[d].data);
            if (begin < other + planes[d].size && other < begin + planes[c].size) return false;
        }
    }

    static thread_local ThpVideo::Decoder decoder;
    Worker& worker = GetWorker();
    const uint32_t planeAddress[3] = {ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]};
    if (!DecodeOnWorker(ctx, packet, info, planes)) {
        ++worker.local;
        EnsureMovieDrawn();
        ThpVideo::Status decoded = decoder.Decode(packet, planes);
        reason = decoded == ThpVideo::Status::DecodeError ? decoder.Error() : ThpVideo::StatusName(decoded);
        if (decoded != ThpVideo::Status::Success) return false;
    }
    const uint64_t frames = worker.parked + worker.waited + worker.local;
    if ((frames & 1023u) == 1u)
        std::fprintf(stderr, "[HOS THP] %llu frames: %llu decoded on a worker core with the video thread parked, "
                     "%llu waited for on the host, %llu decoded on the guest core\n",
                     static_cast<unsigned long long>(frames), static_cast<unsigned long long>(worker.parked),
                     static_cast<unsigned long long>(worker.waited), static_cast<unsigned long long>(worker.local));
    // Replaces the original LCStoreData DMA notifications. Notify only after
    // all three planes have decoded successfully, so cached textures refresh.
    for (size_t c = 0; c < 3; ++c)
        GxNotifyGuestRamDmaWrite(planeAddress[c], static_cast<uint32_t>(planes[c].size));
    ctx->gpr[3] = 0;
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr, "[HOS THP] native libjpeg-turbo decoder active: %ux%u, full-resolution YUV420, GX I8 tiles\n",
                     info.width, info.height);
    }
    return true;
}
} // namespace

// GNU --wrap redirects translated direct calls and registration pointers, while
// retaining the unchanged PPC implementation for unsupported frames/call sites.
extern "C" void __wrap_func_801B3BAC(CpuContext* ctx) {
    const char* reason = "native decoder exception";
    try {
        if (TryNativeDecode(ctx, reason)) return;
    } catch (const std::exception&) {
        reason = "native decoder allocation/memory failure";
    }
    static uint64_t fallbacks = 0;
    ++fallbacks;
    if (fallbacks <= 4)
        std::fprintf(stderr, "[HOS THP] PPC fallback #%llu: %s (lr=%08x)\n",
                     static_cast<unsigned long long>(fallbacks), reason, ctx->lr);
    EnsureMovieDrawn();
    __real_func_801B3BAC(ctx);
}

extern "C" int kartpad_gx_drawdone_skip(void) {
    const uint32_t thread = g_movieDrawThread.load(std::memory_order_relaxed);
    if (thread == 0 || thread != Memory::Read32(kRunningThread)) return 0;
    g_movieDrawOwed = true;
    g_movieDrawMark = GX_HLE_DrawDonesCompleted();
    return 1;
}

extern "C" void __wrap_func_805FBB60(CpuContext* ctx) {
    if (!MovieDrawSkipEnabled()) {
        __real_func_805FBB60(ctx);
        return;
    }
    struct Scope {
        uint32_t outer;
        explicit Scope(uint32_t thread) : outer(g_movieDrawThread.exchange(thread, std::memory_order_relaxed)) {}
        ~Scope() { g_movieDrawThread.store(outer, std::memory_order_relaxed); }
    } scope(Memory::Read32(kRunningThread));
    __real_func_805FBB60(ctx);
}
