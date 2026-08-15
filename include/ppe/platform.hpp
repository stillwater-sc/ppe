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
    std::string vendor;                   // "GenuineIntel", "AuthenticAMD", ...

    unsigned    logical_processors = 0;   // hardware threads
    unsigned    physical_cores = 0;       // SMT siblings collapsed
    unsigned    numa_domains = 0;

    std::size_t l1d_bytes = 0;            // DATA cache, never the I-cache
    std::size_t l1d_assoc = 0;            // ways
    std::size_t l2_bytes = 0;
    std::size_t l3_bytes = 0;             // 0 is legitimate: some parts have none
    std::size_t cache_line_bytes = 0;

    // How many distinct PHYSICAL cores share each level. The per-core budget a
    // model should use is bytes / sharing_cores, but the two are reported
    // separately rather than pre-divided: this struct describes the machine, and
    // how to spend a shared cache is a modelling decision that belongs with the
    // model.
    //
    // Counted in cores, not logical CPUs, and the difference is not pedantic. An
    // SMT pair shares its L1d and L2, so counting CPUs would halve both on any
    // hyperthreaded machine; a 4-core E-cluster sharing one L2 is the case that
    // genuinely needs discounting.
    std::size_t l1d_sharing_cores = 0;
    std::size_t l2_sharing_cores = 0;
    std::size_t l3_sharing_cores = 0;

    /// Where the numbers came from, so a disagreement with measurement can be
    /// attributed. "sysfs", "cpuid", "sysctl", "win32", or "" if nothing ran.
    std::string source;
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

// Portable floor: hardware concurrency and nothing else.
//
// The real backends live in <ppe/detect/cpu.hpp>, which pulls in sysfs, sysctl,
// CPUID and <windows.h> depending on the platform. This header stays free of
// those so the SCHEMA can be included anywhere -- by a consumer that only wants
// to read an attribute set someone else produced, for instance -- without
// dragging an OS header tree behind it.
//
// Use ppe::detect_cpu() from <ppe/detect/cpu.hpp> for a populated set.
inline device_attributes minimal_cpu_attributes() {
    device_attributes a;
    a.kind = device_kind::cpu;
    a.name = std::string("host CPU (") + build_isa() + ")";
    a.logical_processors = std::thread::hardware_concurrency();
    return a;
}

}  // namespace ppe
