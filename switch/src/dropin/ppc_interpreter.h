// PowerPC (Broadway) interpreter for drop-in Retro Rewind (docs/RR-DROP-IN.md): runs guest code
// that has no valid translation, such as a newer Code.pul than the one built in. It works on the
// same CpuContext and flat guest memory as translated code and computes every FP and paired-single
// result with the same runtime helpers (ppc_runtime.h), so a function gives the same bits whether
// it runs translated or interpreted.
//
// FPR representation: like translated code, an FPR's 64 bits hold either a scalar double or a
// packed pair (ps0 high word, ps1 low word). The translator tracks which one statically; the
// interpreter tracks it per register at run time (State::paired) and converts operands with the
// same PPC_PsToScalarInline / PPC_PsFromScalarInline calls the translator emits.
#pragma once

#include <cstdint>

struct CpuContext;

namespace dropin {

inline constexpr uint32_t kReturnSentinel = 0xFFFFFFF0u;  // LR value that ends Run()

struct State {
    uint32_t paired = 0;   // bit i: fpr[i] holds a packed pair, not a scalar double
};

struct InterpHooks {
    using NativeFn = void (*)(CpuContext*);
    // The translated function to call for a guest function entry, or nullptr to interpret it.
    // Must return nullptr for addresses that are not function entries.
    NativeFn (*native)(uint32_t address) = nullptr;
    // Called around each native call so FPRs match what the callee expects (the translator's
    // per-function float ABI). Default: none; a pair stays packed across the call and f1 is
    // treated as a scalar return, as translated callers do for an untyped callee.
    void (*beforeNative)(uint32_t target, CpuContext* ctx, State& state) = nullptr;
    void (*afterNative)(uint32_t target, CpuContext* ctx, State& state) = nullptr;
    // SPRs CpuContext does not hold (and mftb), the system call, and traps.
    uint32_t (*readSpr)(uint32_t spr) = nullptr;
    void (*writeSpr)(uint32_t spr, uint32_t value) = nullptr;
    void (*systemCall)(CpuContext* ctx) = nullptr;
    // An instruction the interpreter cannot run (or a taken trap). Should not return; if it does,
    // the runtime's PPC_Undefined aborts.
    void (*fatal)(uint32_t pc, uint32_t instruction, const char* why) = nullptr;
};

struct RunStats {
    uint64_t instructions = 0;
    uint64_t nativeCalls = 0;
};

// Runs the guest function at `entry` until it returns (branches to the LR it was entered with).
// All registers are read from and left in `ctx`; `state` carries FPR representations in and out
// (zero for a caller whose FPRs are all scalars, which is the translated-code convention).
RunStats Run(CpuContext* ctx, uint32_t entry, const InterpHooks& hooks, State& state);

}  // namespace dropin
