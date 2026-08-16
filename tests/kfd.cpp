// test_kfd -- assertions about the AMD KFD topology parser.
//
// This test exists because neither the development machine nor any CI runner
// has an AMD GPU. Without it the parser would ship having never been run, on
// hardware nobody involved can reach -- and its two conversions are exactly the
// kind that are wrong by a constant factor and look plausible anyway:
//
//   simd_count counts SIMDs, not compute units. A CU holds simd_per_cu of them
//   (2 on RDNA, 4 on GCN), so reporting simd_count as a CU count overstates the
//   geometry by that factor.
//
//   max_engine_clk_fcompute is kHz, not MHz. Off by 1000 in either direction is
//   a GPU clocked at 2 MHz or at 2.1 THz, neither of which a reader would take
//   for a real number -- but a chain that divides twice would give 2.1 MHz,
//   which they might.
//
// The fixtures are the real shape of /sys/class/kfd/kfd/topology/nodes/N/
// properties, including the CPU node the topology also publishes.

#include <ppe/cli.hpp>
#include <ppe/detect/accelerator.hpp>

#include <cstdio>
#include <sstream>

namespace {

int failures = 0;

void expect(const char* what, double got, double want) {
    const bool ok = (got == want);
    if (!ok) ++failures;
    std::printf("  %-44s got %10.2f  want %10.2f  %s\n", what, got, want,
                ok ? "ok" : "FAIL");
}

void expect_str(const char* what, const std::string& got, const std::string& want) {
    const bool ok = (got == want);
    if (!ok) ++failures;
    std::printf("  %-44s got %-10s want %-10s %s\n", what, got.c_str(), want.c_str(),
                ok ? "ok" : "FAIL");
}

// An RDNA2-shaped discrete GPU node.
const char* kGpuNode = R"(cpu_cores_count 0
simd_count 256
mem_banks_count 1
caches_count 78
io_links_count 1
max_waves_per_simd 8
lds_size_in_kb 64
gds_size_in_kb 0
wave_front_size 32
array_count 4
simd_arrays_per_engine 1
cu_per_simd_array 16
simd_per_cu 2
max_engine_clk_fcompute 2100000
location_id 768
)";

// The CPU node KFD also publishes: no SIMDs, and it must not be mistaken for a
// GPU.
const char* kCpuNode = R"(cpu_cores_count 20
simd_count 0
mem_banks_count 1
caches_count 0
simd_per_cu 0
max_engine_clk_fcompute 3000000
location_id 0
)";

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        std::printf("test_kfd -- verify the AMD KFD topology parser\n");
        return 0;
    }

    std::printf("GPU node:\n");
    {
        std::istringstream in(kGpuNode);
        const ppe::detect::kfd_node k = ppe::detect::parse_kfd_properties(in);
        expect("simd_count", static_cast<double>(k.simd_count), 256);
        expect("simd_per_cu", static_cast<double>(k.simd_per_cu), 2);
        expect("clk read as kHz", static_cast<double>(k.clk_khz), 2100000);
        expect("lds_size_in_kb", static_cast<double>(k.lds_kb), 64);
        expect("location_id", static_cast<double>(k.location_id), 768);

        ppe::accelerator a;
        ppe::detect::apply_kfd_node(k, a);
        expect("compute units = simd_count / simd_per_cu",
               static_cast<double>(a.compute.at(0).count), 128);
        expect_str("compute kind", a.compute.at(0).kind, "CU");
        expect("clock converted kHz -> MHz", a.clock_mhz, 2100.0);
        expect("LDS bytes", static_cast<double>(a.memory.at(0).bytes), 65536);
        expect("LDS instances = CU count",
               static_cast<double>(a.memory.at(0).instances), 128);
        expect_str("source", a.source, "kfd");
        expect_str("wavefront recorded", a.capability, "wave32");
    }

    std::printf("\nCPU node must not read as a GPU:\n");
    {
        std::istringstream in(kCpuNode);
        const ppe::detect::kfd_node k = ppe::detect::parse_kfd_properties(in);
        expect("simd_count is 0", static_cast<double>(k.simd_count), 0);
        // The walk skips on simd_count == 0; asserting the field is what that
        // decision rests on.
    }

    std::printf("\nMissing simd_per_cu falls back without dividing by zero:\n");
    {
        std::istringstream in("simd_count 64\nmax_engine_clk_fcompute 1000000\n");
        const ppe::detect::kfd_node k = ppe::detect::parse_kfd_properties(in);
        ppe::accelerator a;
        ppe::detect::apply_kfd_node(k, a);
        expect("count falls back to simd_count",
               static_cast<double>(a.compute.at(0).count), 64);
        expect("clock still converted", a.clock_mhz, 1000.0);
    }

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
