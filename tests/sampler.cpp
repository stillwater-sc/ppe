// test_sampler -- assertions about the sampling profiler.
//
// A profiler is checkable in a way most measurements are not: give it a workload
// with a KNOWN ratio and see whether it recovers it. This runs two functions
// with a deliberate 4:1 work ratio and asserts the profile reflects that -- which
// is a far stronger check than "samples were collected", and it is the check the
// first version of this code would have failed.
//
// That version symbolized 0% (dladdr sees only dynamic symbols) and merged
// unresolved addresses by 4 KiB page, so both functions landed in one entry and
// the ratio was invisible. It looked like a working profiler with one hot spot.

#include <ppe/cli.hpp>
#include <ppe/detect/affinity.hpp>
#include <ppe/probe/sampler.hpp>

#include <cmath>
#include <cstdio>
#include <string>

#if defined(__linux__)
#  include <sched.h>
#endif

// noinline is spelled differently by MSVC. C++ has no portable attribute for it
// -- [[gnu::noinline]] is as compiler-specific as the underscore form -- so the
// spelling is selected here rather than assumed.
#if defined(_MSC_VER)
#  define PPE_TEST_NOINLINE __declspec(noinline)
#else
#  define PPE_TEST_NOINLINE __attribute__((noinline))
#endif

namespace {

int failures = 0;

void expect_true(const char* what, bool ok) {
    if (!ok) ++failures;
    std::printf("  %-54s %s\n", what, ok ? "ok" : "FAIL");
}

// noinline so they remain distinct symbols; the ratio is the assertion.
PPE_TEST_NOINLINE double busy_hot(double x, int n) {
    double s = 0;
    for (int i = 0; i < n * 4; ++i) s += std::sqrt(x + i);
    return s;
}

PPE_TEST_NOINLINE double busy_cold(double x, int n) {
    double s = 0;
    for (int i = 0; i < n; ++i) s += std::sqrt(x + i);
    return s;
}

std::uint64_t samples_for(const ppe::probe::profile_result& p, const char* needle) {
    std::uint64_t n = 0;
    for (const auto& site : p.sites) {
        if (site.symbol.find(needle) != std::string::npos) n += site.samples;
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        std::printf("test_sampler -- verify the sampling profiler recovers a known ratio\n");
        return 0;
    }

    // PIN FIRST. A perf event is bound to the PMU of the CPU it was opened on,
    // and an unpinned thread on a hybrid part can migrate to a core the other
    // PMU owns, where sampling silently stops. Unpinned, this test collected
    // ~670 samples most runs and ~30 on the rest, with the colder function
    // vanishing entirely -- a profile that looked thin rather than broken.
#if defined(__linux__)
    const int cpu = ::sched_getcpu();
    if (cpu >= 0) ppe::pin_current_thread(cpu);
#endif

    ppe::probe::sampler s(4000);
    if (!s.ok()) {
        std::printf("sampler unavailable: %s\n", s.note().c_str());
        std::printf("\nContract when unavailable:\n");
        expect_true("an unavailable sampler states why", !s.note().empty());
        const ppe::probe::profile_result p = s.collect();
        expect_true("collect() on an unavailable sampler is not ok", !p.ok);
        expect_true("and reports no samples", p.samples_collected == 0);
        std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
        return failures == 0 ? 0 : 1;
    }

    s.start();
    volatile double acc = 0;
    for (int r = 0; r < 20; ++r) {
        acc += busy_hot(1.0, 1000000);
        acc += busy_cold(1.0, 1000000);
    }
    s.stop();
    (void)acc;

    const ppe::probe::profile_result p = s.collect();
    std::printf("samples=%llu lost=%llu symbolized=%.0f%%\n",
                static_cast<unsigned long long>(p.samples_collected),
                static_cast<unsigned long long>(p.samples_lost),
                p.symbolized_fraction * 100.0);

    std::printf("\nContract:\n");
    expect_true("collection succeeded", p.ok);
    // Pinned above, so this must not trigger; if it does, the profile below is
    // truncated and its ratios mean nothing.
    expect_true("the thread did not cross core types while sampling",
                !p.migrated_across_pmus);
    expect_true("samples were collected", p.samples_collected > 100);

    // A profile that resolved nothing is indistinguishable from one with no hot
    // spots, so the fraction is asserted rather than merely reported.
    expect_true("most samples resolved to a symbol name",
                p.symbolized_fraction > 0.5);

    const std::uint64_t hot = samples_for(p, "busy_hot");
    const std::uint64_t cold = samples_for(p, "busy_cold");
    std::printf("  busy_hot=%llu busy_cold=%llu\n",
                static_cast<unsigned long long>(hot),
                static_cast<unsigned long long>(cold));

    expect_true("both functions appear separately", hot > 0 && cold > 0);
    if (cold > 0) {
        const double ratio = static_cast<double>(hot) / static_cast<double>(cold);
        // The work ratio is 4:1. A generous band, because this is a sampled
        // statistic on a shared machine -- but narrow enough to fail if the two
        // functions were merged (ratio would be 0 or undefined) or misattributed.
        expect_true("the 4:1 work ratio is recovered (2.5 to 6)",
                    ratio > 2.5 && ratio < 6.0);
    }

    // Lost samples are not a failure, but they must be visible: a profile with
    // invisible holes is weighted toward whatever was hottest.
    if (p.samples_lost > 0) {
        std::printf("  note: %llu samples lost (buffer pressure)\n",
                    static_cast<unsigned long long>(p.samples_lost));
    }

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
