// topology -- report the performance-relevant attributes of this machine.
//
// PLACEHOLDER. Reports only what ppe/platform.hpp can portably discover today;
// the per-platform CPU/GPU/KPU backends described there are the real work. It
// exists now so the build, the presets and the CI lanes have a tool-shaped
// target to compile and smoke-test.
//
// The text output is for humans; --json is the machine-readable form other PPE
// stages consume. Both are interfaces once anything depends on them.

#include <ppe/cli.hpp>
#include <ppe/detect/accelerator.hpp>
#include <ppe/detect/clock.hpp>
#include <ppe/detect/isa.hpp>
#include <ppe/detect/topology.hpp>
#include <ppe/report/topology_report.hpp>
#include <ppe/platform.hpp>
#include <ppe/provenance.hpp>
#include <ppe/version.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void print_help() {
    std::printf(
        "topology -- report platform performance attributes (PPE %s)\n"
        "\n"
        "Usage: topology [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help       show this help and exit\n"
        "      --json       emit the attribute set as JSON\n"
        "      --csv-header emit the provenance as CSV comment lines\n"
        "      --ascii      draw the cluster topology as an ASCII tree\n"
        "      --html PATH  write a self-contained HTML topology report\n"
        "      --topo-json  emit the full cluster topology as JSON\n"
        "      --measure    measure latency and bandwidth per cluster (slow)\n"
        "      --devices    also report GPUs and KPUs\n"
        "      --kpu-config PATH  a kpu-sim system configuration to read\n"
        "      --dram-mib N working set for the measured DRAM rows (default 64)\n"
        "\n"
        "PLACEHOLDER: only portable attributes are reported. Cache sizes, NUMA\n"
        "topology, and GPU/KPU devices need the per-platform backends described\n"
        "in include/ppe/platform.hpp.\n",
        ppe::version_string);
}

// 0 means "not detected" throughout the attribute set, so the human-readable
// form says so rather than printing a zero that reads like a measurement.
void print_field(const char* label, unsigned value) {
    if (value == 0) {
        std::printf("  %-22s not detected\n", label);
    } else {
        std::printf("  %-22s %u\n", label, value);
    }
}

void print_bytes(const char* label, std::size_t value) {
    if (value == 0) {
        std::printf("  %-22s not detected\n", label);
    } else {
        std::printf("  %-22s %zu bytes\n", label, value);
    }
}

/// Cache line with its sharing count. bytes / sharing_cores is the per-core
/// budget a model should spend, but the two are printed separately: this
/// describes the machine, and how to spend a shared cache is a modelling
/// decision. 0 sharers means the topology could not be read -- which is not the
/// same as "private", and is not printed as 1.
void print_cache(const char* label, std::size_t bytes, std::size_t sharers) {
    if (bytes == 0) {
        std::printf("  %-22s not detected\n", label);
        return;
    }
    char size_buf[32];
    if (bytes >= 1024 * 1024) {
        std::snprintf(size_buf, sizeof(size_buf), "%.4g MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(size_buf, sizeof(size_buf), "%.4g KiB",
                      static_cast<double>(bytes) / 1024.0);
    }

    if (sharers == 0) {
        std::printf("  %-22s %-10s (sharing unknown)\n", label, size_buf);
    } else if (sharers == 1) {
        std::printf("  %-22s %-10s (private)\n", label, size_buf);
    } else {
        std::printf("  %-22s %-10s (shared by %zu cores, %.4g KiB/core)\n", label,
                    size_buf, sharers,
                    static_cast<double>(bytes) / sharers / 1024.0);
    }
}

void print_text(const ppe::provenance& p) {
    const ppe::device_attributes& a = p.cpu;
    std::fputs(ppe::to_text(p).c_str(), stdout);
    std::printf("\n%s (via %s):\n", ppe::to_string(a.kind),
                a.source.empty() ? "no backend" : a.source.c_str());
    if (!a.vendor.empty()) {
        std::printf("  %-22s %s\n", "vendor", a.vendor.c_str());
    }
    print_field("logical processors", a.logical_processors);
    print_field("physical cores", a.physical_cores);
    print_field("NUMA domains", a.numa_domains);
    print_cache("L1d", a.l1d_bytes, a.l1d_sharing_cores);
    print_cache("L2", a.l2_bytes, a.l2_sharing_cores);
    print_cache("L3", a.l3_bytes, a.l3_sharing_cores);
    print_bytes("cache line", a.cache_line_bytes);

    // Machine ISA is what the silicon offers; build ISA is the ceiling for this
    // binary. Reporting only one is how a result becomes unattributable -- 25%
    // of "peak" means something different when the binary was built for a
    // baseline ISA than when it was built for the machine's widest.
    const ppe::isa_capabilities isa = ppe::detect_isa();
    std::printf("\nSIMD:\n");
    std::printf("  %-22s %s", "machine ISA", isa.name.c_str());
    if (isa.vector_bits > 0) std::printf(" (%u-bit vectors)", isa.vector_bits);
    std::printf("\n");
    std::printf("  %-22s %s\n", "build ISA", ppe::build_isa());
    if (isa.sve) {
        std::printf("  %-22s %s\n", "",
                    "SVE present; width is implementation defined and not reported");
    }

    const ppe::clock_reading clk = ppe::best_clock();
    if (clk.ghz > 0.0 && clk.measured) {
        std::printf("  %-22s %.3f GHz (perf_event -- MEASURED sustained)\n", "clock",
                    clk.ghz);
    } else if (clk.ghz > 0.0) {
        std::printf("  %-22s %.3f GHz (%s -- a claim, not a measurement)\n", "clock",
                    clk.ghz, clk.source.c_str());
        if (!clk.note.empty()) {
            std::printf("  %-22s %s\n", "", clk.note.c_str());
        }
    } else {
        std::printf("  %-22s not detected (pass --ghz to consumers)\n", "clock");
    }

    std::printf(
        "\nDetection is affinity-aware: it describes the cores this process may run\n"
        "on. Run under taskset to see the hierarchy of a specific core type.\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        print_help();
        return 0;
    }

    const ppe::provenance prov = ppe::collect_provenance();

    // JSON and CSV-comment forms come from ppe/provenance.hpp rather than being
    // hand-rolled here: they need consistent escaping (build flags arrive with
    // quotes and backslashes on Windows) and every PPE executable must emit the
    // same schema, or a result file cannot be joined to any other.
    if (ppe::has_flag(argc, argv, "--topo-json")) {
        std::fputs(ppe::report::to_json(ppe::detect_topology()).c_str(), stdout);
        return 0;
    }

    const bool measure = ppe::has_flag(argc, argv, "--measure");
    std::size_t dram_mib = 64;
    if (const char* v = ppe::flag_value(argc, argv, "--dram-mib"); v != nullptr) {
        const int n = std::atoi(v);
        if (n > 0) dram_mib = static_cast<std::size_t>(n);
    }
    const std::size_t dram_bytes = dram_mib * 1024u * 1024u;

    if (const char* html = ppe::flag_value(argc, argv, "--html"); html != nullptr) {
        const ppe::platform_topology topo = ppe::detect_topology();
        const std::vector<ppe::report::cluster_measurement> meas =
            measure ? ppe::report::measure_clusters(topo, dram_bytes)
                    : std::vector<ppe::report::cluster_measurement>{};
        std::FILE* f = std::fopen(html, "w");
        if (f == nullptr) {
            std::fprintf(stderr, "error: cannot write %s\n", html);
            return 1;
        }
        const std::string page = ppe::report::to_html(topo, prov, meas);
        std::fwrite(page.data(), 1, page.size(), f);
        std::fclose(f);
        std::printf("wrote %zu bytes of HTML to %s\n", page.size(), html);
        return 0;
    }

    if (ppe::has_flag(argc, argv, "--ascii")) {
        const ppe::platform_topology topo = ppe::detect_topology();
        if (measure) {
            std::printf("measuring %zu clusters (pin with taskset for a single one)...\n\n",
                        topo.clusters.size());
            std::fflush(stdout);
        }
        const std::vector<ppe::report::cluster_measurement> meas =
            measure ? ppe::report::measure_clusters(topo, dram_bytes)
                    : std::vector<ppe::report::cluster_measurement>{};
        std::fputs(ppe::report::to_ascii(topo, meas).c_str(), stdout);
        if (ppe::has_flag(argc, argv, "--devices")) {
            const char* kc = ppe::flag_value(argc, argv, "--kpu-config");
            std::fputs(ppe::report::accelerators_to_ascii(
                           ppe::detect_accelerators(kc ? kc : ""))
                           .c_str(),
                       stdout);
        }
        return 0;
    }

    if (ppe::has_flag(argc, argv, "--json")) {
        std::fputs(ppe::to_json(prov).c_str(), stdout);
    } else if (ppe::has_flag(argc, argv, "--csv-header")) {
        std::fputs(ppe::to_csv_comment(prov).c_str(), stdout);
    } else {
        print_text(prov);
    }
    return 0;
}
