#pragma once
#include "memory_access.h"

namespace MemoryInline {
#define MKW_CAPTURED_MEMORY_INLINE inline __attribute__((always_inline))
// Capture only the flat ADDRESS-SPACE base, never an object pointer or page
// permission. On Switch ReserveFlatSpace publishes this base once and retains
// the reservation for the process lifetime. Mapping/protection changes keep
// the same virtual addresses and therefore retain the original fault policy.
// Guest calls can change their data and registers; reads still happen at their
// original instruction sites and use the original uint32_t guest addresses.
class CapturedGuestMemory {
    uint8_t* const base_ = MKW_FLAT_GUEST_BASE;
    template<typename T> MKW_CAPTURED_MEMORY_INLINE T Load(uint32_t address) const {
        T value{};
        std::memcpy(&value, base_ + address, sizeof(T));
        return MaybeByteSwap(value);
    }
    template<typename T> MKW_CAPTURED_MEMORY_INLINE void Store(uint32_t address, T value) const {
        const T swapped = MaybeByteSwap(value);
        std::memcpy(base_ + address, &swapped, sizeof(T));
    }
public:
    MKW_CAPTURED_MEMORY_INLINE uint8_t Read8(uint32_t address) const { return Load<uint8_t>(address); }
    MKW_CAPTURED_MEMORY_INLINE uint16_t Read16(uint32_t address) const { return Load<uint16_t>(address); }
    MKW_CAPTURED_MEMORY_INLINE uint32_t Read32(uint32_t address) const { return Load<uint32_t>(address); }
    MKW_CAPTURED_MEMORY_INLINE float ReadFloat32(uint32_t address) const {
        const uint32_t bits = Load<uint32_t>(address);
        float value; std::memcpy(&value, &bits, sizeof(value)); return value;
    }
    MKW_CAPTURED_MEMORY_INLINE double ReadFloat64(uint32_t address) const {
        const uint64_t bits = Load<uint64_t>(address);
        double value; std::memcpy(&value, &bits, sizeof(value)); return value;
    }
    MKW_CAPTURED_MEMORY_INLINE void WriteRam8(uint32_t address, uint8_t value) const { Store(address, value); }
    MKW_CAPTURED_MEMORY_INLINE void WriteRam16(uint32_t address, uint16_t value) const { Store(address, value); }
    MKW_CAPTURED_MEMORY_INLINE void WriteRam32(uint32_t address, uint32_t value) const { Store(address, value); }
    MKW_CAPTURED_MEMORY_INLINE void WriteRamFloat32(uint32_t address, double value) const {
        Store(address, ConvertPpcDoubleToSingleBits(value));
    }
    MKW_CAPTURED_MEMORY_INLINE void WriteRamFloat64(uint32_t address, double value) const {
        uint64_t bits; std::memcpy(&bits, &value, sizeof(bits)); Store(address, bits);
    }
    MKW_CAPTURED_MEMORY_INLINE void Write8(uint32_t address, uint8_t value) const {
        if (FlatWriteNeedsPolicy(address)) [[unlikely]] { Write8Slow(address, value); return; }
        WriteRam8(address, value);
    }
    MKW_CAPTURED_MEMORY_INLINE void Write16(uint32_t address, uint16_t value) const {
        if (FlatWriteNeedsPolicy(address)) [[unlikely]] { Write16Slow(address, value); return; }
        WriteRam16(address, value);
    }
    MKW_CAPTURED_MEMORY_INLINE void Write32(uint32_t address, uint32_t value) const {
        if (FlatWriteNeedsPolicy(address)) [[unlikely]] { Write32Slow(address, value); return; }
        WriteRam32(address, value);
    }
    MKW_CAPTURED_MEMORY_INLINE void WriteFloat32(uint32_t address, double value) const {
        const uint32_t bits = ConvertPpcDoubleToSingleBits(value);
        if (FlatWriteNeedsPolicy(address)) [[unlikely]] { WriteFloat32Slow(address, value); return; }
        Store(address, bits);
    }
    MKW_CAPTURED_MEMORY_INLINE void WriteFloat64(uint32_t address, double value) const {
        uint64_t bits; std::memcpy(&bits, &value, sizeof(bits));
        if (FlatWriteNeedsPolicy(address)) [[unlikely]] { WriteFloat64Slow(address, value); return; }
        Store(address, bits);
    }
};
#undef MKW_CAPTURED_MEMORY_INLINE
} // namespace MemoryInline
