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
        "  -h, --help   show this help and exit\n"
        "      --json   emit the attribute set as JSON\n"
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

void print_text(const ppe::device_attributes& a) {
    std::printf("PPE %s -- platform attributes\n\n", ppe::version_string);
    std::printf("Build:\n");
    std::printf("  %-22s %s\n", "compiler", ppe::build_compiler());
    std::printf("  %-22s %s\n", "ISA baseline", ppe::build_isa());
    std::printf("\n%s:\n", ppe::to_string(a.kind));
    std::printf("  %-22s %s\n", "name", a.name.c_str());
    print_field("logical processors", a.logical_processors);
    print_field("physical cores", a.physical_cores);
    print_field("NUMA domains", a.numa_domains);
    print_bytes("L1d", a.l1d_bytes);
    print_bytes("L2", a.l2_bytes);
    print_bytes("L3", a.l3_bytes);
    print_bytes("cache line", a.cache_line_bytes);
}

void print_json(const ppe::device_attributes& a) {
    std::printf("{\n");
    std::printf("  \"ppe_version\": \"%s\",\n", ppe::version_string);
    std::printf("  \"build\": { \"compiler\": \"%s\", \"isa\": \"%s\" },\n",
                ppe::build_compiler(), ppe::build_isa());
    std::printf("  \"devices\": [\n");
    std::printf("    {\n");
    std::printf("      \"kind\": \"%s\",\n", ppe::to_string(a.kind));
    std::printf("      \"name\": \"%s\",\n", a.name.c_str());
    std::printf("      \"logical_processors\": %u,\n", a.logical_processors);
    std::printf("      \"physical_cores\": %u,\n", a.physical_cores);
    std::printf("      \"numa_domains\": %u,\n", a.numa_domains);
    std::printf("      \"l1d_bytes\": %zu,\n", a.l1d_bytes);
    std::printf("      \"l2_bytes\": %zu,\n", a.l2_bytes);
    std::printf("      \"l3_bytes\": %zu,\n", a.l3_bytes);
    std::printf("      \"cache_line_bytes\": %zu\n", a.cache_line_bytes);
    std::printf("    }\n");
    std::printf("  ]\n");
    std::printf("}\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        print_help();
        return 0;
    }

    const ppe::device_attributes cpu = ppe::detect_cpu();

    if (ppe::has_flag(argc, argv, "--json")) {
        print_json(cpu);
    } else {
        print_text(cpu);
    }
    return 0;
}
