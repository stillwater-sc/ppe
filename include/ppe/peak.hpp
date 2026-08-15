// peak.hpp -- a stated per-type peak model, so "efficiency" means something.
//
// Adapted from mtl5/ppe/include/ppe/peak.hpp (Stillwater, MIT). That version
// hardcodes one microarchitecture:
//
//     // Target: Alder Lake P-core, AVX2 + FMA + AVX-VNNI, no AVX-512.
//     fp64   2 FMA units x 4 lanes x 2 ops per FMA  = 16 ops/cycle
//
// This one derives what is derivable and is explicit about what is not. The
// three factors in that line have very different epistemic status:
//
//   lanes       DERIVED. vector_bits / bits(T), from runtime ISA detection.
//   ops per FMA DERIVED. 2 when the ISA has FMA, 1 when it does not.
//   FMA units   NOT DERIVABLE. This is a microarchitectural property that no
//               instruction reports. It is an input, defaulting to 2, and every
//               consumer prints the value it used.
//
// WHY NOT MEASURE IT. The mtl5 version records a probe that tried:
//
//     A probe that MEASURED achievable rate looked more honest but was worse:
//     with literal operands the compiler folded the loop away and it reported
//     3.8 million GOP/s; once the operands were made opaque it reported 17.7
//     GOP/s for fp64, BELOW the 37 GOP/s the GEMM kernels actually achieve...
//     A probe that is beaten by the thing it is meant to bound is not a ceiling.
//
// That result is inherited here rather than re-derived. Writing a portable probe
// that reliably reaches vector peak is its own project; the ISA model plus a
// stated FMA-unit count is the honest tool at this scope.
//
// THE MODEL IS WRITTEN DOWN IN FULL so a reader can disagree with a specific
// number rather than with the word "peak". A measurement above 100% of peak
// means the MODEL is wrong, not that the kernel is superhuman, and consumers
// should say so loudly.
#pragma once

#include <ppe/detect/isa.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace ppe {

/// Inputs to the peak model that cannot be detected.
struct peak_model {
    /// FMA/vector-issue units per core. Not reported by any instruction.
    ///
    /// 2 is right for Intel big cores since Haswell and for AMD since Zen 2. It
    /// is WRONG for Intel E-cores (Gracemont issues 1), for older parts, and for
    /// Apple's cores (4 on Firestorm). Left as an input with a documented
    /// default rather than a family/model table, because such a table is wrong
    /// silently and on hardware nobody had when it was written -- an input is
    /// wrong visibly.
    unsigned fma_units = 2;

    /// Sustained single-core clock in GHz. See clock.hpp for how this is
    /// obtained and why it is a claim rather than a measurement.
    double ghz = 0.0;

    /// Widest usable vector, in bits, for the model's lane count.
    unsigned vector_bits = 0;

    /// True when the ISA has fused multiply-add, so one instruction retires two
    /// operations.
    bool has_fma = false;

    /// Human-readable note about anything approximated, for the report.
    std::string caveat;
};

/// Build the model from detected ISA plus the inputs that cannot be detected.
inline peak_model make_peak_model(const isa_capabilities& isa, unsigned fma_units,
                                  double ghz) {
    peak_model m;
    m.fma_units = fma_units;
    m.ghz = ghz;
    m.vector_bits = isa.vector_bits;
    m.has_fma = isa.fma || isa.neon || isa.sve;  // AArch64 FMA is mandatory
    if (isa.sve) {
        m.caveat = "SVE present; vector width reported at the 128-bit NEON floor";
    }
    return m;
}

/// Operations per cycle for operand type T under the model.
///
/// The per-type reasoning, following the source material's structure. Each case
/// states what the hardware actually offers, not what the type nominally is:
///
///   fp64/fp32  full-rate FMA on every unit. units * lanes * 2.
///   fp16       NO native arithmetic without AVX512-FP16 or ARM FP16. Operands
///              convert to fp32 and back, so fp32 is the CEILING and the
///              conversions are extra work on top -- expect a large shortfall,
///              which is the point of modelling it this way rather than
///              pretending 2x fp32.
///   int8       no integer FMA. Accumulating in int32 means widening, so the
///              realistic vector path is the int16 multiply -- modelled as
///              int16 rather than as lanes that do not exist.
///   int16      vector multiply (vpmullw) paired with an add.
///   int32      vector multiply (vpmulld) paired with an add.
///   int64      NO 64-bit SIMD multiply before AVX-512DQ; emulated from 32-bit
///              pieces. On an AVX-512DQ machine this understates the ceiling.
///
/// THE UNIT COUNT APPLIES TO FLOATING POINT ONLY, and that asymmetry is the
/// model's, not an oversight. A part with two FMA pipes does not have two vector
/// integer multipliers: vpmulld is multi-uop and issues at roughly one per
/// cycle. Multiplying the integer rows by fma_units would inflate every integer
/// ceiling by exactly the factor that makes a real int32 kernel look half as
/// efficient as it is. The source material encodes this by writing the FP rows
/// as "2 FMA units x lanes x 2" and the integer rows as "lanes, paired with an
/// add" -- deriving the lane count from detected width must not quietly change
/// that structure.
template <typename T>
double ops_per_cycle(const peak_model& m) {
    const unsigned bits = m.vector_bits ? m.vector_bits : 64;  // scalar floor
    const double per_fma = m.has_fma ? 2.0 : 1.0;
    const double units = static_cast<double>(m.fma_units);

    auto lanes = [&](std::size_t elem_bytes) {
        const double l = static_cast<double>(bits) / (elem_bytes * 8.0);
        return l < 1.0 ? 1.0 : l;
    };

    if constexpr (std::is_same_v<T, double>) {
        return units * lanes(8) * per_fma;
    } else if constexpr (std::is_same_v<T, float>) {
        return units * lanes(4) * per_fma;
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        // Emulated from 32-bit pieces: the lane count is the 64-bit one, and the
        // pairing with an add still gives 2 ops per element.
        return lanes(8) * 2.0;
    } else if constexpr (std::is_same_v<T, std::int32_t>) {
        return lanes(4) * 2.0;
    } else if constexpr (std::is_same_v<T, std::int16_t>) {
        return lanes(2) * 2.0;
    } else if constexpr (std::is_same_v<T, std::int8_t>) {
        // Modelled as the int16 path: widening into a wider accumulator is what
        // real quantized GEMM does.
        return lanes(2) * 2.0;
    } else {
        // _Float16 and friends: fp32 is the ceiling.
        return units * lanes(4) * per_fma;
    }
}

/// Modelled single-core peak in GOP/s (giga-operations per second).
///
/// GOP/s rather than GFLOP/s because the type matrix spans integer and floating
/// point alike, and a GEMM performs 2*m*n*k operations under the usual
/// convention for both -- so the two halves are comparable on one axis.
template <typename T>
double peak_gops(const peak_model& m) {
    return ops_per_cycle<T>(m) * m.ghz;
}

/// The model as text, so a result file records the ceiling it was measured
/// against rather than just a percentage.
inline std::string describe(const peak_model& m) {
    std::string s;
    s += "peak model: " + std::to_string(m.fma_units) + " units x " +
         std::to_string(m.vector_bits) + "-bit vectors";
    s += m.has_fma ? " with FMA" : " without FMA";
    s += " at " + std::to_string(m.ghz) + " GHz";
    if (!m.caveat.empty()) s += " (" + m.caveat + ")";
    return s;
}

}  // namespace ppe
