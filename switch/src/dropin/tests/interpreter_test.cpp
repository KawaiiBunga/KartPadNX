// Host test for dropin::Run. Builds with clang on x86-64 against the staged runtime headers, the
// same helpers translated code uses (see run-dropin-tests.sh):
//   1. hand-assembled programs per instruction group, checked against known results or against
//      the runtime helper the translator would emit for the same instruction;
//   2. with a main.dol argument: real game functions (memcpy, strlen, strcmp, the paired-single
//      PSMTXConcat / PSVECDotProduct / PSMTXMultVec) interpreted straight from the disc's code.
#include <sys/mman.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "dropin/ppc_interpreter.h"
#include "ppc_runtime.h"

using namespace MemoryInline;

// ---- runtime seams translated code also needs ------------------------------------------------
void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept {
    std::fprintf(stderr, "fatal: %.*s: %.*s\n", int(category.size()), category.data(), int(details.size()), details.data());
}

namespace {

int g_failures = 0;
void Expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

// ---- guest memory: the non-Switch flat base, 4 GiB reserved, committed on touch ----------------
void MapGuest() {
    void* want = MKW_FLAT_GUEST_BASE;
    void* got = mmap(want, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (got != want) {
        std::perror("mmap flat guest base");
        std::exit(2);
    }
}
void Put32(uint32_t a, uint32_t v) { FlatWriteRam32At(a, 0, v); }
uint32_t Get32(uint32_t a) { return FlatRead32At(a, 0); }

// ---- a few encoders ----------------------------------------------------------------------------
uint32_t D(uint32_t op, uint32_t d, uint32_t a, int32_t imm) { return op << 26 | d << 21 | a << 16 | (uint32_t(imm) & 0xFFFF); }
uint32_t X(uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t xo, bool rc = false) { return op << 26 | d << 21 | a << 16 | b << 11 | xo << 1 | rc; }
uint32_t A(uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t c, uint32_t xo) { return op << 26 | d << 21 | a << 16 | b << 11 | c << 6 | xo << 1; }
uint32_t addi(uint32_t d, uint32_t a, int32_t i) { return D(14, d, a, i); }
uint32_t li(uint32_t d, int32_t i) { return addi(d, 0, i); }
uint32_t lis(uint32_t d, int32_t i) { return D(15, d, 0, i); }
uint32_t ori(uint32_t a, uint32_t s, uint32_t i) { return D(24, s, a, int32_t(i)); }
uint32_t mr(uint32_t a, uint32_t s) { return X(31, s, a, s, 444); }
uint32_t add(uint32_t d, uint32_t a, uint32_t b) { return X(31, d, a, b, 266); }
uint32_t cmpwi(uint32_t a, int32_t i) { return D(11, 0, a, i); }
uint32_t b(int32_t off) { return 18u << 26 | (uint32_t(off) & 0x03FFFFFC); }
uint32_t bl(int32_t off) { return b(off) | 1; }
uint32_t bc(uint32_t bo, uint32_t bi, int32_t off) { return 16u << 26 | bo << 21 | bi << 16 | (uint32_t(off) & 0xFFFC); }
uint32_t blr() { return 0x4E800020u; }
uint32_t mtctr(uint32_t s) { return 0x7C0903A6u | s << 21; }
uint32_t mflr(uint32_t d) { return 0x7C0802A6u | d << 21; }
uint32_t mtlr(uint32_t s) { return 0x7C0803A6u | s << 21; }
uint32_t stw(uint32_t s, int32_t o, uint32_t a) { return D(36, s, a, o); }
uint32_t stwu(uint32_t s, int32_t o, uint32_t a) { return D(37, s, a, o); }
uint32_t lwz(uint32_t d, int32_t o, uint32_t a) { return D(32, d, a, o); }
uint32_t lfs(uint32_t d, int32_t o, uint32_t a) { return D(48, d, a, o); }
uint32_t stfs(uint32_t s, int32_t o, uint32_t a) { return D(52, s, a, o); }
uint32_t rlwinm(uint32_t a, uint32_t s, uint32_t sh, uint32_t mb, uint32_t me) { return 21u << 26 | s << 21 | a << 16 | sh << 11 | mb << 6 | me << 1; }

constexpr uint32_t kCode = 0x80300000u, kData = 0x80310000u, kStack = 0x80380000u;

std::unordered_map<uint32_t, dropin::InterpHooks::NativeFn> g_natives;
dropin::InterpHooks::NativeFn LookupNative(uint32_t a) {
    const auto it = g_natives.find(a);
    return it == g_natives.end() ? nullptr : it->second;
}
[[noreturn]] void Fatal(uint32_t pc, uint32_t ins, const char* why) {
    std::fprintf(stderr, "interpreter fatal at %08X (%08X): %s\n", pc, ins, why);
    std::exit(3);
}

CpuContext g_ctx;
dropin::InterpHooks Hooks() {
    dropin::InterpHooks h;
    h.native = LookupNative;
    h.fatal = Fatal;
    return h;
}

// Loads `code` at kCode and runs it as a function with a fresh stack.
dropin::RunStats RunCode(const std::vector<uint32_t>& code, dropin::State& state, uint32_t at = kCode) {
    for (size_t i = 0; i < code.size(); ++i) Put32(at + 4 * uint32_t(i), code[i]);
    g_ctx.gpr[1] = kStack;
    return dropin::Run(&g_ctx, at, Hooks(), state);
}
dropin::RunStats RunCode(const std::vector<uint32_t>& code) {
    dropin::State s;
    return RunCode(code, s);
}

// ---- 1. hand-assembled programs ----------------------------------------------------------------

void TestInteger() {
    // sum 1..100 with a counted loop
    RunCode({li(3, 0), li(4, 100), mtctr(4), add(3, 3, 4), addi(4, 4, -1), bc(16, 0, -8), blr()});
    Expect(g_ctx.gpr[3] == 5050, "bdnz loop sums 1..100");

    // 64-bit add: (0xFFFFFFFF:0xFFFFFFFF) + (0:1) via addc/adde
    g_ctx.gpr[3] = 0xFFFFFFFF; g_ctx.gpr[4] = 0xFFFFFFFF; g_ctx.gpr[5] = 0; g_ctx.gpr[6] = 1;
    RunCode({X(31, 4, 4, 6, 10), X(31, 3, 3, 5, 138), blr()});
    Expect(g_ctx.gpr[3] == 0 && g_ctx.gpr[4] == 0, "addc/adde carry through 64 bits");

    // subfic, srawi carry, rlwinm, rlwimi, cntlzw, extsb, mulhw, divw by zero
    g_ctx.gpr[3] = 0xFFFFFFF5;                       // -11
    RunCode({X(31, 3, 4, 2, 824), blr()});           // srawi r4, r3, 2
    Expect(g_ctx.gpr[4] == uint32_t(-3) && (g_ctx.xer & 0x20000000), "srawi rounds toward -inf and sets CA");
    g_ctx.gpr[3] = 0x12345678;
    RunCode({rlwinm(4, 3, 8, 24, 31), rlwinm(5, 3, 0, 28, 3), blr()});
    Expect(g_ctx.gpr[4] == 0x12 && g_ctx.gpr[5] == 0x10000008, "rlwinm extract and wrapped mask");
    g_ctx.gpr[3] = 0xAABBCCDD; g_ctx.gpr[4] = 0x11223344;
    RunCode({20u << 26 | 3 << 21 | 4 << 16 | 16 << 11 | 0 << 6 | 15 << 1, blr()});  // rlwimi r4, r3, 16, 0, 15
    Expect(g_ctx.gpr[4] == 0xCCDD3344, "rlwimi inserts under mask");
    g_ctx.gpr[3] = 0x00010000; g_ctx.gpr[5] = 0x80; g_ctx.gpr[6] = 0x80000000; g_ctx.gpr[7] = 3;
    RunCode({X(31, 3, 4, 0, 26), X(31, 5, 5, 0, 954), X(31, 8, 6, 7, 75), D(8, 9, 7, 10), blr()});
    Expect(g_ctx.gpr[4] == 15 && g_ctx.gpr[5] == 0xFFFFFF80 && g_ctx.gpr[8] == 0xFFFFFFFE && g_ctx.gpr[9] == 7,
           "cntlzw, extsb, mulhw, subfic");
    g_ctx.gpr[3] = 7; g_ctx.gpr[4] = 0;
    RunCode({X(31, 5, 3, 4, 491), X(31, 6, 3, 4, 459), blr()});
    Expect(g_ctx.gpr[5] == 0 && g_ctx.gpr[6] == 0, "divw/divwu by zero give the runtime's 0");

    // compare + conditional branch + record form
    g_ctx.gpr[3] = uint32_t(-5);
    RunCode({cmpwi(3, 0), bc(12, 0, 12), li(4, 1), blr(), li(4, 2), blr()});
    Expect(g_ctx.gpr[4] == 2, "cmpwi + blt taken for a negative value");
    g_ctx.gpr[3] = 0;
    RunCode({X(31, 3, 4, 3, 28, true), bc(12, 2, 12), li(5, 1), blr(), li(5, 2), blr()});   // and. r4,r3,r3 ; beq
    Expect(g_ctx.gpr[5] == 2, "and. sets CR0[EQ]");
}

void TestCallsAndMemory() {
    // fact(n) recursive: standard prologue/epilogue, bl, lmw/stmw-free
    const std::vector<uint32_t> fact = {
        cmpwi(3, 1), bc(12, 1, 12), li(3, 1), blr(),             // if (n <= 1) return 1  (ble: bc 4,1 -> use bgt skip)
        stwu(1, -16, 1), mflr(0), stw(0, 20, 1), stw(31, 12, 1),
        mr(31, 3), addi(3, 3, -1), bl(-40),                      // fact(n - 1)
        X(31, 3, 3, 31, 235),                                    // mullw r3, r3, r31
        lwz(31, 12, 1), lwz(0, 20, 1), mtlr(0), addi(1, 1, 16), blr()};
    // bc(12,1,12) = branch if GT to the prologue; fall through returns 1.
    g_ctx.gpr[3] = 10;
    const auto stats = RunCode(fact);
    Expect(g_ctx.gpr[3] == 3628800 && g_ctx.gpr[1] == kStack, "recursive fact(10) with stack frames: " + std::to_string(stats.instructions) + " instructions");

    // stmw / lmw round trip
    for (uint32_t r = 26; r < 32; ++r) g_ctx.gpr[r] = 0x1000 + r;
    g_ctx.gpr[3] = kData;
    RunCode({D(47, 26, 3, 0), li(26, 0), li(31, 0), D(46, 26, 3, 0), blr()});
    Expect(g_ctx.gpr[26] == 0x101A && g_ctx.gpr[31] == 0x101F && Get32(kData + 20) == 0x101F, "stmw/lmw");

    // byte/half loads with sign extension, update form, byte-reversed
    Put32(kData, 0x80FF1234);
    g_ctx.gpr[3] = kData;
    RunCode({D(42, 4, 3, 0), D(34, 5, 3, 1), X(31, 6, 0, 3, 534), D(33, 7, 3, 0), blr()});
    Expect(g_ctx.gpr[4] == 0xFFFF80FF && g_ctx.gpr[5] == 0xFF && g_ctx.gpr[6] == 0x3412FF80 && g_ctx.gpr[3] == kData,
           "lha sign-extends, lbz, lwbrx, lwzu");
}

void TestNatives() {
    // bl to a native function, and a tail call (b) into one.
    g_natives[0x80400000u] = [](CpuContext* c) { c->gpr[3] *= 2; c->lr = 0xDEADBEEF; };
    const uint32_t here = kCode;
    g_ctx.gpr[3] = 21;
    RunCode({stwu(1, -16, 1), mflr(0), stw(0, 20, 1),
             bl(int32_t(0x80400000u - (here + 12))), addi(3, 3, 1),
             lwz(0, 20, 1), mtlr(0), addi(1, 1, 16), blr()});
    Expect(g_ctx.gpr[3] == 43, "bl into translated code and back");
    g_ctx.gpr[3] = 5;
    const auto s = RunCode({b(int32_t(0x80400000u - here))});
    Expect(g_ctx.gpr[3] == 10 && s.nativeCalls == 1, "tail call into translated code returns to our caller");
}

void TestFloat() {
    // Every scalar op against the helper the translator emits.
    Put32(kData + 0, 0x3FC00000);   // 1.5f
    Put32(kData + 4, 0x40400000);   // 3.0f
    Put32(kData + 8, 0x3DCCCCCD);   // 0.1f
    g_ctx.gpr[3] = kData;
    RunCode({lfs(1, 0, 3), lfs(2, 4, 3), lfs(3, 8, 3),
             A(59, 4, 1, 2, 0, 21), A(59, 5, 1, 0, 3, 25), A(59, 6, 1, 2, 3, 29), A(59, 7, 2, 1, 0, 18),
             A(63, 8, 1, 2, 3, 29), X(63, 9, 0, 3, 12), stfs(6, 12, 3), blr()});
    auto f = [](int i) { return g_ctx.fpr[i].d; };
    double e = 0;
    PpcFaddsStateInline(e, 1.5, 3.0);
    bool ok = f(4) == e;
    PpcFmulsStateInline(e, 1.5, double(0.1f));
    ok &= f(5) == e;
    PpcFmaddsStateInline(e, 1.5, double(0.1f), 3.0);
    ok &= f(6) == e;
    PpcFdivsStateInline(e, 3.0, 1.5);
    ok &= f(7) == e;
    PpcFmaddStateInline(e, 1.5, double(0.1f), 3.0);
    ok &= f(8) == e && f(9) == double(float(0.1f));
    Expect(ok, "fadds/fmuls/fmadds/fdivs/fmadd/frsp bit-identical to the translator's helpers");
    Expect(Get32(kData + 12) == PpcBitCastToU32Inline(float(f(6))), "stfs stores the single");

    // fctiwz + stfiwx, fcmpu, fsel, fneg/fabs
    g_ctx.fpr[1].d = -7.9; g_ctx.fpr[2].d = 2.0;
    RunCode({X(63, 3, 0, 1, 15), X(63, 0, 1, 2, 0), A(63, 5, 1, 2, 3, 23),
             X(63, 6, 0, 1, 40), X(63, 7, 0, 1, 264), blr()});
    g_ctx.gpr[4] = kData;
    RunCode({X(31, 3, 0, 4, 983), blr()});
    Expect(Get32(kData) == uint32_t(-7), "fctiwz truncates; stfiwx stores the word");
    Expect(((g_ctx.cr >> 28) & 0xF) == 8, "fcmpu sets CR0[LT]");
    Expect(f(5) == 2.0 && f(6) == 7.9 && f(7) == 7.9, "fsel picks frB for a negative control; fneg; fabs");
}

void TestPaired() {
    // psq_l with u8 quantization (GQR2: load type 4 = u8, scale 2 -> /4), ps ops, psq_st float.
    FlatWriteRam8At(kData, 0, 8);
    FlatWriteRam8At(kData, 1, 20);
    g_ctx.gqr[2] = (4u << 16) | (2u << 24);
    g_ctx.gqr[0] = 0;
    g_ctx.gpr[3] = kData;
    g_ctx.fpr[5].d = PpcPackPairedInline(10.f, 100.f);
    dropin::State st;
    st.paired = 1u << 5;
    RunCode({56u << 26 | 1 << 21 | 3 << 16 | 0 << 15 | 2 << 12 | 0,       // psq_l f1, 0(r3), 0, qr2 -> (2, 5)
             A(4, 2, 1, 0, 1, 25),                                         // ps_mul f2 = f1*f1 -> (4, 25)
             A(4, 3, 1, 5, 2, 29),                                         // ps_madd f3 = f1*f2+f5 -> (18, 225)
             A(4, 4, 3, 1, 2, 10),                                         // ps_sum0 f4 = (f3.ps0+f1.ps1, f2.ps1) -> (23, 25)
             X(4, 6, 3, 1, 560),                                           // ps_merge01 f6 = (f3.ps0, f1.ps1) -> (18, 5)
             60u << 26 | 4 << 21 | 3 << 16 | 0 << 15 | 0 << 12 | 8,       // psq_st f4, 8(r3), 0, qr0
             blr()}, st);
    auto ps = [](int i) { return std::pair<float, float>(PpcGetPs0Inline(g_ctx.fpr[i].d), PpcGetPs1Inline(g_ctx.fpr[i].d)); };
    Expect(ps(1) == std::pair<float, float>(2.f, 5.f), "psq_l dequantizes u8 with scale 2");
    Expect(ps(3) == std::pair<float, float>(18.f, 225.f) && ps(4) == std::pair<float, float>(23.f, 25.f) &&
               ps(6) == std::pair<float, float>(18.f, 5.f),
           "ps_mul, ps_madd, ps_sum0, ps_merge01");
    Expect(Get32(kData + 8) == PpcBitCastToU32Inline(23.f) && Get32(kData + 12) == PpcBitCastToU32Inline(25.f), "psq_st stores both lanes");
    Expect(st.paired == ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6)), "paired-representation tags follow the writes");

    // A scalar used by a paired op is replicated (lfs then ps_add), like the translator's PsFromScalar.
    Put32(kData, 0x40000000);   // 2.0f
    dropin::State st2;
    RunCode({lfs(1, 0, 3), A(4, 2, 1, 1, 0, 21), blr()}, st2);
    Expect(ps(2) == std::pair<float, float>(4.f, 4.f), "scalar operand to a paired op is replicated");
}

// ---- 2. real game code from main.dol -----------------------------------------------------------

bool LoadDol(const char* path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> dol((std::istreambuf_iterator<char>(in)), {});
    if (dol.size() < 0x100) return false;
    auto be = [&](size_t o) { return uint32_t(dol[o]) << 24 | dol[o + 1] << 16 | dol[o + 2] << 8 | dol[o + 3]; };
    for (int i = 0; i < 18; ++i) {
        const uint32_t off = be(4 * i), addr = be(0x48 + 4 * i), size = be(0x90 + 4 * i);
        if (!size) continue;
        std::memcpy(MKW_FLAT_GUEST_BASE + addr, dol.data() + off, size);
    }
    return true;
}

dropin::RunStats Call(uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t r = 3;
    for (uint32_t a : args) g_ctx.gpr[r++] = a;
    g_ctx.gpr[1] = kStack;
    dropin::State s;
    return dropin::Run(&g_ctx, fn, Hooks(), s);
}

void PutFloats(uint32_t a, const float* v, int n) { for (int i = 0; i < n; ++i) Put32(a + 4 * i, PpcBitCastToU32Inline(v[i])); }
float GetFloat(uint32_t a) { return PpcBitCastToFloatInline(Get32(a)); }

void TestGameCode(const char* dolPath) {
    if (!LoadDol(dolPath)) {
        Expect(false, std::string("could not read ") + dolPath);
        return;
    }
    const char* text = "Retro Rewind, interpreted";
    for (size_t i = 0; i <= std::strlen(text); ++i) FlatWriteRam8At(kData, int32_t(i), uint8_t(text[i]));
    auto s = Call(0x80021254u, {kData});
    Expect(g_ctx.gpr[3] == std::strlen(text), "game strlen: " + std::to_string(s.instructions) + " instructions");
    s = Call(0x80005F34u, {kData + 0x100, kData, uint32_t(std::strlen(text) + 1)});
    bool same = true;
    for (size_t i = 0; i <= std::strlen(text); ++i) same &= FlatRead8At(kData + 0x100, int32_t(i)) == uint8_t(text[i]);
    Expect(same && g_ctx.gpr[3] == kData + 0x100, "game memcpy");
    Call(0x8001329Cu, {kData, kData + 0x100});
    const uint32_t eq = g_ctx.gpr[3];
    FlatWriteRam8At(kData + 0x100, 0, 'S');
    Call(0x8001329Cu, {kData, kData + 0x100});
    Expect(eq == 0 && int32_t(g_ctx.gpr[3]) < 0, "game strcmp");

    // PSMTXConcat(a, b, ab): 3x4 matrices with paired singles.
    const float ma[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const float mb[12] = {0.5f, -1, 2, 0.25f, 3, 0.125f, -2, 1, 1.5f, 4, -0.5f, 2};
    PutFloats(kData, ma, 12);
    PutFloats(kData + 0x40, mb, 12);
    s = Call(0x80199D64u, {kData, kData + 0x40, kData + 0x80});
    double worst = 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
            double want = (c == 3 ? ma[r * 4 + 3] : 0.0);
            for (int k = 0; k < 3; ++k) want += double(ma[r * 4 + k]) * mb[k * 4 + c];
            worst = std::fmax(worst, std::fabs(want - GetFloat(kData + 0x80 + 4 * (r * 4 + c))));
        }
    Expect(worst < 1e-4, "game PSMTXConcat (paired singles) matches a 3x4 product: " + std::to_string(s.instructions) + " instructions");

    // PSVECDotProduct(a, b) -> f1
    const float va[3] = {1.5f, -2, 4}, vb[3] = {2, 0.5f, 0.25f};
    PutFloats(kData, va, 3);
    PutFloats(kData + 0x10, vb, 3);
    Call(0x8019ACACu, {kData, kData + 0x10});
    Expect(std::fabs(g_ctx.fpr[1].d - 3.0) < 1e-6 || std::fabs(PpcGetPs0Inline(g_ctx.fpr[1].d) - 3.0f) < 1e-6,
           "game PSVECDotProduct");

    // PSMTXMultVec(m, src, dst)
    const float v[3] = {1, 2, 3};
    PutFloats(kData, ma, 12);
    PutFloats(kData + 0x40, v, 3);
    Call(0x8019A91Cu, {kData, kData + 0x40, kData + 0x50});
    Expect(GetFloat(kData + 0x50) == 18 && GetFloat(kData + 0x54) == 46 && GetFloat(kData + 0x58) == 74, "game PSMTXMultVec");
}

}  // namespace

int main(int argc, char** argv) {
    MapGuest();
    CpuContextScope scope(&g_ctx);
    TestInteger();
    TestCallsAndMemory();
    TestNatives();
    TestFloat();
    TestPaired();
    if (argc > 1) TestGameCode(argv[1]);
    std::printf(g_failures ? "FAILED (%d)\n" : "All interpreter tests passed.\n", g_failures);
    return g_failures ? 1 : 0;
}
