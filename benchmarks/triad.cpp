// triad -- STREAM-style memory bandwidth microbenchmark.
//
// PLACEHOLDER. a[i] = b[i] + scalar * c[i] over arrays large enough to miss
// cache, reporting GB/s. The shape is right; the rigor is not: single thread,
// no NUMA placement, no page-touch policy, no repetition statistics, and the
// working-set size is a hardcoded default rather than one chosen from the
// detected last-level cache size.
//
// Wiring the array size to ppe::detect_cpu().l3_bytes is the point at which
// this stops being a placeholder -- a bandwidth number is meaningless without
// knowing the working set sits outside the cache that would otherwise serve it.
//
// Never timed in CI: shared runners cannot provide pinned cores or a quiet
// machine, so a bandwidth gate built on them measures the neighbours.

#include <ppe/cli.hpp>
#include <ppe/harness.hpp>
#include <ppe/platform.hpp>
#include <ppe/provenance.hpp>
#include <ppe/version.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {

void print_help() {
    std::printf(
        "triad -- STREAM-style memory bandwidth benchmark (PPE %s)\n"
        "\n"
        "Usage: triad [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help      show this help and exit\n"
        "      --mib N     working set per array, in MiB (default 64)\n"
        "      --iters N   timed iterations (default 5)\n"
        "      --json      emit the provenance record as JSON and exit\n"
        "\n"
        "PLACEHOLDER: single-threaded, no NUMA placement, working-set size not\n"
        "derived from the detected cache hierarchy. Pin to a core (taskset)\n"
        "before believing any number it prints.\n",
        ppe::version_string);
}

int parse_int(int argc, char** argv, std::string_view flag, int fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (flag == argv[i]) {
            const int v = std::atoi(argv[i + 1]);
            if (v > 0) return v;
        }
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        print_help();
        return 0;
    }

    const ppe::provenance prov = ppe::collect_provenance();
    if (ppe::has_flag(argc, argv, "--json")) {
        std::fputs(ppe::to_json(prov).c_str(), stdout);
        return 0;
    }

    const int mib = parse_int(argc, argv, "--mib", 64);
    const int iters = parse_int(argc, argv, "--iters", 5);

    const std::size_t bytes = static_cast<std::size_t>(mib) * 1024u * 1024u;
    const std::size_t n = bytes / sizeof(double);

    std::fputs(ppe::to_text(prov).c_str(), stdout);
    std::printf("arrays  : 3 x %d MiB (%zu elements each)\n\n", mib, n);

    std::vector<double> a(n, 0.0);
    std::vector<double> b(n, 1.0);
    std::vector<double> c(n, 2.0);
    const double scalar = 3.0;

    // Triad moves 3 arrays per iteration: read b, read c, write a. The write is
    // counted once; on a write-allocate machine without non-temporal stores the
    // true traffic is higher, which is one reason this is a placeholder.
    const double moved_bytes = 3.0 * static_cast<double>(n) * sizeof(double);

    // ppe::time_median warms up once (first touch faults the pages in, and
    // timing that measures the allocator) and reports the median rather than
    // the mean: on any machine that is not perfectly quiet the distribution has
    // a long right tail from scheduling, and a mean reports the interference.
    const double seconds = ppe::time_median(
        [&] {
            for (std::size_t i = 0; i < n; ++i) {
                a[i] = b[i] + scalar * c[i];
            }
        },
        static_cast<std::size_t>(iters));

    const double gbs = seconds > 0.0 ? moved_bytes / seconds / 1e9 : 0.0;

    std::printf("%-12s %12s %12s\n", "reps", "median s", "GB/s");
    std::printf("%-12d %12.6f %12.2f\n", iters, seconds, gbs);

    // Consume the result so the loop above cannot be optimized away entirely.
    std::printf("\nchecksum %.1f\n", a[n / 2]);
    std::printf(
        "NOTE: placeholder measurement -- single-threaded, no NUMA placement,\n"
        "      working set not sized against the detected last-level cache.\n");
    return 0;
}
