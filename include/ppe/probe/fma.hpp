// fma.hpp -- measure the FMA issue width, the last assumed factor in the peak
// model.
//
// ppe/peak.hpp separates its three factors by what is knowable: lanes are
// derived from the detected vector width, ops-per-FMA is derived from the ISA,
// and the FMA unit count is "NOT DERIVABLE -- no instruction reports this",
// carried as an input defaulting to 2. That default is right for Intel big cores
// since Haswell and AMD since Zen 2, and wrong for Intel E-cores, older parts,
// and Apple's cores. It multiplies every floating-point peak.
//
// WHY THIS CAN BE MEASURED WHEN COMPUTE PEAK COULD NOT. peak.hpp records a
// probe that tried to measure achievable GOP/s and failed twice -- 3.8 million
// GOP/s with the loop folded away, then 17.7 GOP/s once operands were opaque,
// below what real kernels achieved. That probe measured a RATE, in operations
// per second, and so inherited every problem of wall-clock timing: the clock it
// divided by, the frequency the core happened to run at, and whatever else the
// machine was doing.
//
// This measures a RATIO -- FMA instructions per CYCLE -- using hardware
// counters. Clock frequency cancels out entirely, so no sustained-clock figure
// is needed and thermal state does not matter. And the numerator is exact by
// construction rather than inferred: the loop below issues a known number of FMA
// instructions, because each accumulator is a dependent chain the compiler
// cannot fold, reassociate, or hoist.
//
// SATURATION IS THE MEASUREMENT. With enough independent accumulators to cover
// the FMA latency, the loop is throughput-bound and issues one FMA per unit per
// cycle. The measured FMAs/cycle then IS the unit count. Too few accumulators
// and the chain stalls on latency, reporting a fraction of the true width --
// which is why the count below is 12, comfortably above latency x width for
// every part this runs on.
#pragma once

#include <ppe/probe/counters.hpp>

#include <cstdint>
#include <string>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#  define PPE_FMA_PROBE_X86 1
#  include <immintrin.h>
#else
#  define PPE_FMA_PROBE_X86 0
#endif

namespace ppe::probe {

struct fma_measurement {
    bool   ok = false;
    double units = 0.0;        ///< measured FMA instructions per cycle
    int    rounded = 0;        ///< nearest integer, for the peak model
    double fmas_per_cycle = 0.0;
    std::uint64_t cycles = 0;
    std::uint64_t fma_instructions = 0;   ///< what the loop was written to issue
    std::uint64_t instructions_retired = 0;  ///< what the CPU actually retired
    std::string note;          ///< why not, when !ok
};

#if PPE_FMA_PROBE_X86

/// The timed kernel: 12 independent 256-bit FMA chains.
///
/// __attribute__((target)) rather than a global -mfma, so this compiles and runs
/// correctly whatever the build's baseline ISA is -- the probe must not require
/// the whole project to be built for AVX2.
///
/// Each accumulator is its own dependent chain (acc = a*b + acc), so the
/// compiler must emit exactly one FMA per accumulator per iteration: it cannot
/// fold them (floating-point addition is not associative), cannot hoist them
/// (each depends on its previous value), and cannot vectorize across them (they
/// are already vectors). That is what makes the instruction count exact rather
/// than estimated.
__attribute__((target("avx2,fma"))) inline double fma_kernel(std::uint64_t iters,
                                                             double seed) {
    __m256d a = _mm256_set1_pd(seed);
    __m256d b = _mm256_set1_pd(1.0000001);
    // DISTINCT starting values, and this is not cosmetic. Initialising every
    // accumulator to the same value made all twelve chains provably equal at
    // every step, so the compiler collapsed them into ONE -- the disassembly
    // held two vfmadd231pd for an unroll of two, not twelve. The probe then
    // divided a 12x-too-large instruction count by real cycles and reported
    // 2.9992 FMA/cycle on a part that issues 2, having actually measured FMA
    // latency (0.25/cycle = 1/4 cycles) rather than throughput.
    __m256d c0 = _mm256_set1_pd(seed + 1.0);
    __m256d c1 = _mm256_set1_pd(seed + 2.0);
    __m256d c2 = _mm256_set1_pd(seed + 3.0);
    __m256d c3 = _mm256_set1_pd(seed + 4.0);
    __m256d c4 = _mm256_set1_pd(seed + 5.0);
    __m256d c5 = _mm256_set1_pd(seed + 6.0);
    __m256d c6 = _mm256_set1_pd(seed + 7.0);
    __m256d c7 = _mm256_set1_pd(seed + 8.0);
    __m256d c8 = _mm256_set1_pd(seed + 9.0);
    __m256d c9 = _mm256_set1_pd(seed + 10.0);
    __m256d c10 = _mm256_set1_pd(seed + 11.0);
    __m256d c11 = _mm256_set1_pd(seed + 12.0);

    for (std::uint64_t i = 0; i < iters; ++i) {
        c0 = _mm256_fmadd_pd(a, b, c0);
        c1 = _mm256_fmadd_pd(a, b, c1);
        c2 = _mm256_fmadd_pd(a, b, c2);
        c3 = _mm256_fmadd_pd(a, b, c3);
        c4 = _mm256_fmadd_pd(a, b, c4);
        c5 = _mm256_fmadd_pd(a, b, c5);
        c6 = _mm256_fmadd_pd(a, b, c6);
        c7 = _mm256_fmadd_pd(a, b, c7);
        c8 = _mm256_fmadd_pd(a, b, c8);
        c9 = _mm256_fmadd_pd(a, b, c9);
        c10 = _mm256_fmadd_pd(a, b, c10);
        c11 = _mm256_fmadd_pd(a, b, c11);
    }

    // Consume every accumulator, or the compiler is free to delete the chains
    // whose results are unused -- which would silently reduce the instruction
    // count the measurement assumes.
    const __m256d s = _mm256_add_pd(
        _mm256_add_pd(_mm256_add_pd(c0, c1), _mm256_add_pd(c2, c3)),
        _mm256_add_pd(_mm256_add_pd(c4, c5), _mm256_add_pd(c6, c7)));
    const __m256d t = _mm256_add_pd(
        _mm256_add_pd(_mm256_add_pd(c8, c9), _mm256_add_pd(c10, c11)), s);
    alignas(32) double out[4];
    _mm256_store_pd(out, t);
    return out[0] + out[1] + out[2] + out[3];
}

inline constexpr std::uint64_t kChains = 12;

#endif  // PPE_FMA_PROBE_X86

/// Measure FMA instructions issued per cycle.
///
/// Returns ok=false with a reason when the ISA has no FMA path here, or when
/// hardware counters are unavailable -- this needs a cycle count, and there is
/// no honest substitute.
inline fma_measurement measure_fma_units(std::uint64_t iters = 2000000) {
    fma_measurement m;

#if !PPE_FMA_PROBE_X86
    m.note = "the FMA probe is implemented for x86-64 with GCC or Clang only; "
             "other ISAs keep the assumed unit count";
    return m;
#else
    cycle_counter c;
    if (!c.ok()) {
        m.note = "needs hardware counters to count cycles: " + c.note();
        return m;
    }

    // Warm up: the first pass pays for frequency ramp and page faults, and on a
    // part with an AVX offset the core may still be transitioning.
    volatile double warm = fma_kernel(iters / 10 + 1, 1.0);
    (void)warm;

    // Count retired instructions alongside cycles, so the probe can tell
    // whether the compiler emitted the loop it was asked for. This check is the
    // reason the collapsed-accumulator bug above is not still present: an
    // assumed instruction count is exactly the kind of premise that fails
    // silently and produces a clean-looking ratio.
    cycle_counter insn(1 /* PERF_COUNT_HW_INSTRUCTIONS */);

    c.start();
    if (insn.ok()) insn.start();
    volatile double sink = fma_kernel(iters, 1.0);
    m.cycles = c.stop();
    if (insn.ok()) m.instructions_retired = insn.stop();
    (void)sink;

    if (m.cycles == 0) {
        m.note = "counter opened on PMU '" + c.pmu() + "' but counted zero cycles";
        return m;
    }

    m.fma_instructions = iters * kChains;

    // The loop issues kChains FMAs plus a handful of loop-control instructions
    // per iteration, so retired instructions should be at least the FMA count.
    // Materially fewer means the compiler removed work -- collapsed the chains,
    // hoisted them, or deleted them -- and every number below would be computed
    // from a premise that no longer holds.
    if (m.instructions_retired > 0 &&
        m.instructions_retired < m.fma_instructions * 9 / 10) {
        m.note = "the compiler did not emit the expected loop: " +
                 std::to_string(m.fma_instructions) + " FMAs were requested but only " +
                 std::to_string(m.instructions_retired) +
                 " instructions retired; the measurement would be meaningless";
        return m;
    }
    m.fmas_per_cycle = static_cast<double>(m.fma_instructions) /
                       static_cast<double>(m.cycles);
    m.units = m.fmas_per_cycle;
    m.rounded = static_cast<int>(m.fmas_per_cycle + 0.5);
    if (m.rounded < 1) m.rounded = 1;
    m.ok = true;
    return m;
#endif
}

}  // namespace ppe::probe
