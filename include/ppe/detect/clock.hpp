// clock.hpp -- the core clock, as a CLAIM.
//
// WHY THIS IS NOT A MEASUREMENT, WHICH THE PLAN ORIGINALLY ASKED FOR.
//
// docs/plans/first-application.md says phase 3 takes the clock "from a
// sustained-clock measurement rather than a command-line flag". Reaching that
// honestly needs one of three things, and none of them is available here:
//
//   1. A performance counter (fixed-function CPU_CLK_UNHALTED). This is the
//      right instrument and it needs privileges PPE cannot assume -- perf_event
//      access on Linux is gated by perf_event_paranoid, and there is no portable
//      equivalent at all.
//   2. The timestamp counter. TSC is INVARIANT on every part since Nehalem: it
//      ticks at a fixed reference rate regardless of the core clock, which is
//      precisely what makes it a good wall clock and useless as a core clock.
//   3. A dependent instruction chain of known latency, timed. This turns the
//      problem into "assume a microarchitectural constant", which is the same
//      class of assumption the FMA-unit count already carries -- except that
//      here the constant varies more across vendors and the error is silent.
//
// A fourth option -- a throughput loop whose IPC is assumed -- is worse: the
// compiler decides the instruction mix, so the assumption is about the compiler
// rather than the hardware.
//
// So this reports what the OS claims, labelled as a claim, and every consumer
// prints it alongside the peak it produced. That is consistent with the rest of
// the repository: mtl5/ppe's peak.hpp already treats the clock as "a property of
// the machine, supplied by the caller rather than guessed here", and the honest
// improvement over a bare --ghz flag is a default that is usually right and
// always labelled, not a number that looks measured and is not.
//
// The gap is real and worth closing. A perf_event backend on Linux, used when
// permissions allow and degrading to the claim when they do not, would give a
// genuine sustained figure on the machines this project actually measures on.
// That is tracked as future work rather than faked here.
//
// WHAT THE CLAIM MEANS. This is a nominal or maximum frequency, not the clock
// the core sustained during a measurement. Under an AVX-heavy load with several
// cores busy, the real sustained clock can be well below it, which makes any
// efficiency computed from it an UNDER-estimate of what the machine achieved.
#pragma once

#include <cstddef>
#include <string>

#if defined(__linux__)
#  include <cstdlib>
#  include <fstream>
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
#elif defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace ppe {

struct clock_claim {
    double ghz = 0.0;         ///< 0 = not detected
    std::string source;       ///< "cpufreq", "proc", "sysctl", "registry", ""
    bool is_max = false;      ///< true = maximum/nominal, false = instantaneous
};

namespace detect {

#if defined(__linux__)

inline clock_claim clock_linux() {
    clock_claim c;

    // cpufreq reports kHz. scaling_max_freq is the policy ceiling, which is the
    // most stable thing available -- the "current" file changes between the read
    // and any use of it.
    for (const char* path : {"/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
                             "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"}) {
        std::ifstream in(path);
        if (!in) continue;
        double khz = 0.0;
        if (in >> khz && khz > 0.0) {
            c.ghz = khz / 1e6;
            c.source = "cpufreq";
            c.is_max = true;
            return c;
        }
    }

    // /proc/cpuinfo "cpu MHz" is the instantaneous frequency of that CPU at the
    // moment of the read. Better than nothing, worse than cpufreq, and labelled
    // as instantaneous so a reader knows not to trust it as a ceiling.
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 7, "cpu MHz") == 0) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const double mhz = std::atof(line.c_str() + colon + 1);
            if (mhz > 0.0) {
                c.ghz = mhz / 1e3;
                c.source = "proc";
                c.is_max = false;
                return c;
            }
        }
    }
    return c;
}

#elif defined(__APPLE__)

inline clock_claim clock_apple() {
    clock_claim c;
    // hw.cpufrequency_max exists on Intel Macs. Apple silicon exposes no core
    // frequency through any public interface, so this legitimately returns
    // nothing there -- which is reported as "not detected" rather than filled in
    // with a plausible guess.
    std::uint64_t hz = 0;
    std::size_t len = sizeof(hz);
    for (const char* name : {"hw.cpufrequency_max", "hw.cpufrequency"}) {
        len = sizeof(hz);
        if (::sysctlbyname(name, &hz, &len, nullptr, 0) == 0 && hz > 0) {
            c.ghz = static_cast<double>(hz) / 1e9;
            c.source = "sysctl";
            c.is_max = true;
            return c;
        }
    }
    return c;
}

#elif defined(_WIN32)

inline clock_claim clock_windows() {
    clock_claim c;
    HKEY key{};
    if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                        KEY_READ, &key) != ERROR_SUCCESS) {
        return c;
    }
    DWORD mhz = 0;
    DWORD size = sizeof(mhz);
    DWORD type = 0;
    const LSTATUS rc = ::RegQueryValueExA(key, "~MHz", nullptr, &type,
                                          reinterpret_cast<LPBYTE>(&mhz), &size);
    ::RegCloseKey(key);
    if (rc == ERROR_SUCCESS && type == REG_DWORD && mhz > 0) {
        // ~MHz is the frequency recorded at boot. It is nominal, not a ceiling
        // and not current.
        c.ghz = static_cast<double>(mhz) / 1e3;
        c.source = "registry";
        c.is_max = false;
    }
    return c;
}

#endif

}  // namespace detect

/// The OS-claimed core clock. Never a sustained measurement -- see the header
/// comment for why, and for what it would take to do better.
inline clock_claim detect_clock() {
#if defined(__linux__)
    return detect::clock_linux();
#elif defined(__APPLE__)
    return detect::clock_apple();
#elif defined(_WIN32)
    return detect::clock_windows();
#else
    return clock_claim{};
#endif
}

}  // namespace ppe
