// Host tool for test_kamek_loader.py: loads a Code.pul with dropin::LoadKamek into a fake guest
// memory (MEM1 + the 0x81800000 overlay) and prints the module image and every patch.
//   kamek_loader_dump Code.pul module.bin   -> module.bin = code+bss; stdout = "W addr size value" lines
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "dropin/kamek_loader.h"

static std::vector<uint8_t> g_mem(0x02000000);  // 0x80000000..0x82000000

static uint8_t* Guest(uint32_t address, uint32_t size) {
    const uint32_t a = address & 0x3FFFFFFFu;  // cached/uncached mirrors
    return a + size <= g_mem.size() ? g_mem.data() + a : nullptr;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<uint8_t> pul((std::istreambuf_iterator<char>(in)), {});
    const auto r = dropin::LoadKamek(pul.data(), pul.size(), Guest);
    if (!r.ok) {
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return 1;
    }
    std::ofstream(argv[2], std::ios::binary)
        .write(reinterpret_cast<const char*>(Guest(r.moduleStart, r.codeSize + r.bssSize)), r.codeSize + r.bssSize);
    std::printf("M %08X %X %X %08X %08X\n", r.moduleStart, r.codeSize, r.bssSize, r.ctorStart, r.ctorEnd);
    std::printf("S");
    for (uint8_t b : r.codeSha1) std::printf("%02x", b);
    std::printf("\n");
    for (const auto& w : r.writes) std::printf("W %08X %u %08X\n", w.address, w.size, w.value);
    for (uint32_t s : r.skipped) std::printf("K %08X\n", s);
    return 0;
}
