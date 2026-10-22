#include "thp_video_decoder.h"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace ThpVideo {
namespace {
uint16_t Be16(const uint8_t* p) noexcept { return (uint16_t(p[0]) << 8) | p[1]; }

struct JpegError {
    jpeg_error_mgr base{};
    std::jmp_buf jump;
    char text[JMSG_LENGTH_MAX]{};
};

void JpegFailure(j_common_ptr jpeg) {
    auto* error = reinterpret_cast<JpegError*>(jpeg->err);
    jpeg->err->format_message(jpeg, error->text);
    std::longjmp(error->jump, 1);
}

void JpegMessage(j_common_ptr jpeg, int level) {
    // libjpeg normally conceals truncated/corrupt input and prints warnings.
    // Reject it instead, leaving all guest output intact for the PPC fallback.
    if (level < 0) JpegFailure(jpeg);
}

void TileRows(uint8_t* destination, const uint8_t* rows, size_t width, size_t height) {
    for (size_t y = 0; y < height; y += 4) {
        const uint8_t* r0 = rows + y * width;
        const uint8_t* r1 = r0 + width;
        const uint8_t* r2 = r1 + width;
        const uint8_t* r3 = r2 + width;
        for (size_t x = 0; x < width; x += 8) {
            uint64_t* dst64 = reinterpret_cast<uint64_t*>(destination);
            uint64_t v0, v1, v2, v3;
            std::memcpy(&v0, r0 + x, 8);
            std::memcpy(&v1, r1 + x, 8);
            std::memcpy(&v2, r2 + x, 8);
            std::memcpy(&v3, r3 + x, 8);
            dst64[0] = v0;
            dst64[1] = v1;
            dst64[2] = v2;
            dst64[3] = v3;
            destination += 32;
        }
    }
}
} // namespace

const char* StatusName(Status status) noexcept {
    switch (status) {
    case Status::Success: return "success";
    case Status::Unsupported: return "unsupported THP format";
    case Status::Malformed: return "malformed THP frame";
    case Status::OutputTooSmall: return "invalid THP output buffer";
    case Status::DecodeError: return "JPEG decode error";
    }
    return "unknown THP status";
}

Status InspectFrame(std::span<const uint8_t> packet, FrameInfo& info) noexcept {
    info = {};
    if (packet.size() < 4 || packet.size() > kMaxPacketSize ||
        packet[0] != 0xff || packet[1] != 0xd8) return Status::Malformed;

    FrameInfo parsed;
    bool haveFrame = false;
    uint32_t quantTables = 0, dcTables = 0, acTables = 0;
    std::array<uint8_t, 3> quantIds{};
    size_t offset = 2;
    while (offset + 4 <= packet.size()) {
        if (packet[offset] != 0xff) return Status::Malformed;
        const uint8_t marker = packet[offset + 1];
        const size_t length = Be16(packet.data() + offset + 2);
        if (length < 2 || length > packet.size() - offset - 2) return Status::Malformed;
        const auto segment = packet.subspan(offset + 4, length - 2);
        offset += length + 2;

        switch (marker) {
        case 0xdb: { // 8-bit quantization tables, freshly supplied by each frame.
            size_t i = 0;
            while (i < segment.size()) {
                const uint8_t id = segment[i];
                if (id > 3) return Status::Unsupported;
                if (segment.size() - i < 65) return Status::Malformed;
                for (size_t j = i + 1; j < i + 65; ++j)
                    if (!segment[j]) return Status::Malformed;
                quantTables |= 1u << id;
                i += 65;
            }
            break;
        }
        case 0xc4: { // Huffman tables; libjpeg validates the actual code trees.
            size_t i = 0;
            while (i < segment.size()) {
                if (segment.size() - i < 17) return Status::Malformed;
                const uint8_t id = segment[i];
                if ((id & 0xec) != 0) return Status::Unsupported;
                size_t codes = 0;
                for (size_t j = i + 1; j < i + 17; ++j) codes += segment[j];
                if (!codes || codes > 256 || codes > segment.size() - i - 17)
                    return Status::Malformed;
                ((id & 0x10) ? acTables : dcTables) |= 1u << (id & 3);
                i += 17 + codes;
            }
            break;
        }
        case 0xc0: // Baseline 8-bit YCbCr 4:2:0, in complete 16x16 MCUs.
            if (haveFrame || segment.size() != 15) return Status::Malformed;
            if (segment[0] != 8 || segment[5] != 3) return Status::Unsupported;
            parsed.height = Be16(segment.data() + 1);
            parsed.width = Be16(segment.data() + 3);
            if (!parsed.width || !parsed.height || parsed.width > kMaxDimension ||
                parsed.height > kMaxDimension || (parsed.width & 15) || (parsed.height & 15))
                return Status::Unsupported;
            for (size_t i = 0; i < 3; ++i) {
                if (segment[6 + i * 3] != i + 1 ||
                    segment[7 + i * 3] != (i == 0 ? 0x22 : 0x11) || segment[8 + i * 3] > 3)
                    return Status::Unsupported;
                quantIds[i] = segment[8 + i * 3];
            }
            haveFrame = true;
            break;
        case 0xda: { // One interleaved scan; THP has no JPEG entropy escaping.
            if (!haveFrame || segment.size() != 10) return Status::Malformed;
            if (segment[0] != 3 || segment[7] != 0 || segment[8] != 63 || segment[9] != 0)
                return Status::Unsupported;
            for (size_t i = 0; i < 3; ++i) {
                const uint8_t tables = segment[2 + i * 2];
                if (segment[1 + i * 2] != i + 1 || (tables & 0xcc)) return Status::Unsupported;
                if (!(quantTables & (1u << quantIds[i])) ||
                    !(dcTables & (1u << (tables >> 4))) || !(acTables & (1u << (tables & 3))))
                    return Status::Malformed;
            }
            size_t end = packet.size();
            while (end > offset && packet[end - 1] == 0) --end;
            // Only the final EOI is a marker. An FF D9 inside THP entropy is
            // data and must be escaped too, never mistaken for the end.
            if (packet.size() - end > 31 || end < offset + 3 ||
                packet[end - 2] != 0xff || packet[end - 1] != 0xd9) return Status::Malformed;
            parsed.scanOffset = offset;
            parsed.entropyEnd = end - 2;
            info = parsed;
            return Status::Success;
        }
        default:
            // In particular, THP restart intervals do not use JPEG restart
            // markers. Preserve the original decoder for these and other modes.
            return Status::Unsupported;
        }
    }
    return Status::Malformed;
}

struct Decoder::Impl {
    jpeg_decompress_struct jpeg{};
    JpegError error;
    bool created = false;
    std::vector<uint8_t> jpegBytes, rowBytes;

    ~Impl() { if (created) jpeg_destroy_decompress(&jpeg); }

    Status DecodeJpeg(const FrameInfo& info, const std::array<Plane, 3>& output) {
        // All longjmp-sensitive state lives in this heap object. No C++ object
        // requiring destruction is introduced between setjmp and libjpeg calls.
        if (setjmp(error.jump)) {
            jpeg_destroy_decompress(&jpeg);
            created = false;
            return Status::DecodeError;
        }
        if (!created) {
            jpeg.err = jpeg_std_error(&error.base);
            error.base.error_exit = JpegFailure;
            error.base.emit_message = JpegMessage;
            created = true;
            jpeg_create_decompress(&jpeg);
        }
        error.text[0] = '\0';
        jpeg_mem_src(&jpeg, jpegBytes.data(), static_cast<unsigned long>(jpegBytes.size()));
        if (jpeg_read_header(&jpeg, TRUE) != JPEG_HEADER_OK || jpeg.progressive_mode ||
            jpeg.arith_code || jpeg.restart_interval || jpeg.data_precision != 8 ||
            jpeg.num_components != 3 || jpeg.jpeg_color_space != JCS_YCbCr ||
            jpeg.image_width != info.width || jpeg.image_height != info.height) {
            jpeg_abort_decompress(&jpeg);
            return Status::Unsupported;
        }
        for (int i = 0; i < 3; ++i) {
            if (jpeg.comp_info[i].h_samp_factor != (i == 0 ? 2 : 1) ||
                jpeg.comp_info[i].v_samp_factor != (i == 0 ? 2 : 1)) {
                jpeg_abort_decompress(&jpeg);
                return Status::Unsupported;
            }
        }
        jpeg.raw_data_out = TRUE;
        // Fast integer IDCT has ARM NEON SIMD implementation in devkitPro's libjpeg-turbo.
        jpeg.dct_method = JDCT_IFAST;
        jpeg.do_fancy_upsampling = FALSE;
        jpeg.do_block_smoothing = FALSE;
        if (!jpeg_start_decompress(&jpeg)) return Status::DecodeError;

        JSAMPROW rows[3][16];
        JSAMPARRAY planes[3] = {rows[0], rows[1], rows[2]};
        size_t base = 0;
        for (size_t c = 0; c < 3; ++c) {
            const size_t width = info.width / (c == 0 ? 1 : 2);
            const size_t height = c == 0 ? 16 : 8;
            for (size_t y = 0; y < height; ++y) rows[c][y] = rowBytes.data() + base + width * y;
            base += width * height;
        }
        while (jpeg.output_scanline < jpeg.output_height) {
            const size_t y = jpeg.output_scanline;
            if (jpeg_read_raw_data(&jpeg, planes, 16) != 16) {
                jpeg_abort_decompress(&jpeg);
                return Status::DecodeError;
            }
            for (size_t c = 0; c < 3; ++c) {
                const size_t scale = c == 0 ? 1 : 2;
                const size_t width = info.width / scale;
                TileRows(output[c].data + (y / scale) * width, rows[c][0], width, 16 / scale);
            }
        }
        if (!jpeg_finish_decompress(&jpeg)) return Status::DecodeError;
        return Status::Success;
    }
};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;
const char* Decoder::Error() const noexcept { return impl_->error.text; }

Status Decoder::Decode(std::span<const uint8_t> packet, const std::array<Plane, 3>& output) {
    FrameInfo info;
    const auto inspected = InspectFrame(packet, info);
    if (inspected != Status::Success) return inspected;
    for (size_t c = 0; c < 3; ++c)
        if (!output[c].data || output[c].size < info.PlaneSize(c)) return Status::OutputTooSmall;

    // Copy whole spans between FF bytes; avoid a branch per entropy byte.
    auto& bytes = impl_->jpegBytes;
    bytes.resize(packet.size() * 2);
    std::memcpy(bytes.data(), packet.data(), info.scanOffset);
    size_t source = info.scanOffset, destination = info.scanOffset;
    while (source < info.entropyEnd) {
        const auto* ff = static_cast<const uint8_t*>(
            std::memchr(packet.data() + source, 0xff, info.entropyEnd - source));
        const size_t count = ff ? static_cast<size_t>(ff - packet.data()) + 1 - source : info.entropyEnd - source;
        std::memcpy(bytes.data() + destination, packet.data() + source, count);
        source += count;
        destination += count;
        if (ff) bytes[destination++] = 0;
    }
    bytes[destination++] = 0xff;
    bytes[destination++] = 0xd9;
    bytes.resize(destination);
    impl_->rowBytes.resize(static_cast<size_t>(info.width) * 24);
    return impl_->DecodeJpeg(info, output);
}
} // namespace ThpVideo
