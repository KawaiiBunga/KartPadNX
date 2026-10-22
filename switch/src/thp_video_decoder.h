#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ThpVideo {
constexpr size_t kMaxPacketSize = 4u * 1024u * 1024u;
constexpr uint32_t kMaxDimension = 1024;

enum class Status { Success, Unsupported, Malformed, OutputTooSmall, DecodeError };
const char* StatusName(Status status) noexcept;

struct FrameInfo {
    uint32_t width = 0, height = 0;
    size_t scanOffset = 0, entropyEnd = 0;
    size_t PlaneSize(size_t plane) const noexcept {
        return static_cast<size_t>(width) * height / (plane == 0 ? 1u : 4u);
    }
};

// Input is exactly one video component from a THP frame, including its EOI
// and optional zero padding. Entropy bytes are NOT JPEG byte-stuffed.
Status InspectFrame(std::span<const uint8_t> packet, FrameInfo& info) noexcept;

struct Plane { uint8_t* data = nullptr; size_t size = 0; };

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Produces full-resolution Y/Cb/Cr in GX I8 (8x4 tile) order. Buffers and
    // decoder state are reused. No output is modified unless decoding succeeds.
    Status Decode(std::span<const uint8_t> packet, const std::array<Plane, 3>& output);
    const char* Error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ThpVideo
