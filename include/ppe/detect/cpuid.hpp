// cpuid.hpp -- minimal portable CPUID wrapper.
//
// Ported from mtl5/include/mtl/util/cpuid.hpp (Stillwater, MIT).
//
// x86 only. PPE_HAS_X86_CPUID is 0 on every other ISA, where the whole block
// (including <cpuid.h> / <intrin.h>) disappears.
#pragma once

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  define PPE_HAS_X86_CPUID 1
#else
#  define PPE_HAS_X86_CPUID 0
#endif

#if PPE_HAS_X86_CPUID
#  if defined(_MSC_VER)
#    include <intrin.h>
#  else
#    include <cpuid.h>
#  endif

namespace ppe::detect {

/// Fill regs[eax, ebx, ecx, edx] for CPUID `leaf`/`subleaf`.
inline void cpuidex(int leaf, int subleaf, unsigned regs[4]) {
#  if defined(_MSC_VER)
    int r[4];
    __cpuidex(r, leaf, subleaf);
    regs[0] = static_cast<unsigned>(r[0]);
    regs[1] = static_cast<unsigned>(r[1]);
    regs[2] = static_cast<unsigned>(r[2]);
    regs[3] = static_cast<unsigned>(r[3]);
#  else
    unsigned a, b, c, d;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    regs[0] = a; regs[1] = b; regs[2] = c; regs[3] = d;
#  endif
}

}  // namespace ppe::detect
#endif  // PPE_HAS_X86_CPUID
