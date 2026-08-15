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
#include <ppe/platform.hpp>
#include <ppe/provenance.hpp>
#include <ppe/version.hpp>

#include <cstdio>

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

void print_text(const ppe::provenance& p) {
    const ppe::device_attributes& a = p.cpu;
    std::fputs(ppe::to_text(p).c_str(), stdout);
    std::printf("\n%s:\n", ppe::to_string(a.kind));
    print_field("logical processors", a.logical_processors);
    print_field("physical cores", a.physical_cores);
    print_field("NUMA domains", a.numa_domains);
    print_bytes("L1d", a.l1d_bytes);
    print_bytes("L2", a.l2_bytes);
    print_bytes("L3", a.l3_bytes);
    print_bytes("cache line", a.cache_line_bytes);
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
    if (ppe::has_flag(argc, argv, "--json")) {
        std::fputs(ppe::to_json(prov).c_str(), stdout);
    } else if (ppe::has_flag(argc, argv, "--csv-header")) {
        std::fputs(ppe::to_csv_comment(prov).c_str(), stdout);
    } else {
        print_text(prov);
    }
    return 0;
}
