// roofline -- where compute peak meets measured bandwidth.
//
// The roofline model says an achievable rate is bounded by
//
//     min(compute_peak,  arithmetic_intensity * memory_bandwidth)
//
// and the interesting number is the RIDGE POINT -- the arithmetic intensity at
// which the two bounds cross. Below it a kernel is memory bound and adding FLOPs
// per byte helps; above it, it is compute bound and only more compute does.
//
// The two halves of this have very different provenance, and the report says so
// on every line:
//
//   compute peak   MODELLED. Derived vector width and FMA-vs-no-FMA, times an
//                  FMA-unit count that no instruction reports, times an
//                  OS-CLAIMED clock. See ppe/peak.hpp and ppe/detect/clock.hpp.
//   bandwidth      MEASURED, here, now, by the same streaming probe that
//                  applications/memory_hierarchy uses, at a working set past the
//                  last-level cache.
//
// A ridge point built from a modelled numerator and a measured denominator is
// only as good as the model, and the model's weakest input is the FMA-unit
// count. Override it with --fma-units when you know the part; the value used is
// printed either way.

#include <ppe/cli.hpp>
#include <ppe/detect/clock.hpp>
#include <ppe/detect/cpu.hpp>
#include <ppe/detect/isa.hpp>
#include <ppe/harness.hpp>
#include <ppe/peak.hpp>
#include <ppe/probe/fma.hpp>
#include <ppe/platform.hpp>
#include <ppe/provenance.hpp>
#include <ppe/version.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kMiB = 1024u * 1024u;

template <typename T>
void keep(T value) {
    static volatile T sink;
    sink = value;
}

void print_help() {
    std::printf(
        "roofline -- modelled compute peak against measured bandwidth (PPE %s)\n"
        "\n"
        "Usage: roofline [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help          show this help and exit\n"
        "      --fma-units N   vector/FMA issue units per core (default 2)\n"
        "      --ghz X         override the OS-claimed core clock\n"
        "      --mib N         working set for the bandwidth probe (default 128)\n"
        "      --json          emit the provenance record as JSON and exit\n"
        "\n"
        "The compute ceiling is a MODEL and the bandwidth is a MEASUREMENT. Pin\n"
        "the process before believing the bandwidth, and check the FMA-unit count\n"
        "against the part before believing the ceiling.\n",
        ppe::version_string);
}

/// Streaming-read bandwidth at a working set past the last-level cache. Same
/// probe as applications/memory_hierarchy, so the two agree by construction.
double measure_bandwidth_gbs(std::size_t bytes) {
    const std::size_t n = bytes / sizeof(double);
    if (n < 64) return 0.0;

    std::vector<double> a(n, 1.0);
    double sink = 0.0;

    // Four independent accumulators: a single one serializes on the adder's
    // latency and measures the FPU rather than memory.
    const double seconds = ppe::time_median(
        [&] {
            double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
            for (std::size_t i = 0; i + 3 < n; i += 4) {
                s0 += a[i];
                s1 += a[i + 1];
                s2 += a[i + 2];
                s3 += a[i + 3];
            }
            sink += s0 + s1 + s2 + s3;
        },
        3);

    keep(sink);
    return seconds > 0.0 ? static_cast<double>(bytes) / seconds / 1e9 : 0.0;
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

double parse_double(int argc, char** argv, std::string_view flag, double fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (flag == argv[i]) {
            const double v = std::atof(argv[i + 1]);
            if (v > 0.0) return v;
        }
    }
    return fallback;
}

struct type_row {
    const char* label;
    double peak_gops;
};

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

    const ppe::isa_capabilities isa = ppe::detect_isa();
    // Measured when the kernel permits hardware counters, the OS claim
    // otherwise. The peak model multiplies by this number, so whether it was
    // counted or asserted changes what every GOP/s figure below means.
    const ppe::clock_reading clk = ppe::best_clock();

    // MEASURED where possible. This was the peak model's last assumed factor,
    // and it is wrong by 2x on an E-core: Golden Cove issues 2 FMAs/cycle,
    // Gracemont 1. An explicit --fma-units still wins, for reproducing a figure
    // from another machine.
    const ppe::probe::fma_measurement fma = ppe::probe::measure_fma_units();
    const int fma_override = parse_int(argc, argv, "--fma-units", 0);
    const unsigned fma_units =
        fma_override > 0 ? static_cast<unsigned>(fma_override)
                         : (fma.ok ? static_cast<unsigned>(fma.rounded) : 2u);
    const double ghz = parse_double(argc, argv, "--ghz", clk.ghz);
    const std::size_t bytes =
        static_cast<std::size_t>(parse_int(argc, argv, "--mib", 128)) * kMiB;

    std::fputs(ppe::to_text(prov).c_str(), stdout);

    // Both ISAs are reported. The machine's is the ceiling for the hardware;
    // the build's is the ceiling for THIS binary, and a kernel compiled for a
    // narrower baseline cannot reach the wider peak no matter what the silicon
    // offers.
    std::printf("machine ISA : %s (%u-bit vectors%s)\n", isa.name.c_str(),
                isa.vector_bits, isa.fma || isa.neon ? ", FMA" : ", no FMA");
    std::printf("build ISA   : %s\n", ppe::build_isa());
    if (isa.vector_bits > 0 && std::string_view(ppe::build_isa()) != isa.name) {
        std::printf(
            "              ^ build and machine differ: this binary cannot reach the\n"
            "                machine ceiling below. Rebuild with the release preset\n"
            "                (PPE_NATIVE_ARCH=ON) to close the gap.\n");
    }

    if (ghz <= 0.0) {
        std::printf(
            "\nerror: no core clock available (%s) and no --ghz given.\n"
            "       Apple silicon exposes no core frequency through any public\n"
            "       interface; pass --ghz explicitly.\n",
            clk.source.empty() ? "no backend" : clk.source.c_str());
        return 2;
    }

    if (clk.measured) {
        std::printf("clock       : %.3f GHz (perf_event) -- MEASURED sustained clock\n",
                    ghz);
    } else {
        std::printf("clock       : %.3f GHz (%s) -- a CLAIM, not a sustained measurement\n",
                    ghz, clk.source.c_str());
        if (!clk.note.empty()) {
            std::printf("              %s\n", clk.note.c_str());
        }
    }
    if (fma_override > 0) {
        std::printf("fma units   : %u (given on the command line)\n\n", fma_units);
    } else if (fma.ok) {
        std::printf("fma units   : %u (MEASURED: %.3f FMA/cycle over %llu cycles)\n\n",
                    fma_units, fma.fmas_per_cycle,
                    static_cast<unsigned long long>(fma.cycles));
    } else {
        std::printf("fma units   : %u (assumed -- %s)\n\n", fma_units,
                    fma.note.c_str());
    }

    const ppe::peak_model model = ppe::make_peak_model(isa, fma_units, ghz);

    std::printf("Measuring streaming bandwidth at %zu MiB ...\n", bytes / kMiB);
    const double bw = measure_bandwidth_gbs(bytes);
    if (bw <= 0.0) {
        std::printf("error: bandwidth probe produced no result\n");
        return 1;
    }
    std::printf("bandwidth   : %.2f GB/s (MEASURED, single thread, unpinned unless"
                " you pinned it)\n\n", bw);

    const type_row rows[] = {
        {"fp64",  ppe::peak_gops<double>(model)},
        {"fp32",  ppe::peak_gops<float>(model)},
        {"int32", ppe::peak_gops<std::int32_t>(model)},
        {"int16", ppe::peak_gops<std::int16_t>(model)},
        {"int8",  ppe::peak_gops<std::int8_t>(model)},
        {"int64", ppe::peak_gops<std::int64_t>(model)},
    };

    std::printf("%-8s %14s %18s\n", "type", "peak GOP/s", "ridge (op/byte)");
    for (const type_row& r : rows) {
        std::printf("%-8s %14.1f %18.2f\n", r.label, r.peak_gops, r.peak_gops / bw);
    }

    std::printf("\n%s\n", ppe::describe(model).c_str());
    std::printf(
        "\nRidge point = modelled peak / measured bandwidth. A kernel below its\n"
        "type's ridge is memory bound: more FLOPs per byte moved will help it, and\n"
        "a faster core will not. Above it, the reverse.\n"
        "\nThe compute column is a MODEL whose weakest input is the FMA-unit count\n"
        "(%u here) and whose clock is claimed rather than sustained. The bandwidth\n"
        "column is measured. Treat a measured rate above 100%% of a modelled peak\n"
        "as evidence the model is wrong, not as a superhuman kernel.\n",
        fma_units);

    return 0;
}
