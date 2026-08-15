// test_trace -- assertions about the recorder that do not depend on the machine.
//
// Timings are deliberately NOT asserted on: a duration is a property of the
// runner, and asserting one on shared CI hardware is a flake generator. What is
// asserted is the bookkeeping -- that events are retained, that drops are
// counted rather than silently swallowed, that a disabled recorder records
// nothing, and that threads get separate lanes. Those have right answers.

#include <ppe/cli.hpp>
#include <ppe/trace.hpp>

#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(const char* what, long long got, long long want) {
    const bool ok = (got == want);
    if (!ok) ++failures;
    std::printf("  %-42s got %6lld  want %6lld  %s\n", what, got, want,
                ok ? "ok" : "FAIL");
}

void expect_true(const char* what, bool ok) {
    if (!ok) ++failures;
    std::printf("  %-42s %s\n", what, ok ? "ok" : "FAIL");
}

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        std::printf("test_trace -- verify the trace recorder's bookkeeping\n");
        return 0;
    }

    // -- Disabled by default ---------------------------------------------
    // A tracer that records before anyone asked would perturb every
    // measurement in the repository by default.
    {
        ppe::trace::recorder r(16);
        { ppe::trace::scope s("ignored", "test", r); }
        const auto st = r.collect_stats();
        std::printf("Disabled recorder:\n");
        expect("records nothing when disabled", static_cast<long long>(st.recorded), 0);
        expect("drops nothing when disabled", static_cast<long long>(st.dropped), 0);
    }

    // -- Records when enabled --------------------------------------------
    {
        ppe::trace::recorder r(16);
        r.enable();
        for (int i = 0; i < 5; ++i) {
            ppe::trace::scope s("span", "test", r);
        }
        const auto st = r.collect_stats();
        std::printf("\nEnabled recorder:\n");
        expect("five spans retained", static_cast<long long>(st.recorded), 5);
        expect("nothing dropped under capacity", static_cast<long long>(st.dropped), 0);
        expect("one thread lane", static_cast<long long>(st.threads), 1);
    }

    // -- Overflow is counted, not silent ---------------------------------
    // The property that matters: recorded + dropped accounts for every span
    // offered. A trace with silent gaps produces a schedule view with
    // invisible holes.
    {
        constexpr int kCapacity = 8;
        constexpr int kOffered = 20;
        ppe::trace::recorder r(kCapacity);
        r.enable();
        for (int i = 0; i < kOffered; ++i) {
            ppe::trace::scope s("span", "test", r);
        }
        const auto st = r.collect_stats();
        std::printf("\nOverflow (%d offered into capacity %d):\n", kOffered, kCapacity);
        expect("retained equals capacity", static_cast<long long>(st.recorded),
               kCapacity);
        expect("dropped equals the remainder",
               static_cast<long long>(st.dropped), kOffered - kCapacity);
        expect("every offered span is accounted for",
               static_cast<long long>(st.recorded + st.dropped), kOffered);
    }

    // -- Threads get separate lanes --------------------------------------
    // Per-thread buffers are what keeps the recorder off a lock on the hot
    // path; separate lanes are the observable consequence.
    {
        constexpr int kThreads = 4;
        constexpr int kPerThread = 10;
        ppe::trace::recorder r(1024);
        r.enable();

        std::vector<std::thread> pool;
        for (int t = 0; t < kThreads; ++t) {
            pool.emplace_back([&r] {
                r.name_thread("worker");
                for (int i = 0; i < kPerThread; ++i) {
                    ppe::trace::scope s("work", "test", r);
                }
            });
        }
        for (auto& th : pool) th.join();

        const auto st = r.collect_stats();
        std::printf("\nConcurrent recording (%d threads x %d spans):\n", kThreads,
                    kPerThread);
        expect("all spans retained", static_cast<long long>(st.recorded),
               kThreads * kPerThread);
        expect("nothing dropped", static_cast<long long>(st.dropped), 0);
        expect("one lane per thread", static_cast<long long>(st.threads), kThreads);
    }

    // -- Nesting -----------------------------------------------------------
    // Nested scopes must all be retained; the viewer reconstructs the stack
    // from the timestamps.
    {
        ppe::trace::recorder r(64);
        r.enable();
        {
            ppe::trace::scope outer("outer", "test", r);
            {
                ppe::trace::scope middle("middle", "test", r);
                { ppe::trace::scope inner("inner", "test", r); }
            }
        }
        const auto st = r.collect_stats();
        std::printf("\nNesting:\n");
        expect("three nested spans retained", static_cast<long long>(st.recorded), 3);
    }

    // -- Export ------------------------------------------------------------
    {
        ppe::trace::recorder r(8);
        r.enable();
        r.name_thread("exporter");
        for (int i = 0; i < 12; ++i) {  // 8 kept, 4 dropped
            ppe::trace::scope s("span", "test", r);
        }
        const ppe::provenance prov = ppe::collect_provenance();
        ppe::trace::recorder::stats st;
        const bool ok = r.write_chrome_json("ppe_test_trace.json", prov, &st);

        std::printf("\nExport:\n");
        expect_true("write_chrome_json succeeds", ok);
        expect("export reports the drops", static_cast<long long>(st.dropped), 4);

        // The file must actually contain the drop count: a caller that ignores
        // the return value must still be able to see the trace is truncated.
        bool found_dropped = false;
        if (std::FILE* f = std::fopen("ppe_test_trace.json", "r"); f != nullptr) {
            char buf[8192];
            const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            found_dropped = std::string(buf).find("\"events_dropped\": 4") !=
                            std::string::npos;
            std::fclose(f);
        }
        expect_true("drop count is written into the trace file", found_dropped);
        std::remove("ppe_test_trace.json");
    }

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
