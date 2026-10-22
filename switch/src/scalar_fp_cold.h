#pragma once
#include <kartpad/semantics/ppc_semantics.h>

// Keep the existing exceptional scalar-FP semantics in shared cold functions.
// The translated finite fast paths and their instruction ordering stay intact.
namespace KartPadSwitchFp {
[[gnu::noinline, gnu::cold]] bool Binary(
    double& destination, double a, double b,
    kartpad::semantics::ScalarFpBinaryOperation operation, bool single, bool force25);
[[gnu::noinline, gnu::cold]] bool Fused(
    double& destination, double a, double c, double b,
    bool subtract, bool single, bool negative);
[[gnu::noinline, gnu::cold]] uint32_t Compare(bool ordered, double a, double b);
}
