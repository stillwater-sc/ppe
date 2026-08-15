// blocking_study -- how does GEMM tiling interact with the cache hierarchy?
//
// PLACEHOLDER. It sweeps one blocking parameter over a naive ijk GEMM and
// prints achieved GFLOP/s, which is the shape of the real study but not the
// study: the block sizes are hardcoded rather than derived from the detected
// cache hierarchy, only one kernel is measured, and there is no repetition,
// warm-up, outlier rejection, or provenance record.
//
// What it demonstrates is the intended structure -- a study is an executable
// that emits a table, and the numbers it emits are only as good as the machine
// model behind them. Wiring the sweep to ppe::detect_cpu() cache sizes is the
// first real step (see include/ppe/platform.hpp).
//
// Not run by CI: real numbers need pinned cores and a quiet machine.

#include <ppe/cli.hpp>
#include <ppe/platform.hpp>
#include <ppe/version.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {

void print_help() {
    std::printf(
        "blocking_study -- GEMM blocking sweep (PPE %s)\n"
        "\n"
        "Usage: blocking_study [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help     show this help and exit\n"
        "      --size N   square matrix dimension (default 256)\n"
        "\n"
        "PLACEHOLDER: hardcoded block sizes, single kernel, single trial. Pin to\n"
        "a performance core (taskset) before believing any number it prints.\n",
        ppe::version_string);
}

// Blocked ijk GEMM: C += A * B, all row-major, C pre-zeroed by the caller.
void gemm_blocked(const std::vector<double>& a, const std::vector<double>& b,
                  std::vector<double>& c, int n, int block) {
    for (int ii = 0; ii < n; ii += block) {
        const int i_end = (ii + block < n) ? ii + block : n;
        for (int kk = 0; kk < n; kk += block) {
            const int k_end = (kk + block < n) ? kk + block : n;
            for (int jj = 0; jj < n; jj += block) {
                const int j_end = (jj + block < n) ? jj + block : n;
                for (int i = ii; i < i_end; ++i) {
                    for (int k = kk; k < k_end; ++k) {
                        const double aik = a[static_cast<std::size_t>(i) * n + k];
                        for (int j = jj; j < j_end; ++j) {
                            c[static_cast<std::size_t>(i) * n + j] +=
                                aik * b[static_cast<std::size_t>(k) * n + j];
                        }
                    }
                }
            }
        }
    }
}

int parse_size(int argc, char** argv, int fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view("--size") == argv[i]) {
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

    const int n = parse_size(argc, argv, 256);
    const ppe::device_attributes cpu = ppe::detect_cpu();

    std::printf("PPE %s -- GEMM blocking sweep (PLACEHOLDER)\n", ppe::version_string);
    std::printf("device : %s\n", cpu.name.c_str());
    std::printf("compiler: %s, ISA baseline: %s\n", ppe::build_compiler(), ppe::build_isa());
    std::printf("size   : %d x %d\n\n", n, n);

    const std::size_t elements = static_cast<std::size_t>(n) * n;
    std::vector<double> a(elements, 1.0);
    std::vector<double> b(elements, 1.0);
    std::vector<double> c(elements);

    // 2*n^3 flops for a square GEMM: one multiply and one add per inner
    // iteration.
    const double flops = 2.0 * static_cast<double>(n) * n * n;

    std::printf("%-8s %12s %12s\n", "block", "seconds", "GFLOP/s");
    for (const int block : {16, 32, 64, 128, 256}) {
        if (block > n) continue;
        std::fill(c.begin(), c.end(), 0.0);

        const auto start = std::chrono::steady_clock::now();
        gemm_blocked(a, b, c, n, block);
        const auto stop = std::chrono::steady_clock::now();

        const double seconds = std::chrono::duration<double>(stop - start).count();
        std::printf("%-8d %12.6f %12.2f\n", block, seconds,
                    seconds > 0.0 ? flops / seconds / 1e9 : 0.0);
    }

    std::printf(
        "\nNOTE: placeholder measurement -- single trial, no warm-up, block sizes\n"
        "      not derived from the detected cache hierarchy.\n");
    return 0;
}
