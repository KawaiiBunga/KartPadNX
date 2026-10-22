#include "ppc_runtime.h"
#include "scalar_fp_cold.h"

namespace KartPadSwitchFp {
bool Binary(double& destination, double a, double b,
            kartpad::semantics::ScalarFpBinaryOperation operation, bool single, bool force25) {
    if (force25) b = kartpad::semantics::Force25Bit(b);
    CpuContext* cpu = CurrentCpuContext();
    return PpcCommitScalarFpInline(cpu, destination,
        kartpad::semantics::EvaluatePpcScalarBinary(cpu->fpscr, operation, a, b, single));
}
bool Fused(double& destination, double a, double c, double b,
           bool subtract, bool single, bool negative) {
    CpuContext* cpu = CurrentCpuContext();
    return PpcCommitScalarFpInline(cpu, destination,
        kartpad::semantics::EvaluatePpcFused(cpu->fpscr, a, c, b, subtract, single, negative));
}
uint32_t Compare(bool ordered, double a, double b) {
    CpuContext* cpu = CurrentCpuContext();
    const auto result = kartpad::semantics::EvaluatePpcFloatCompare(cpu->fpscr, a, b, ordered);
    cpu->fpscr = result.fpscr;
    return result.condition;
}
}
