#include "dropin/ppc_interpreter.h"

#include <cmath>
#include <cstring>

#include "ppc_runtime.h"

// Runtime FPSCR helpers (runtime/src/fpu_helpers.cpp), shared with translated code.
extern "C" void PPC_Mtfsf(uint32_t fieldMask, double source);
extern "C" void PPC_Mtfsb1(uint32_t bit);
extern "C" void PPC_Mtfsb0(uint32_t bit);
extern "C" void PPC_Mtfsfi(uint32_t field, uint32_t value);
extern "C" double PPC_Mffs();
extern "C" double PPC_Fctiw(double value);

namespace dropin {
namespace {

using namespace MemoryInline;

constexpr uint32_t kXerSo = 0x80000000u, kXerOv = 0x40000000u, kXerCa = 0x20000000u;

inline int32_t Simm(uint32_t i) { return static_cast<int16_t>(i & 0xFFFF); }
inline uint32_t Mask(uint32_t mb, uint32_t me) {
    const uint32_t begin = 0xFFFFFFFFu >> mb;
    const uint32_t end = me >= 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (begin & end) : (begin | end);
}
inline uint32_t Bswap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t Bswap16(uint16_t v) { return __builtin_bswap16(v); }

template <uint32_t W, uint32_t I>
double PsqLoad(uint32_t gqr, uint32_t addr) { return PPC_PsqLGqrInline<W, I>(gqr, addr); }
template <uint32_t W, uint32_t I>
void PsqStore(uint32_t gqr, uint32_t addr, double v) { PPC_PsqStGqrInline<W, I>(gqr, addr, v); }

using PsqLoadFn = double (*)(uint32_t, uint32_t);
using PsqStoreFn = void (*)(uint32_t, uint32_t, double);
constexpr PsqLoadFn kPsqLoad[2][8] = {
    {PsqLoad<0, 0>, PsqLoad<0, 1>, PsqLoad<0, 2>, PsqLoad<0, 3>, PsqLoad<0, 4>, PsqLoad<0, 5>, PsqLoad<0, 6>, PsqLoad<0, 7>},
    {PsqLoad<1, 0>, PsqLoad<1, 1>, PsqLoad<1, 2>, PsqLoad<1, 3>, PsqLoad<1, 4>, PsqLoad<1, 5>, PsqLoad<1, 6>, PsqLoad<1, 7>},
};
constexpr PsqStoreFn kPsqStore[2][8] = {
    {PsqStore<0, 0>, PsqStore<0, 1>, PsqStore<0, 2>, PsqStore<0, 3>, PsqStore<0, 4>, PsqStore<0, 5>, PsqStore<0, 6>, PsqStore<0, 7>},
    {PsqStore<1, 0>, PsqStore<1, 1>, PsqStore<1, 2>, PsqStore<1, 3>, PsqStore<1, 4>, PsqStore<1, 5>, PsqStore<1, 6>, PsqStore<1, 7>},
};

class Interp {
public:
    Interp(CpuContext* ctx, const InterpHooks& hooks, State& state) : c_(*ctx), h_(hooks), s_(state) {}

    RunStats Run(uint32_t entry) {
        const uint32_t callerLr = c_.lr;
        c_.lr = kReturnSentinel;
        pc_ = entry;
        while (!done_) {
            const uint32_t ins = FlatRead32At(pc_, 0);
            ++stats_.instructions;
            next_ = pc_ + 4;
            Execute(ins);
            pc_ = next_;
        }
        c_.lr = callerLr;
        c_.pc = pc_;
        return stats_;
    }

private:
    CpuContext& c_;
    const InterpHooks& h_;
    State& s_;
    RunStats stats_;
    uint32_t pc_ = 0, next_ = 0;
    bool done_ = false;

    // ---- registers ----
    uint32_t& R(uint32_t i) { return c_.gpr[i]; }
    uint32_t RA0(uint32_t i) { return i ? c_.gpr[i] : 0; }   // rA|0
    bool Paired(uint32_t i) const { return (s_.paired >> i) & 1; }
    double Scalar(uint32_t i) { return Paired(i) ? PPC_PsToScalarInline(c_.fpr[i].d) : c_.fpr[i].d; }
    double Pair(uint32_t i) { return Paired(i) ? c_.fpr[i].d : PPC_PsFromScalarInline(c_.fpr[i].d); }
    void SetScalar(uint32_t i, double v) { c_.fpr[i].d = v; s_.paired &= ~(1u << i); }
    void SetPair(uint32_t i, double v) { c_.fpr[i].d = v; s_.paired |= 1u << i; }

    bool Ca() const { return c_.xer & kXerCa; }
    void SetCa(bool on) { c_.xer = on ? (c_.xer | kXerCa) : (c_.xer & ~kXerCa); }
    void SetOv(bool on) { c_.xer = on ? (c_.xer | kXerOv | kXerSo) : (c_.xer & ~kXerOv); }
    void SetCrField(uint32_t f, uint32_t v) {
        const uint32_t shift = (7 - f) * 4;
        c_.cr = (c_.cr & ~(0xFu << shift)) | ((v & 0xF) << shift);
    }
    void Cr0(uint32_t result) {
        const int32_t r = static_cast<int32_t>(result);
        SetCrField(0, (r < 0 ? 8u : r > 0 ? 4u : 2u) | (c_.xer >> 31));
    }
    void Cr1() { SetCrField(1, c_.fpscr >> 28); }
    bool CrBit(uint32_t b) const { return (c_.cr >> (31 - b)) & 1; }
    void SetCrBit(uint32_t b, bool v) { c_.cr = v ? (c_.cr | (1u << (31 - b))) : (c_.cr & ~(1u << (31 - b))); }

    [[noreturn]] void Fatal(uint32_t ins, const char* why) {
        c_.pc = pc_;
        if (h_.fatal) h_.fatal(pc_, ins, why);
        PPC_Undefined(pc_, ins, why);
    }

    // ---- control flow ----
    void CallNative(InterpHooks::NativeFn fn, uint32_t target) {
        ++stats_.nativeCalls;
        c_.pc = target;
        if (h_.beforeNative) h_.beforeNative(target, &c_, s_);
        fn(&c_);
        if (h_.afterNative) h_.afterNative(target, &c_, s_);
        else s_.paired &= ~2u;  // f1 comes back as the scalar return
    }

    // A taken branch. `link` is bl/bcl/blrl/bcctrl: LR = pc+4 (already set by the caller).
    void Branch(uint32_t target, bool link) {
        if (target == kReturnSentinel) {
            done_ = true;
            return;
        }
        InterpHooks::NativeFn fn = h_.native ? h_.native(target) : nullptr;
        if (!fn) {
            next_ = target;
            return;
        }
        const uint32_t returnTo = c_.lr;  // translated code need not preserve ctx->lr
        CallNative(fn, target);
        if (link) return;  // a call: carry on after it
        // A tail call into translated code: it returned to our caller.
        c_.lr = returnTo;
        Branch(returnTo & ~3u, false);
    }

    bool CondOk(uint32_t bo, uint32_t bi, bool decrementCtr) {
        if (decrementCtr && !(bo & 4)) --c_.ctr;
        const bool ctrOk = (bo & 4) || ((c_.ctr != 0) != ((bo >> 1) & 1));
        const bool condOk = (bo & 16) || (CrBit(bi) == ((bo >> 3) & 1));
        return ctrOk && condOk;
    }

    // ---- integer helpers ----
    uint32_t AddCarry(uint32_t a, uint32_t b, uint32_t carryIn, bool setCa) {
        const uint64_t wide = uint64_t(a) + b + carryIn;
        if (setCa) SetCa(wide >> 32);
        return static_cast<uint32_t>(wide);
    }
    void Overflow(uint32_t a, uint32_t b, uint32_t r) {  // signed overflow of r = a + b
        SetOv(((a ^ r) & (b ^ r)) >> 31);
    }

    void Execute(uint32_t ins);
    void Op4(uint32_t ins);
    void Op19(uint32_t ins);
    void Op31(uint32_t ins);
    void Op59(uint32_t ins);
    void Op63(uint32_t ins);
    void LoadStoreMultipleString(uint32_t ins, uint32_t xo);
};

void Interp::Execute(uint32_t ins) {
    const uint32_t op = ins >> 26, d = (ins >> 21) & 31, a = (ins >> 16) & 31;
    const uint32_t uimm = ins & 0xFFFF;
    const int32_t simm = Simm(ins);
    switch (op) {
        case 3:  // twi
            if (((ins >> 21) & 31) && [&] {
                    const int32_t x = int32_t(R(a)), y = simm;
                    const uint32_t to = d;
                    return ((to & 16) && x < y) || ((to & 8) && x > y) || ((to & 4) && x == y) ||
                           ((to & 2) && uint32_t(x) < uint32_t(y)) || ((to & 1) && uint32_t(x) > uint32_t(y));
                }())
                Fatal(ins, "trap (twi)");
            return;
        case 4: return Op4(ins);
        case 7: R(d) = uint32_t(int32_t(R(a)) * simm); return;                          // mulli
        case 8: R(d) = AddCarry(~R(a), uint32_t(simm), 1, true); return;                // subfic
        case 10: {                                                                       // cmpli
            const uint32_t x = R(a), y = uimm;
            SetCrField(d >> 2, (x < y ? 8u : x > y ? 4u : 2u) | (c_.xer >> 31));
            return;
        }
        case 11: {                                                                       // cmpi
            const int32_t x = int32_t(R(a)), y = simm;
            SetCrField(d >> 2, (x < y ? 8u : x > y ? 4u : 2u) | (c_.xer >> 31));
            return;
        }
        case 12: R(d) = AddCarry(R(a), uint32_t(simm), 0, true); return;                 // addic
        case 13: R(d) = AddCarry(R(a), uint32_t(simm), 0, true); Cr0(R(d)); return;      // addic.
        case 14: R(d) = RA0(a) + uint32_t(simm); return;                                 // addi
        case 15: R(d) = RA0(a) + (uimm << 16); return;                                   // addis
        case 16: {                                                                       // bc
            const uint32_t bo = d, bi = a;
            const int32_t bd = int32_t(int16_t(ins & 0xFFFC));
            if (!CondOk(bo, bi, true)) return;
            const uint32_t target = (ins & 2) ? uint32_t(bd) : pc_ + uint32_t(bd);
            if (ins & 1) c_.lr = pc_ + 4;
            return Branch(target, ins & 1);
        }
        case 17:                                                                         // sc
            if (!h_.systemCall) Fatal(ins, "system call");
            h_.systemCall(&c_);
            return;
        case 18: {                                                                       // b
            int32_t li = int32_t(ins & 0x03FFFFFC);
            if (li & 0x02000000) li |= ~0x03FFFFFF;
            const uint32_t target = (ins & 2) ? uint32_t(li) : pc_ + uint32_t(li);
            if (ins & 1) c_.lr = pc_ + 4;
            return Branch(target, ins & 1);
        }
        case 19: return Op19(ins);
        case 20: {                                                                       // rlwimi
            const uint32_t sh = (ins >> 11) & 31, mb = (ins >> 6) & 31, me = (ins >> 1) & 31;
            const uint32_t m = Mask(mb, me);
            R(a) = (PpcRotl32Inline(R(d), sh) & m) | (R(a) & ~m);
            if (ins & 1) Cr0(R(a));
            return;
        }
        case 21: {                                                                       // rlwinm
            const uint32_t sh = (ins >> 11) & 31, mb = (ins >> 6) & 31, me = (ins >> 1) & 31;
            R(a) = PpcRotl32Inline(R(d), sh) & Mask(mb, me);
            if (ins & 1) Cr0(R(a));
            return;
        }
        case 23: {                                                                       // rlwnm
            const uint32_t b = (ins >> 11) & 31, mb = (ins >> 6) & 31, me = (ins >> 1) & 31;
            R(a) = PpcRotl32Inline(R(d), R(b) & 31) & Mask(mb, me);
            if (ins & 1) Cr0(R(a));
            return;
        }
        case 24: R(a) = R(d) | uimm; return;                                             // ori
        case 25: R(a) = R(d) | (uimm << 16); return;                                     // oris
        case 26: R(a) = R(d) ^ uimm; return;                                             // xori
        case 27: R(a) = R(d) ^ (uimm << 16); return;                                     // xoris
        case 28: R(a) = R(d) & uimm; Cr0(R(a)); return;                                  // andi.
        case 29: R(a) = R(d) & (uimm << 16); Cr0(R(a)); return;                          // andis.
        case 31: return Op31(ins);
        // D-form loads and stores. Update forms write the address back to rA.
        case 32: case 33: { const uint32_t ea = RA0(a) + simm; R(d) = FlatRead32At(ea, 0); if (op & 1) R(a) = ea; return; }
        case 34: case 35: { const uint32_t ea = RA0(a) + simm; R(d) = FlatRead8At(ea, 0); if (op & 1) R(a) = ea; return; }
        case 36: case 37: { const uint32_t ea = RA0(a) + simm; FlatWrite32At(ea, 0, R(d)); if (op & 1) R(a) = ea; return; }
        case 38: case 39: { const uint32_t ea = RA0(a) + simm; FlatWrite8At(ea, 0, uint8_t(R(d))); if (op & 1) R(a) = ea; return; }
        case 40: case 41: { const uint32_t ea = RA0(a) + simm; R(d) = FlatRead16At(ea, 0); if (op & 1) R(a) = ea; return; }
        case 42: case 43: { const uint32_t ea = RA0(a) + simm; R(d) = uint32_t(int32_t(int16_t(FlatRead16At(ea, 0)))); if (op & 1) R(a) = ea; return; }
        case 44: case 45: { const uint32_t ea = RA0(a) + simm; FlatWrite16At(ea, 0, uint16_t(R(d))); if (op & 1) R(a) = ea; return; }
        case 46: { uint32_t ea = RA0(a) + simm; for (uint32_t r = d; r < 32; ++r, ea += 4) R(r) = FlatRead32At(ea, 0); return; }   // lmw
        case 47: { uint32_t ea = RA0(a) + simm; for (uint32_t r = d; r < 32; ++r, ea += 4) FlatWrite32At(ea, 0, R(r)); return; }  // stmw
        case 48: case 49: { const uint32_t ea = RA0(a) + simm; SetScalar(d, double(FlatReadFloat32At(ea, 0))); if (op & 1) R(a) = ea; return; }
        case 50: case 51: { const uint32_t ea = RA0(a) + simm; SetScalar(d, FlatReadFloat64At(ea, 0)); if (op & 1) R(a) = ea; return; }
        case 52: case 53: { const uint32_t ea = RA0(a) + simm; FlatWriteFloat32At(ea, 0, Scalar(d)); if (op & 1) R(a) = ea; return; }
        case 54: case 55: { const uint32_t ea = RA0(a) + simm; FlatWriteFloat64At(ea, 0, Scalar(d)); if (op & 1) R(a) = ea; return; }
        case 56: case 57: case 60: case 61: {                                            // psq_l(u), psq_st(u)
            int32_t off = int32_t(ins & 0xFFF);
            if (off & 0x800) off |= ~0xFFF;
            const uint32_t w = (ins >> 15) & 1, i = (ins >> 12) & 7;
            const uint32_t ea = ((op & 1) ? R(a) : RA0(a)) + uint32_t(off);
            if (op < 60) SetPair(d, kPsqLoad[w][i](c_.gqr[i], ea));
            else kPsqStore[w][i](c_.gqr[i], ea, Pair(d));
            if (op & 1) R(a) = ea;
            return;
        }
        case 59: return Op59(ins);
        case 63: return Op63(ins);
        default: Fatal(ins, "unknown primary opcode");
    }
}

void Interp::Op19(uint32_t ins) {
    const uint32_t d = (ins >> 21) & 31, a = (ins >> 16) & 31, b = (ins >> 11) & 31;
    switch ((ins >> 1) & 0x3FF) {
        case 0: SetCrField(d >> 2, (c_.cr >> ((7 - (a >> 2)) * 4)) & 0xF); return;      // mcrf
        case 16: {                                                                       // bclr
            if (!CondOk(d, a, true)) return;
            const uint32_t target = c_.lr & ~3u;
            if (ins & 1) c_.lr = pc_ + 4;
            return Branch(target, ins & 1);
        }
        case 528: {                                                                      // bcctr
            if (!CondOk(d, a, false)) return;
            const uint32_t target = c_.ctr & ~3u;
            if (ins & 1) c_.lr = pc_ + 4;
            return Branch(target, ins & 1);
        }
        case 33: SetCrBit(d, !(CrBit(a) || CrBit(b))); return;                           // crnor
        case 129: SetCrBit(d, CrBit(a) && !CrBit(b)); return;                            // crandc
        case 193: SetCrBit(d, CrBit(a) != CrBit(b)); return;                             // crxor
        case 225: SetCrBit(d, !(CrBit(a) && CrBit(b))); return;                          // crnand
        case 257: SetCrBit(d, CrBit(a) && CrBit(b)); return;                             // crand
        case 289: SetCrBit(d, CrBit(a) == CrBit(b)); return;                             // creqv
        case 417: SetCrBit(d, CrBit(a) || !CrBit(b)); return;                            // crorc
        case 449: SetCrBit(d, CrBit(a) || CrBit(b)); return;                             // cror
        case 150: return;                                                                // isync
        default: Fatal(ins, "unknown opcode 19 form");
    }
}

void Interp::Op31(uint32_t ins) {
    const uint32_t d = (ins >> 21) & 31, a = (ins >> 16) & 31, b = (ins >> 11) & 31;
    const bool rc = ins & 1, oe = (ins >> 10) & 1;
    const uint32_t xo = (ins >> 1) & 0x3FF;
    const uint32_t ea = RA0(a) + R(b);
    // XO-form arithmetic (9-bit XO, bit 21 = OE).
    switch (xo & 0x1FF) {
        case 266: { const uint32_t x = R(a), y = R(b), r = x + y; if (oe) Overflow(x, y, r); R(d) = r; if (rc) Cr0(r); return; }       // add
        case 10: { const uint32_t x = R(a), y = R(b); R(d) = AddCarry(x, y, 0, true); if (oe) Overflow(x, y, R(d)); if (rc) Cr0(R(d)); return; }   // addc
        case 138: { const uint32_t x = R(a), y = R(b); R(d) = AddCarry(x, y, Ca(), true); if (oe) Overflow(x, y, R(d)); if (rc) Cr0(R(d)); return; }  // adde
        case 202: { const uint32_t x = R(a); R(d) = AddCarry(x, 0, Ca(), true); if (oe) Overflow(x, 0, R(d)); if (rc) Cr0(R(d)); return; }        // addze
        case 234: { const uint32_t x = R(a); R(d) = AddCarry(x, 0xFFFFFFFFu, Ca(), true); if (oe) Overflow(x, 0xFFFFFFFFu, R(d)); if (rc) Cr0(R(d)); return; }  // addme
        case 40: { const uint32_t x = ~R(a), y = R(b), r = x + y + 1; if (oe) Overflow(x, y, r); R(d) = r; if (rc) Cr0(r); return; }  // subf
        case 8: { const uint32_t x = ~R(a), y = R(b); R(d) = AddCarry(x, y, 1, true); if (oe) Overflow(x, y, R(d)); if (rc) Cr0(R(d)); return; }   // subfc
        case 136: { const uint32_t x = ~R(a), y = R(b); R(d) = AddCarry(x, y, Ca(), true); if (oe) Overflow(x, y, R(d)); if (rc) Cr0(R(d)); return; }  // subfe
        case 200: { const uint32_t x = ~R(a); R(d) = AddCarry(x, 0, Ca(), true); if (oe) Overflow(x, 0, R(d)); if (rc) Cr0(R(d)); return; }        // subfze
        case 232: { const uint32_t x = ~R(a); R(d) = AddCarry(x, 0xFFFFFFFFu, Ca(), true); if (oe) Overflow(x, 0xFFFFFFFFu, R(d)); if (rc) Cr0(R(d)); return; }  // subfme
        case 104: { const uint32_t x = R(a); R(d) = 0u - x; if (oe) SetOv(x == 0x80000000u); if (rc) Cr0(R(d)); return; }                       // neg
        case 235: { const int64_t p = int64_t(int32_t(R(a))) * int32_t(R(b)); R(d) = uint32_t(p); if (oe) SetOv(p != int64_t(int32_t(p))); if (rc) Cr0(R(d)); return; }  // mullw
        case 75: R(d) = uint32_t((int64_t(int32_t(R(a))) * int32_t(R(b))) >> 32); if (rc) Cr0(R(d)); return;    // mulhw
        case 11: R(d) = uint32_t((uint64_t(R(a)) * R(b)) >> 32); if (rc) Cr0(R(d)); return;                      // mulhwu
        case 491: {                                                                                                // divw
            const int32_t x = int32_t(R(a)), y = int32_t(R(b));
            if (oe) SetOv(y == 0 || (x == INT32_MIN && y == -1));
            R(d) = uint32_t(PPC_Divw(x, y)); if (rc) Cr0(R(d)); return;
        }
        case 459: if (oe) SetOv(R(b) == 0); R(d) = PPC_Divwu(R(a), R(b)); if (rc) Cr0(R(d)); return;           // divwu
        default: break;
    }
    switch (xo) {
        case 0: case 32: {                                                               // cmp, cmpl
            uint32_t f;
            if (xo == 0) { const int32_t x = int32_t(R(a)), y = int32_t(R(b)); f = x < y ? 8 : x > y ? 4 : 2; }
            else { const uint32_t x = R(a), y = R(b); f = x < y ? 8 : x > y ? 4 : 2; }
            SetCrField(d >> 2, f | (c_.xer >> 31));
            return;
        }
        case 4: {                                                                        // tw
            const int32_t x = int32_t(R(a)), y = int32_t(R(b));
            if (((d & 16) && x < y) || ((d & 8) && x > y) || ((d & 4) && x == y) ||
                ((d & 2) && uint32_t(x) < uint32_t(y)) || ((d & 1) && uint32_t(x) > uint32_t(y)))
                Fatal(ins, "trap (tw)");
            return;
        }
        case 28: R(a) = R(d) & R(b); break;                                              // and
        case 60: R(a) = R(d) & ~R(b); break;                                             // andc
        case 444: R(a) = R(d) | R(b); break;                                             // or (mr)
        case 412: R(a) = R(d) | ~R(b); break;                                            // orc
        case 316: R(a) = R(d) ^ R(b); break;                                             // xor
        case 476: R(a) = ~(R(d) & R(b)); break;                                          // nand
        case 124: R(a) = ~(R(d) | R(b)); break;                                          // nor
        case 284: R(a) = ~(R(d) ^ R(b)); break;                                          // eqv
        case 24: R(a) = PPC_Slw(R(d), R(b)); break;                                      // slw
        case 536: R(a) = PPC_Srw(R(d), R(b)); break;                                     // srw
        case 792: {                                                                      // sraw
            const uint32_t s = R(d), n = R(b) & 0x3F;
            const uint32_t r = PPC_Sraw(s, n);
            SetCa(int32_t(s) < 0 && (n > 31 ? s != 0 : (s & ((1u << n) - 1)) != 0));
            R(a) = r;
            break;
        }
        case 824: {                                                                      // srawi
            const uint32_t s = R(d), n = b;
            R(a) = uint32_t(int32_t(s) >> n);
            SetCa(int32_t(s) < 0 && n && (s & ((1u << n) - 1)) != 0);
            break;
        }
        case 26: R(a) = PPC_CntlzwInline(R(d)); break;                                   // cntlzw
        case 922: R(a) = uint32_t(int32_t(int16_t(R(d)))); break;                        // extsh
        case 954: R(a) = uint32_t(int32_t(int8_t(R(d)))); break;                         // extsb

        // X-form loads/stores; xo bit 5 set = update form.
        case 23: R(d) = FlatRead32At(ea, 0); return;                                     // lwzx
        case 55: R(d) = FlatRead32At(ea, 0); R(a) = ea; return;                          // lwzux
        case 87: R(d) = FlatRead8At(ea, 0); return;                                      // lbzx
        case 119: R(d) = FlatRead8At(ea, 0); R(a) = ea; return;                          // lbzux
        case 279: R(d) = FlatRead16At(ea, 0); return;                                    // lhzx
        case 311: R(d) = FlatRead16At(ea, 0); R(a) = ea; return;                         // lhzux
        case 343: R(d) = uint32_t(int32_t(int16_t(FlatRead16At(ea, 0)))); return;        // lhax
        case 375: R(d) = uint32_t(int32_t(int16_t(FlatRead16At(ea, 0)))); R(a) = ea; return;  // lhaux
        case 151: FlatWrite32At(ea, 0, R(d)); return;                                    // stwx
        case 183: FlatWrite32At(ea, 0, R(d)); R(a) = ea; return;                         // stwux
        case 215: FlatWrite8At(ea, 0, uint8_t(R(d))); return;                            // stbx
        case 247: FlatWrite8At(ea, 0, uint8_t(R(d))); R(a) = ea; return;                 // stbux
        case 407: FlatWrite16At(ea, 0, uint16_t(R(d))); return;                          // sthx
        case 439: FlatWrite16At(ea, 0, uint16_t(R(d))); R(a) = ea; return;               // sthux
        case 534: R(d) = Bswap32(FlatRead32At(ea, 0)); return;                           // lwbrx
        case 790: R(d) = Bswap16(FlatRead16At(ea, 0)); return;                           // lhbrx
        case 662: FlatWrite32At(ea, 0, Bswap32(R(d))); return;                           // stwbrx
        case 918: FlatWrite16At(ea, 0, Bswap16(uint16_t(R(d)))); return;                 // sthbrx
        case 535: SetScalar(d, double(FlatReadFloat32At(ea, 0))); return;                // lfsx
        case 567: SetScalar(d, double(FlatReadFloat32At(ea, 0))); R(a) = ea; return;     // lfsux
        case 599: SetScalar(d, FlatReadFloat64At(ea, 0)); return;                        // lfdx
        case 631: SetScalar(d, FlatReadFloat64At(ea, 0)); R(a) = ea; return;             // lfdux
        case 663: FlatWriteFloat32At(ea, 0, Scalar(d)); return;                          // stfsx
        case 695: FlatWriteFloat32At(ea, 0, Scalar(d)); R(a) = ea; return;               // stfsux
        case 727: FlatWriteFloat64At(ea, 0, Scalar(d)); return;                          // stfdx
        case 759: FlatWriteFloat64At(ea, 0, Scalar(d)); R(a) = ea; return;               // stfdux
        case 983: FlatWrite32At(ea, 0, uint32_t(c_.fpr[d].raw)); return;                 // stfiwx
        case 20: R(d) = FlatRead32At(ea, 0); return;                                     // lwarx (one guest CPU)
        case 150: FlatWrite32At(ea, 0, R(d)); SetCrField(0, 2u | (c_.xer >> 31)); return;  // stwcx.
        case 533: case 597: case 661: case 725: return LoadStoreMultipleString(ins, xo);
        case 1014:                                                                       // dcbz
            for (uint32_t i = 0; i < 32; i += 4) FlatWriteRam32At(ea & ~31u, int32_t(i), 0);
            return;
        case 54: case 86: case 246: case 278: case 470: case 982: case 598: case 854: case 566:
            return;                                                                      // cache ops, sync, eieio, tlbsync

        // CR, XER, SPRs.
        case 19: R(d) = c_.cr; return;                                                   // mfcr
        case 144: {                                                                      // mtcrf
            const uint32_t crm = (ins >> 12) & 0xFF;
            uint32_t m = 0;
            for (int f = 0; f < 8; ++f) if (crm & (0x80 >> f)) m |= 0xF0000000u >> (4 * f);
            c_.cr = (c_.cr & ~m) | (R(d) & m);
            return;
        }
        case 512: SetCrField(d >> 2, c_.xer >> 28); c_.xer &= 0x0FFFFFFFu; return;        // mcrxr
        case 83: R(d) = c_.msr; return;                                                  // mfmsr
        case 146: c_.msr = R(d); return;                                                 // mtmsr
        case 595: case 659: R(d) = 0; return;                                            // mfsr(in)
        case 210: case 242: return;                                                      // mtsr(in)
        case 339: case 371: {                                                            // mfspr, mftb
            const uint32_t spr = ((ins >> 16) & 31) | (((ins >> 11) & 31) << 5);
            switch (spr) {
                case 1: R(d) = c_.xer; return;
                case 8: R(d) = c_.lr; return;
                case 9: R(d) = c_.ctr; return;
                case 26: R(d) = c_.srr0; return;
                case 27: R(d) = c_.srr1; return;
                case 1008: R(d) = c_.hid0; return;
                case 1009: R(d) = c_.hid1; return;
                case 920: R(d) = c_.hid2; return;
                default:
                    if (spr >= 912 && spr <= 919) { R(d) = c_.gqr[spr - 912]; return; }
                    if (!h_.readSpr) Fatal(ins, "mfspr of an SPR the runtime does not model");
                    R(d) = h_.readSpr(spr);
                    return;
            }
        }
        case 467: {                                                                      // mtspr
            const uint32_t spr = ((ins >> 16) & 31) | (((ins >> 11) & 31) << 5);
            switch (spr) {
                case 1: c_.xer = R(d); return;
                case 8: c_.lr = R(d); return;
                case 9: c_.ctr = R(d); return;
                case 26: c_.srr0 = R(d); return;
                case 27: c_.srr1 = R(d); return;
                case 1008: c_.hid0 = R(d); return;
                case 1009: c_.hid1 = R(d); return;
                case 920: c_.hid2 = R(d); return;
                default:
                    if (spr >= 912 && spr <= 919) { c_.gqr[spr - 912] = R(d); return; }
                    if (!h_.writeSpr) Fatal(ins, "mtspr of an SPR the runtime does not model");
                    h_.writeSpr(spr, R(d));
                    return;
            }
        }
        default: Fatal(ins, "unknown opcode 31 form");
    }
    if (rc) Cr0(R(a));  // logical and shift forms fall through here
}

void Interp::LoadStoreMultipleString(uint32_t ins, uint32_t xo) {
    const uint32_t d = (ins >> 21) & 31, a = (ins >> 16) & 31, b = (ins >> 11) & 31;
    const bool immediate = xo == 597 || xo == 725, store = xo == 661 || xo == 725;
    uint32_t ea = immediate ? RA0(a) : RA0(a) + R(b);
    uint32_t n = immediate ? (b ? b : 32) : (c_.xer & 0x7F);
    uint32_t r = d == 0 ? 31 : d - 1, shift = 0;
    for (; n; --n, ++ea) {
        if (shift == 0) {
            r = (r + 1) & 31;
            if (!store) R(r) = 0;
        }
        const uint32_t bit = 24 - shift;
        if (store) FlatWrite8At(ea, 0, uint8_t(R(r) >> bit));
        else R(r) |= uint32_t(FlatRead8At(ea, 0)) << bit;
        shift = (shift + 8) & 31;
    }
}

void Interp::Op59(uint32_t ins) {                                                       // single-precision A-form
    const uint32_t d = (ins >> 21) & 31, a = (ins >> 16) & 31, b = (ins >> 11) & 31, c = (ins >> 6) & 31;
    double r = 0;
    switch ((ins >> 1) & 31) {
        case 18: PpcFdivsStateInline(r, Scalar(a), Scalar(b)); break;
        case 20: PpcFsubsStateInline(r, Scalar(a), Scalar(b)); break;
        case 21: PpcFaddsStateInline(r, Scalar(a), Scalar(b)); break;
        case 25: PpcFmulsStateInline(r, Scalar(a), Scalar(c)); break;
        case 28: PpcFmsubsStateInline(r, Scalar(a), Scalar(c), Scalar(b)); break;
        case 29: PpcFmaddsStateInline(r, Scalar(a), Scalar(c), Scalar(b)); break;
        case 30: PpcFnmsubsStateInline(r, Scalar(a), Scalar(c), Scalar(b)); break;
        case 31: r = PpcFnmaddsInline(Scalar(a), Scalar(c), Scalar(b)); break;
        case 24:                                                                         // fres: a packed, replicated result
            SetPair(d, PpcFresValueStateInline(c_.fpr[d].d, Scalar(b)));
            if (ins & 1) Cr1();
            return;
        default: Fatal(ins, "unknown opcode 59 form");
    }
    SetScalar(d, r);
    if (ins & 1) Cr1();
}

void Interp::Op63(uint32_t ins) {
    const uint32_t d = (ins >> 21) & 31, a = (ins >> 16) & 31, b = (ins >> 11) & 31, c = (ins >> 6) & 31;
    const uint32_t xo5 = (ins >> 1) & 31;
    if (xo5 >= 18 && xo5 != 22 && xo5 != 24 && xo5 != 27) {                             // A-form
        double r = 0;
        switch (xo5) {
            case 18: PpcFdivStateInline(r, Scalar(a), Scalar(b)); break;
            case 20: PpcFsubStateInline(r, Scalar(a), Scalar(b)); break;
            case 21: PpcFaddStateInline(r, Scalar(a), Scalar(b)); break;
            case 23: r = PPC_Fsel(Scalar(a), Scalar(b), Scalar(c)); break;
            case 25: PpcFmulStateInline(r, Scalar(a), Scalar(c)); break;
            case 26: PpcFrsqrteStateInline(r, Scalar(b)); break;
            case 28: r = PpcFmsubInline(Scalar(a), Scalar(c), Scalar(b)); break;
            case 29: PpcFmaddStateInline(r, Scalar(a), Scalar(c), Scalar(b)); break;
            case 30: PpcFnmsubStateInline(r, Scalar(a), Scalar(c), Scalar(b)); break;
            case 31: r = PpcFnmaddInline(Scalar(a), Scalar(c), Scalar(b)); break;
            default: Fatal(ins, "unknown opcode 63 A-form");
        }
        SetScalar(d, r);
        if (ins & 1) Cr1();
        return;
    }
    switch ((ins >> 1) & 0x3FF) {
        case 0: PpcCompareCrStateInline(c_.cr, d >> 2, false, Scalar(a), Scalar(b)); return;   // fcmpu
        case 32: PpcCompareCrStateInline(c_.cr, d >> 2, true, Scalar(a), Scalar(b)); return;   // fcmpo
        case 12: SetScalar(d, double(PpcForceSingleValueInline(Scalar(b)))); break;           // frsp
        case 14: SetScalar(d, PPC_Fctiw(Scalar(b))); break;                                    // fctiw
        case 15: { double r = 0; PpcFctiwzStateInline(r, Scalar(b)); SetScalar(d, r); break; } // fctiwz
        case 40: { PPC_FPR v; v.d = Scalar(b); v.raw ^= 0x8000000000000000ull; SetScalar(d, v.d); break; }   // fneg
        case 136: { PPC_FPR v; v.d = Scalar(b); v.raw |= 0x8000000000000000ull; SetScalar(d, v.d); break; }  // fnabs
        case 264: { PPC_FPR v; v.d = Scalar(b); v.raw &= ~0x8000000000000000ull; SetScalar(d, v.d); break; } // fabs
        case 72:                                                                               // fmr keeps the representation
            c_.fpr[d] = c_.fpr[b];
            s_.paired = (s_.paired & ~(1u << d)) | (uint32_t(Paired(b)) << d);
            break;
        case 583: SetScalar(d, PPC_Mffs()); break;                                             // mffs
        case 711: PPC_Mtfsf((ins >> 17) & 0xFF, c_.fpr[b].d); break;                           // mtfsf
        case 38: PPC_Mtfsb1(d); break;                                                         // mtfsb1
        case 70: PPC_Mtfsb0(d); break;                                                         // mtfsb0
        case 134: PPC_Mtfsfi(d >> 2, (ins >> 12) & 0xF); break;                                // mtfsfi
        case 64: {                                                                             // mcrfs
            const uint32_t shift = (7 - (a >> 2)) * 4;
            SetCrField(d >> 2, (c_.fpscr >> shift) & 0xF);
            c_.fpscr &= ~((0xFu << shift) & 0x9FF80700u);  // clears the exception bits it copied
            return;
        }
        default: Fatal(ins, "unknown opcode 63 form");
    }
    if (ins & 1) Cr1();
}

void Interp::Op4(uint32_t ins) {                                                        // paired singles
    const uint32_t d = (ins >> 21) & 31, a = (ins >> 16) & 31, b = (ins >> 11) & 31, c = (ins >> 6) & 31;
    const uint32_t xo6 = (ins >> 1) & 63;
    if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) {                               // psq_lx/stx/lux/stux
        const uint32_t w = (ins >> 10) & 1, i = (ins >> 7) & 7;
        const bool update = xo6 >= 38;
        const uint32_t ea = (update ? R(a) : RA0(a)) + R(b);
        if (xo6 & 1) kPsqStore[w][i](c_.gqr[i], ea, Pair(d));
        else SetPair(d, kPsqLoad[w][i](c_.gqr[i], ea));
        if (update) R(a) = ea;
        return;
    }
    double r = 0;
    switch ((ins >> 1) & 31) {
        case 10: r = PPC_PsSum0Inline(Pair(a), Pair(b), Pair(c)); break;
        case 11: r = PPC_PsSum1Inline(Pair(a), Pair(b), Pair(c)); break;
        case 12: r = PPC_PsMuls0Inline(Pair(a), Pair(c)); break;
        case 13: r = PPC_PsMuls1Inline(Pair(a), Pair(c)); break;
        case 14: r = PPC_PsMadds0Inline(Pair(a), Pair(c), Pair(b)); break;
        case 15: r = PPC_PsMadds1Inline(Pair(a), Pair(c), Pair(b)); break;
        case 18: r = PPC_PsDivInline(Pair(a), Pair(b)); break;
        case 20: r = PPC_PsSubInline(Pair(a), Pair(b)); break;
        case 21: r = PPC_PsAddInline(Pair(a), Pair(b)); break;
        case 23: r = PPC_PsSelInline(Pair(c), Pair(a), Pair(b)); break;     // a >= -0 ? c : b
        case 24: {                                                          // ps_res
            const double p = Pair(b);
            r = PpcPackPairedInline(float(PpcApproximateReciprocalInline(PpcGetPs0Inline(p))),
                                    float(PpcApproximateReciprocalInline(PpcGetPs1Inline(p))));
            break;
        }
        case 25: r = PPC_PsMulInline(Pair(a), Pair(c)); break;
        case 26: {                                                          // ps_rsqrte
            const double p = Pair(b);
            r = PpcPackPairedInline(float(PpcApproximateReciprocalSquareRootInline(PpcGetPs0Inline(p))),
                                    float(PpcApproximateReciprocalSquareRootInline(PpcGetPs1Inline(p))));
            break;
        }
        case 28: r = PPC_PsMsubInline(Pair(a), Pair(c), Pair(b)); break;
        case 29: r = PPC_PsMaddInline(Pair(a), Pair(c), Pair(b)); break;
        case 30: r = PPC_PsNmsubInline(Pair(a), Pair(c), Pair(b)); break;
        case 31: r = PPC_PsNmaddInline(Pair(a), Pair(c), Pair(b)); break;
        default:
            switch ((ins >> 1) & 0x3FF) {
                case 0: case 32: case 64: case 96: {                        // ps_cmpu0/o0/u1/o1
                    const double pa = Pair(a), pb = Pair(b);
                    const bool lane1 = ((ins >> 1) & 0x3FF) >= 64;
                    const bool ordered = (((ins >> 1) & 0x3FF) & 32) != 0;
                    const double x = lane1 ? PpcGetPs1Inline(pa) : PpcGetPs0Inline(pa);
                    const double y = lane1 ? PpcGetPs1Inline(pb) : PpcGetPs0Inline(pb);
                    PpcCompareCrStateInline(c_.cr, d >> 2, ordered, x, y);
                    return;
                }
                case 40: r = PPC_PsNegInline(Pair(b)); break;
                case 72: r = Pair(b); break;                                // ps_mr
                case 136: r = PPC_PsNegInline(PPC_PsAbsInline(Pair(b))); break;   // ps_nabs
                case 264: r = PPC_PsAbsInline(Pair(b)); break;
                case 528: r = PPC_PsMerge00Inline(Pair(a), Pair(b)); break;
                case 560: r = PPC_PsMerge01Inline(Pair(a), Pair(b)); break;
                case 592: r = PPC_PsMerge10Inline(Pair(a), Pair(b)); break;
                case 624: r = PPC_PsMerge11Inline(Pair(a), Pair(b)); break;
                case 1014: {                                                // dcbz_l
                    const uint32_t ea = (RA0(a) + R(b)) & ~31u;
                    for (uint32_t i = 0; i < 32; i += 4) FlatWriteRam32At(ea, int32_t(i), 0);
                    return;
                }
                default: Fatal(ins, "unknown paired-single form");
            }
    }
    SetPair(d, r);
    if (ins & 1) Cr1();
}

}  // namespace

RunStats Run(CpuContext* ctx, uint32_t entry, const InterpHooks& hooks, State& state) {
    return Interp(ctx, hooks, state).Run(entry);
}

}  // namespace dropin
