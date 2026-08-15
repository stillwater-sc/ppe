// isa.hpp -- what SIMD the MACHINE can do, as opposed to what this binary was
// compiled for.
//
// Two different questions, and the peak model needs both:
//
//   ppe::build_isa()      what the compiler targeted. The ceiling for THIS
//                         binary: code compiled for SSE2 cannot use AVX-512 no
//                         matter what the silicon offers.
//   ppe::detect_isa()     what the silicon offers. The ceiling for the machine,
//                         and the right input to a peak model that describes
//                         hardware rather than a particular build.
//
// A measurement that reports one without the other is unattributable: a result
// at 25% of "peak" means something entirely different when the binary was built
// for a baseline ISA than when it was built for the machine's widest.
//
// OS ENABLEMENT IS PART OF THE ANSWER. A CPU can report AVX support while the OS
// has not enabled the register state to save it across context switches, and
// using it then faults. The XGETBV check below is not pedantry -- feature bits
// alone are the classic way to produce a peak model for registers the process
// cannot legally touch.
#pragma once

#include <ppe/detect/cpuid.hpp>

#include <cstddef>
#include <string>

#if defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
#  include <sys/auxv.h>
#endif

namespace ppe {

/// SIMD capability of the machine this is running on.
struct isa_capabilities {
    bool sse2 = false;
    bool avx = false;
    bool avx2 = false;
    bool fma = false;         ///< FMA3: fused multiply-add, 2 ops per instruction
    bool avx512f = false;
    bool avx512dq = false;    ///< 64-bit SIMD integer multiply lives here
    bool avx_vnni = false;    ///< int8 dot-product acceleration
    bool avx512_vnni = false;
    bool neon = false;
    bool sve = false;

    /// Widest usable vector register, in bits. 0 if nothing was detected.
    ///
    /// SVE is deliberately NOT reported here: its width is implementation
    /// defined (128..2048) and only discoverable by executing an instruction, so
    /// a fixed number would be a guess. When sve is true and this reads 128, the
    /// NEON floor is being reported and the real width may be larger.
    unsigned vector_bits = 0;

    /// Short human-readable summary, e.g. "AVX2+FMA" or "AArch64 NEON".
    std::string name = "unknown";
};

namespace detect {

#if PPE_HAS_X86_CPUID

/// True if the OS has enabled saving the YMM (and optionally ZMM) register
/// state. Without this, the feature bits are a trap.
inline bool os_supports_avx(bool want_avx512) {
    unsigned regs[4];
    cpuidex(1, 0, regs);
    const bool osxsave = (regs[2] & (1u << 27)) != 0;
    if (!osxsave) return false;

    // XCR0 bit 1 = SSE state, bit 2 = AVX (YMM) state,
    // bits 5,6,7 = AVX-512 opmask, ZMM_Hi256, Hi16_ZMM.
    unsigned long long xcr0 = 0;
#  if defined(_MSC_VER)
    xcr0 = _xgetbv(0);
#  else
    unsigned eax, edx;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    xcr0 = (static_cast<unsigned long long>(edx) << 32) | eax;
#  endif
    const bool ymm = (xcr0 & 0x6ull) == 0x6ull;
    if (!want_avx512) return ymm;
    return ymm && (xcr0 & 0xE0ull) == 0xE0ull;
}

inline isa_capabilities detect_isa_x86() {
    isa_capabilities c;
    unsigned regs[4];

    cpuidex(0, 0, regs);
    const unsigned max_leaf = regs[0];

    cpuidex(1, 0, regs);
    c.sse2 = (regs[3] & (1u << 26)) != 0;
    const bool cpu_avx = (regs[2] & (1u << 28)) != 0;
    const bool cpu_fma = (regs[2] & (1u << 12)) != 0;

    bool cpu_avx2 = false, cpu_avx512f = false, cpu_avx512dq = false;
    bool cpu_avx512vnni = false, cpu_avxvnni = false;
    if (max_leaf >= 7) {
        cpuidex(7, 0, regs);
        cpu_avx2       = (regs[1] & (1u << 5)) != 0;
        cpu_avx512f    = (regs[1] & (1u << 16)) != 0;
        cpu_avx512dq   = (regs[1] & (1u << 17)) != 0;
        cpu_avx512vnni = (regs[2] & (1u << 11)) != 0;

        cpuidex(7, 1, regs);
        cpu_avxvnni = (regs[0] & (1u << 4)) != 0;
    }

    const bool ymm_ok = cpu_avx && os_supports_avx(false);
    const bool zmm_ok = cpu_avx512f && os_supports_avx(true);

    c.avx          = ymm_ok;
    c.avx2         = cpu_avx2 && ymm_ok;
    c.fma          = cpu_fma && ymm_ok;
    c.avx512f      = zmm_ok;
    c.avx512dq     = cpu_avx512dq && zmm_ok;
    c.avx512_vnni  = cpu_avx512vnni && zmm_ok;
    c.avx_vnni     = cpu_avxvnni && ymm_ok;

    if (c.avx512f)      { c.vector_bits = 512; c.name = "x86-64 AVX-512"; }
    else if (c.avx2)    { c.vector_bits = 256; c.name = c.fma ? "x86-64 AVX2+FMA" : "x86-64 AVX2"; }
    else if (c.avx)     { c.vector_bits = 256; c.name = "x86-64 AVX"; }
    else if (c.sse2)    { c.vector_bits = 128; c.name = "x86-64 SSE2"; }
    return c;
}

#endif  // PPE_HAS_X86_CPUID

#if defined(__aarch64__) || defined(_M_ARM64)

inline isa_capabilities detect_isa_arm() {
    isa_capabilities c;
    // NEON (ASIMD) is mandatory on AArch64 -- there is no configuration without
    // it, so there is nothing to probe.
    c.neon = true;
    c.vector_bits = 128;
    c.name = "AArch64 NEON";

#  if defined(__linux__)
#    if defined(HWCAP_SVE)
    const unsigned long hw = ::getauxval(AT_HWCAP);
    if ((hw & HWCAP_SVE) != 0) {
        c.sve = true;
        // vector_bits stays at the NEON floor: SVE's width is implementation
        // defined and only discoverable by executing an instruction. Reporting
        // 128 understates a wider machine, which is the safe direction for a
        // ceiling -- overstating it would make every measurement look worse
        // than it is against a peak the hardware never had.
        c.name = "AArch64 NEON+SVE";
    }
#    endif
#  endif
    return c;
}

#endif  // __aarch64__

}  // namespace detect

/// Detect the machine's SIMD capability.
inline isa_capabilities detect_isa() {
#if PPE_HAS_X86_CPUID
    return detect::detect_isa_x86();
#elif defined(__aarch64__) || defined(_M_ARM64)
    return detect::detect_isa_arm();
#else
    return isa_capabilities{};
#endif
}

inline const isa_capabilities& cached_isa() {
    static const isa_capabilities c = detect_isa();
    return c;
}

}  // namespace ppe
