// platform.hpp -- platform attribute detection (PLACEHOLDER).
//
// This is the seed of the repository's central contract: the machine model that
// blocking studies parameterize against, that traces are annotated with, and
// that visualizations label their axes from. It currently reports only what is
// portably available from the standard library and the compiler's predefined
// macros -- enough to exercise the build, not enough to engineer against.
//
// The real implementation is expected to grow per-platform backends behind this
// same interface:
//
//   CPU  : CPUID / sysfs (/sys/devices/system/cpu) / sysctl / GetLogicalProcessorInformationEx,
//          hwloc for topology and NUMA, cache line/size/sharing per level
//   GPU  : CUDA / HIP / Level Zero / Metal runtime queries -- SM or CU counts,
//          on-chip memory, clocks
//   KPU  : the kpu-sim / kpu-hw interfaces -- PE fabric geometry, scratchpad and
//          L1/L2/L3 tile capacities
//
// A device that is absent is a normal outcome, not an error: `detect()` reports
// what it found and leaves the rest unset.
#pragma once

#include <string>
#include <thread>

namespace ppe {

// Broad device class an attribute set describes.
enum class device_kind { cpu, gpu, kpu };

inline const char* to_string(device_kind k) {
    switch (k) {
        case device_kind::cpu: return "CPU";
        case device_kind::gpu: return "GPU";
        case device_kind::kpu: return "KPU";
    }
    return "unknown";
}

// A single device's performance-relevant attributes.
//
// Zero means "not detected" rather than "zero" -- the placeholder backend fills
// in almost nothing, and a study must be able to tell an absent value from a
// measured one.
struct device_attributes {
    device_kind kind = device_kind::cpu;
    std::string name = "unknown";
    unsigned    logical_processors = 0;   // hardware threads
    unsigned    physical_cores = 0;       // not portably available; 0 here
    unsigned    numa_domains = 0;         // 0 here
    std::size_t l1d_bytes = 0;            // 0 here
    std::size_t l2_bytes = 0;             // 0 here
    std::size_t l3_bytes = 0;             // 0 here
    std::size_t cache_line_bytes = 0;     // 0 here
};

// The ISA baseline this translation unit was compiled for.
//
// Compiler predefined macros describe the effect of the flags, which is the
// question that matters for a measurement: a result recorded without knowing
// its ISA baseline is not comparable to anything.
inline const char* build_isa() {
#if defined(__AVX512F__)
    return "x86-64 AVX-512";
#elif defined(__AVX2__)
    return "x86-64 AVX2";
#elif defined(__AVX__)
    return "x86-64 AVX";
#elif defined(__SSE2__) || defined(_M_X64)
    return "x86-64 SSE2";
#elif defined(__ARM_FEATURE_SVE)
    return "AArch64 SVE";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "AArch64 NEON";
#else
    return "unknown";
#endif
}

inline const char* build_compiler() {
#if defined(__clang__)
    return "Clang";
#elif defined(__GNUC__)
    return "GCC";
#elif defined(_MSC_VER)
    return "MSVC";
#else
    return "unknown";
#endif
}

// Detect the host CPU. PLACEHOLDER: reports hardware_concurrency and nothing
// else. Cache sizes, topology and NUMA need the per-platform backends above.
inline device_attributes detect_cpu() {
    device_attributes a;
    a.kind = device_kind::cpu;
    a.name = std::string("host CPU (") + build_isa() + ")";
    a.logical_processors = std::thread::hardware_concurrency();
    return a;
}

}  // namespace ppe
