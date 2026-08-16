// test_fma -- assertions about the FMA issue-width probe.
//
// The probe divides a KNOWN instruction count by measured cycles, so its whole
// validity rests on the compiler emitting the loop that was written. It did not,
// the first time: twelve accumulators initialised to the same value are provably
// equal at every step, so the compiler collapsed all twelve chains into one. The
// disassembly held two vfmadd231pd, not twelve. The probe divided a 12x-too-large
// count by real cycles and reported 2.9992 FMA/cycle on a part that issues 2 --
// a clean-looking number that was actually FMA latency, 0.25/cycle, scaled by 12.
//
// So the probe now counts retired instructions as well, and these assert that
// self-check rather than the FMA count itself: the count is a property of the
// silicon, and asserting 2 would fail on the E-core beside it, which issues 1.

#include <ppe/cli.hpp>
#include <ppe/probe/fma.hpp>

#include <cstdio>

namespace {

int failures = 0;

void expect_true(const char* what, bool ok) {
    if (!ok) ++failures;
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
}

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        std::printf("test_fma -- verify the FMA issue-width probe's contract\n");
        return 0;
    }

    const ppe::probe::fma_measurement m = ppe::probe::measure_fma_units(200000);

    std::printf("This machine: %s\n", m.ok ? "measured" : "not measured");
    if (m.ok) {
        std::printf("  %.4f FMA/cycle over %llu cycles, %llu instructions retired\n",
                    m.fmas_per_cycle, static_cast<unsigned long long>(m.cycles),
                    static_cast<unsigned long long>(m.instructions_retired));
    } else {
        std::printf("  %s\n", m.note.c_str());
    }

    std::printf("\nContract:\n");
    expect_true("not measured implies a stated reason", m.ok || !m.note.empty());
    expect_true("measured implies no complaint", !m.ok || m.note.empty());

    if (m.ok) {
        // A range, not a value: 2 on Golden Cove, 1 on Gracemont, 4 on some
        // Apple cores. Asserting a number would fail on the core beside this one.
        expect_true("FMA/cycle is physically plausible (0.2 to 8)",
                    m.fmas_per_cycle > 0.2 && m.fmas_per_cycle < 8.0);
        expect_true("rounded unit count is at least 1", m.rounded >= 1);

        // THE CHECK THAT MATTERS. Retired instructions must cover the FMAs the
        // loop was written to issue; materially fewer means the compiler removed
        // work and the ratio is meaningless. This is the assertion the original
        // bug would have failed.
        if (m.instructions_retired > 0) {
            expect_true("retired instructions cover the requested FMAs",
                        m.instructions_retired >= m.fma_instructions);
            // Loop control adds a few per iteration; an enormous excess would
            // mean the kernel is doing something other than FMAs.
            expect_true("retired instructions are not wildly more than requested",
                        m.instructions_retired < m.fma_instructions * 3);
        }
    } else {
        expect_true("an unmeasured probe reports no unit count", m.rounded == 0);
        expect_true("an unmeasured probe reports no rate", m.fmas_per_cycle == 0.0);
    }

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
