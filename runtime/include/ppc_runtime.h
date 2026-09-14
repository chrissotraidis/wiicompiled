// The PowerPC ISA package (runtime/include/isa/), re-exported under the single
// header name the generated code and the runtime include. The package has two
// host seams this project satisfies elsewhere: "ppc_isa_memory.h" (pulled by
// the quantized tier) and the ShowRuntimeFatalPopup implementation.

#pragma once

#include "isa/ppc_isa_config.h"
#include "isa/big_endian.h"
#include "isa/ppc_isa_fpenv.h"
#include "isa/ppc_isa_context.h"
#include "isa/ppc_isa_int.h"
#include "isa/ppc_isa_float.h"
#include "isa/ppc_isa_quantized.h"
#include <kartpad/semantics/ppc_semantics.h>

// The translator emits state-aware scalar helpers so FPSCR sticky bits,
// exception enables, FPRF, and suppressed destination writes remain visible
// to the guest.  Keep these tiny adapters beside the generated-code umbrella
// header; the exact architectural rules live in KartPad's tested semantics
// library and are shared with the translation conformance fixtures.
inline bool PpcCommitScalarFpInline(
    double& destination, kartpad::semantics::ScalarFpResult result)
{
    CpuContext* cpu = CurrentCpuContext();
    cpu->fpscr = result.fpscr;
    MkwApplyHostNiMode(cpu->fpscr);
    if (result.write_destination)
        destination = result.value;
    return result.write_destination;
}

inline uint32_t PpcCompareStateInline(bool ordered, double a, double b)
{
    CpuContext* cpu = CurrentCpuContext();
    const auto result = kartpad::semantics::EvaluatePpcFloatCompare(
        cpu->fpscr, a, b, ordered);
    cpu->fpscr = result.fpscr;
    MkwApplyHostNiMode(cpu->fpscr);
    return result.condition;
}

inline bool PpcFaddsStateInline(double& d, double a, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcScalarBinary(CurrentCpuContext()->fpscr, kartpad::semantics::ScalarFpBinaryOperation::Add, a, b, true)); }
inline bool PpcFsubsStateInline(double& d, double a, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcScalarBinary(CurrentCpuContext()->fpscr, kartpad::semantics::ScalarFpBinaryOperation::Subtract, a, b, true)); }
inline bool PpcFmulsStateInline(double& d, double a, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcScalarBinary(CurrentCpuContext()->fpscr, kartpad::semantics::ScalarFpBinaryOperation::Multiply, a, kartpad::semantics::Force25Bit(b), true)); }
inline bool PpcFdivsStateInline(double& d, double a, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcScalarBinary(CurrentCpuContext()->fpscr, kartpad::semantics::ScalarFpBinaryOperation::Divide, a, b, true)); }
inline bool PpcFaddStateInline(double& d, double a, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcScalarBinary(CurrentCpuContext()->fpscr, kartpad::semantics::ScalarFpBinaryOperation::Add, a, b, false)); }
inline bool PpcFsubStateInline(double& d, double a, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcScalarBinary(CurrentCpuContext()->fpscr, kartpad::semantics::ScalarFpBinaryOperation::Subtract, a, b, false)); }
inline bool PpcFmulStateInline(double& d, double a, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcScalarBinary(CurrentCpuContext()->fpscr, kartpad::semantics::ScalarFpBinaryOperation::Multiply, a, b, false)); }
inline bool PpcFdivStateInline(double& d, double a, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcScalarBinary(CurrentCpuContext()->fpscr, kartpad::semantics::ScalarFpBinaryOperation::Divide, a, b, false)); }
inline bool PpcFctiwzStateInline(double& d, double value) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcConvertToInteger(CurrentCpuContext()->fpscr, value, FE_TOWARDZERO)); }
inline bool PpcFmaddsStateInline(double& d, double a, double c, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcFused(CurrentCpuContext()->fpscr, a, c, b, false, true, false)); }
inline bool PpcFmsubsStateInline(double& d, double a, double c, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcFused(CurrentCpuContext()->fpscr, a, c, b, true, true, false)); }
inline bool PpcFnmsubStateInline(double& d, double a, double c, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcFused(CurrentCpuContext()->fpscr, a, c, b, true, false, true)); }
inline bool PpcFnmsubsStateInline(double& d, double a, double c, double b) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcFused(CurrentCpuContext()->fpscr, a, c, b, true, true, true)); }
inline bool PpcFrsqrteStateInline(double& d, double value) { return PpcCommitScalarFpInline(d, kartpad::semantics::EvaluatePpcEstimate(CurrentCpuContext()->fpscr, value, true)); }

inline double PpcFresValueStateInline(double current, double value)
{
    CpuContext* cpu = CurrentCpuContext();
    const auto result = kartpad::semantics::EvaluatePpcEstimate(
        cpu->fpscr, value, false);
    cpu->fpscr = result.fpscr;
    MkwApplyHostNiMode(cpu->fpscr);
    if (!result.write_destination)
        return current;
    const float lane = static_cast<float>(result.value);
    return PpcPackPairedInline(lane, lane);
}
