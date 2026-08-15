// test_peak_model -- the derived peak model must reproduce the hand-written one.
//
// ppe/peak.hpp generalizes mtl5/ppe/include/ppe/peak.hpp: it derives the lane
// count from the detected vector width instead of hardcoding one
// microarchitecture. A generalization is only correct if it still produces the
// original numbers on the original target, and this asserts exactly that
// against the table documented in the source material:
//
//     fp64  16   fp32  32   fp16  32
//     int8  32   int16 32   int32 16   int64 8      (Alder Lake P-core, AVX2+FMA)
//
// This test exists because the first version of the derived model FAILED it. It
// applied the FMA-unit multiplier to the integer rows as well as the floating-
// point ones, producing int32 = 8 where the source says 16 and int16 = 64 where
// the source says 32. A part with two FMA pipes does not have two vector integer
// multipliers, and the error would have made every integer kernel's measured
// efficiency wrong by 2x in one direction or the other -- silently, since a
// plausible percentage is indistinguishable from a correct one.

#include <ppe/cli.hpp>
#include <ppe/peak.hpp>

#include <cstdint>
#include <cstdio>

namespace {

/// The mtl5/ppe target the constants were written for.
ppe::isa_capabilities alder_lake_p_core() {
    ppe::isa_capabilities isa;
    isa.sse2 = isa.avx = isa.avx2 = isa.fma = true;
    isa.vector_bits = 256;
    isa.name = "x86-64 AVX2+FMA";
    return isa;
}

int failures = 0;

void expect(const char* what, double got, double want) {
    const bool ok = (got == want);
    if (!ok) ++failures;
    std::printf("  %-8s got %6.1f  want %6.1f  %s\n", what, got, want,
                ok ? "ok" : "FAIL");
}

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        std::printf("test_peak_model -- verify the derived peak model\n");
        return 0;
    }

    // 1 GHz makes peak_gops read directly as ops/cycle.
    const ppe::peak_model m = ppe::make_peak_model(alder_lake_p_core(), 2, 1.0);

    std::printf("Alder Lake P-core (AVX2+FMA, 256-bit, 2 units), ops/cycle:\n");
    expect("fp64",  ppe::peak_gops<double>(m),       16.0);
    expect("fp32",  ppe::peak_gops<float>(m),        32.0);
    expect("int8",  ppe::peak_gops<std::int8_t>(m),  32.0);
    expect("int16", ppe::peak_gops<std::int16_t>(m), 32.0);
    expect("int32", ppe::peak_gops<std::int32_t>(m), 16.0);
    expect("int64", ppe::peak_gops<std::int64_t>(m),  8.0);

    // Doubling the vector width doubles every lane count, and nothing else.
    ppe::isa_capabilities wide = alder_lake_p_core();
    wide.avx512f = true;
    wide.vector_bits = 512;
    const ppe::peak_model m512 = ppe::make_peak_model(wide, 2, 1.0);

    std::printf("\nSame part at 512-bit vectors:\n");
    expect("fp64",  ppe::peak_gops<double>(m512),       32.0);
    expect("fp32",  ppe::peak_gops<float>(m512),        64.0);
    expect("int32", ppe::peak_gops<std::int32_t>(m512), 32.0);
    expect("int64", ppe::peak_gops<std::int64_t>(m512), 16.0);

    // A part without FMA loses the 2-ops-per-instruction factor on FP only.
    ppe::isa_capabilities no_fma = alder_lake_p_core();
    no_fma.fma = false;
    const ppe::peak_model m_nofma = ppe::make_peak_model(no_fma, 2, 1.0);

    std::printf("\nSame width without FMA (FP halves, integer unchanged):\n");
    expect("fp64",  ppe::peak_gops<double>(m_nofma),        8.0);
    expect("fp32",  ppe::peak_gops<float>(m_nofma),        16.0);
    expect("int32", ppe::peak_gops<std::int32_t>(m_nofma), 16.0);

    // One unit halves the FP rows and leaves the integer rows alone. This is the
    // asymmetry the original bug erased.
    const ppe::peak_model m1 = ppe::make_peak_model(alder_lake_p_core(), 1, 1.0);

    std::printf("\nOne FMA unit (FP halves, integer unchanged):\n");
    expect("fp64",  ppe::peak_gops<double>(m1),        8.0);
    expect("fp32",  ppe::peak_gops<float>(m1),        16.0);
    expect("int16", ppe::peak_gops<std::int16_t>(m1), 32.0);
    expect("int32", ppe::peak_gops<std::int32_t>(m1), 16.0);

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
