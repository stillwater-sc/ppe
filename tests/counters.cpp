// test_counters -- assertions about the hardware-counter backend.
//
// The SUCCESS path cannot be tested here: this development machine reports
// perf_event_paranoid=4, which denies unprivileged counters outright, and no
// test may change a kernel setting to make itself pass. So what is asserted is
// everything that must hold on a machine that REFUSES -- which is the common
// case, and the one where a silent zero would be worst.
//
// The API is also asserted to exist unconditionally. Burying it behind
// #if defined(__linux__) would make this file compile on Linux alone, which is
// the bug tools/lint/platform_includes.py was extended to catch after it broke
// four CI jobs.

#include <ppe/cli.hpp>
#include <ppe/detect/clock.hpp>
#include <ppe/probe/counters.hpp>

#include <cstdio>

namespace {

int failures = 0;

void expect_true(const char* what, bool ok) {
    if (!ok) ++failures;
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
}

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        std::printf("test_counters -- verify the perf_event backend's contract\n");
        return 0;
    }

    const ppe::probe::counter_support sup = ppe::probe::counters_available();
    std::printf("This machine: counters %s (paranoid=%d)\n",
                sup.available ? "AVAILABLE" : "denied", sup.paranoid);
    if (!sup.note.empty()) std::printf("  %s\n", sup.note.c_str());

    std::printf("\nContract that holds either way:\n");
    // Availability and the reason are mutually exclusive: a caller deciding
    // whether to trust a number must never see both empty.
    expect_true("unavailable implies a stated reason",
                sup.available || !sup.note.empty());
    expect_true("available implies no complaint",
                !sup.available || sup.note.empty());

    {
        ppe::probe::cycle_counter c;
        expect_true("ok() agrees with counters_available()", c.ok() == sup.available);
        // start/stop on a refused counter must be harmless, not undefined: the
        // caller has no obligation to check ok() first.
        c.start();
        const std::uint64_t cycles = c.stop();
        if (!c.ok()) {
            expect_true("a refused counter reports zero cycles, not garbage",
                        cycles == 0);
        } else {
            expect_true("an open counter counts something", cycles > 0);
        }
    }

    std::string why;
    const double measured = ppe::probe::measure_clock_ghz(0.02, &why);
    if (measured > 0.0) {
        // A plausible range rather than a value: this is the runner's clock.
        expect_true("a measured clock is physically plausible (0.1-10 GHz)",
                    measured > 0.1 && measured < 10.0);
    } else {
        // Zero is legitimate for two reasons -- counters denied, or the thread
        // crossed core types mid-measurement -- and both must be stated. What is
        // never acceptable is a zero with no explanation.
        expect_true("an unmeasured clock is 0 with a stated reason", !why.empty());
    }
    if (!sup.available) {
        expect_true("denied counters never yield a measurement", measured == 0.0);
    }

    std::printf("\nbest_clock() falls back rather than reporting nothing:\n");
    const ppe::clock_reading r = ppe::best_clock(0.02);
    // ONE DIRECTION ONLY. Counters being available does not guarantee a
    // measurement: on a hybrid part an unpinned thread can migrate between PMU
    // domains mid-measurement, and the probe correctly refuses rather than
    // reporting a core it did not run on. Asserting equality made this test
    // flaky under load -- it failed once in a full-suite run and never
    // reproduced in 24 attempts, which is exactly the profile of a race.
    expect_true("a measured clock implies counters were available",
                !r.measured || sup.available);
    expect_true("a fallback names its source", r.measured || !r.source.empty());
    expect_true("a fallback explains why it fell back",
                r.measured || !r.note.empty());
    // The one thing that must never happen: a clock reported as measured that
    // was not, or a measured clock with no value.
    expect_true("measured implies a value", !r.measured || r.ghz > 0.0);

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
