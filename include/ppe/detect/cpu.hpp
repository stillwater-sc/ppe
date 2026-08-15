// cpu.hpp -- CPU attribute detection: what the OS claims this machine is.
//
// Adapted from mtl5/include/mtl/util/{cache_info,system_info}.hpp (Stillwater,
// MIT), with the parts mtl5 left out of scope added here because they are this
// repository's subject rather than a means to an end:
//
//   * physical core counts and NUMA domains, which mtl5's cpu_info documents as
//     "not portably derivable and stays out of scope"
//   * a real Windows backend. mtl5 falls back to CPUID on Windows and says why:
//     GetLogicalProcessorInformationEx needs <windows.h>, which its core library
//     must not force on every translation unit. PPE has no such constraint --
//     detection IS the product here -- so the Win32 path is implemented, which
//     also fixes the one configuration CPUID cannot reach at all: Windows ARM64.
//
// EVERY FIELD IS BEST EFFORT. 0 means NOT DETECTED and callers must fall back;
// it never means "a cache of size zero". A machine model that silently reports a
// zero-byte L3 produces confident nonsense downstream.
//
// WHY SYSFS IS PREFERRED OVER CPUID ON LINUX, x86 INCLUDED:
//
//   * DETERMINISM. CPUID describes whichever core the calling thread happens to
//     be on, so on a hybrid part (Alder Lake and later) the same binary reports
//     a P-core or an E-core hierarchy run to run.
//   * SHARING. sysfs publishes shared_cpu_list per cache, so a cluster L2 shared
//     by four cores can be discounted. CPUID's equivalent counts logical
//     processors and needs threads-per-core to interpret.
//   * AFFINITY. sysfs can be scanned over exactly the CPUs this process may run
//     on, so under `taskset` detection describes where the work will actually
//     run. That matters more here than in mtl5: PPE's own measurement guidance
//     is to pin the process, so detection and measurement must agree about which
//     core they are talking about.
#pragma once

#include <ppe/detect/cpuid.hpp>
#include <ppe/platform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#  include <cerrno>
#  include <fstream>
#  include <sched.h>
#  include <set>
#  include <utility>
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
#  if defined(_MSC_VER)
// RegGetValue/RegOpenKeyEx live in Advapi32; auto-link it so header-only
// consumers on MSVC and Clang-CL need no extra link flags. Other Windows
// toolchains link -ladvapi32 in CMake.
#    pragma comment(lib, "advapi32")
#  endif
#endif

namespace ppe {
namespace detect {

// ---------------------------------------------------------------------------
// x86 CPUID
// ---------------------------------------------------------------------------
#if PPE_HAS_X86_CPUID

inline std::string vendor_string_x86() {
    unsigned regs[4];
    cpuidex(0, 0, regs);
    char v[13] = {};
    // EBX, EDX, ECX -- the order the vendor string is returned in.
    for (int i = 0; i < 4; ++i) v[i]     = static_cast<char>((regs[1] >> (8 * i)) & 0xff);
    for (int i = 0; i < 4; ++i) v[4 + i] = static_cast<char>((regs[3] >> (8 * i)) & 0xff);
    for (int i = 0; i < 4; ++i) v[8 + i] = static_cast<char>((regs[2] >> (8 * i)) & 0xff);
    return std::string(v);
}

/// Marketing brand string from extended leaves 0x80000002..4.
inline std::string brand_string_x86() {
    unsigned regs[4];
    cpuidex(static_cast<int>(0x80000000u), 0, regs);
    if (regs[0] < 0x80000004u) return {};

    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
        cpuidex(static_cast<int>(0x80000002u + static_cast<unsigned>(i)), 0, regs);
        std::memcpy(brand + i * 16, regs, 16);
    }
    std::string b = brand;
    const std::size_t first = b.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const std::size_t last = b.find_last_not_of(" \t\r\n");
    return b.substr(first, last - first + 1);
}

/// CPUID deterministic-cache-parameters walk. Intel uses leaf 4; AMD publishes
/// the identical encoding at extended leaf 0x8000001D.
///
/// Per-CURRENT-CORE and reports no sharing, so on a hybrid machine it is not
/// reproducible. That is why it is a fallback rather than the default.
inline void fill_caches_x86(device_attributes& a) {
    unsigned regs[4];
    cpuidex(0, 0, regs);
    const unsigned max_leaf = regs[0];

    const std::string vendor = vendor_string_x86();
    const bool is_amd = vendor == "AuthenticAMD";

    int leaf = 4;
    if (is_amd) {
        cpuidex(static_cast<int>(0x80000000u), 0, regs);
        if (regs[0] >= 0x8000001Du) leaf = static_cast<int>(0x8000001Du);
    }
    if (leaf == 4 && max_leaf < 4) return;  // no deterministic-cache leaf at all

    for (int i = 0; i < 32; ++i) {
        cpuidex(leaf, i, regs);
        const unsigned type = regs[0] & 0x1fu;
        if (type == 0) break;  // 0 = null: end of enumeration
        const unsigned level = (regs[0] >> 5) & 0x7u;
        const std::size_t line  = static_cast<std::size_t>(regs[1] & 0xfffu) + 1;
        const std::size_t parts = static_cast<std::size_t>((regs[1] >> 12) & 0x3ffu) + 1;
        const std::size_t ways  = static_cast<std::size_t>((regs[1] >> 22) & 0x3ffu) + 1;
        const std::size_t sets  = static_cast<std::size_t>(regs[2]) + 1;
        const std::size_t size  = ways * parts * line * sets;

        if (a.cache_line_bytes == 0) a.cache_line_bytes = line;
        // type 1 = data, 2 = instruction, 3 = unified. The I-cache must never be
        // mistaken for L1d: they are the same size on most parts, so the error
        // would be invisible in the numbers.
        if (level == 1 && type == 1) { a.l1d_bytes = size; a.l1d_assoc = ways; }
        else if (level == 2 && type != 2) { a.l2_bytes = size; }
        else if (level == 3 && type != 2) { a.l3_bytes = size; }
    }
}

#endif  // PPE_HAS_X86_CPUID

// ---------------------------------------------------------------------------
// Linux: sysfs
// ---------------------------------------------------------------------------
#if defined(__linux__)

inline std::string read_sysfs(const std::string& path) {
    std::ifstream in(path);
    std::string s;
    if (in) std::getline(in, s);
    return s;
}

/// Parse the kernel's cache size spelling: a decimal with an optional K/M/G.
inline std::size_t parse_size(const std::string& s) {
    if (s.empty()) return 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end == s.c_str()) return 0;
    std::size_t mult = 1;
    if (end && *end) {
        switch (*end) {
            case 'K': case 'k': mult = 1024; break;
            case 'M': case 'm': mult = 1024 * 1024; break;
            case 'G': case 'g': mult = 1024 * 1024 * 1024; break;
            default: break;
        }
    }
    return static_cast<std::size_t>(v) * mult;
}

/// Expand a sysfs cpu list ("0-3,8" or "0,6") to the ids it names.
inline std::vector<int> parse_cpu_list(const std::string& s) {
    std::vector<int> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        const std::string part = s.substr(pos, comma - pos);
        const std::size_t dash = part.find('-');
        if (dash == std::string::npos) {
            if (!part.empty()) out.push_back(std::atoi(part.c_str()));
        } else {
            const int lo = std::atoi(part.substr(0, dash).c_str());
            const int hi = std::atoi(part.substr(dash + 1).c_str());
            for (int v = lo; v <= hi; ++v) out.push_back(v);
        }
        pos = comma + 1;
    }
    return out;
}

/// How many distinct PHYSICAL cores appear in a cpu list. SMT siblings collapse
/// to one; a 4-core cluster counts 4. Identity is (package, core_id), because
/// core_id is only unique within a package.
inline std::size_t distinct_cores(const std::vector<int>& cpus) {
    std::set<std::pair<int, int>> cores;
    for (int cpu : cpus) {
        const std::string topo =
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        const std::string core = read_sysfs(topo + "core_id");
        if (core.empty()) continue;  // topology unavailable
        const std::string pkg = read_sysfs(topo + "physical_package_id");
        cores.insert({pkg.empty() ? 0 : std::atoi(pkg.c_str()), std::atoi(core.c_str())});
    }
    return cores.size();
}

/// CPUs this process may actually be scheduled on. Under `taskset` that is the
/// pinned set, which is what makes detection agree with where the work will run.
///
/// A plain cpu_set_t is fixed at CPU_SETSIZE (1024) logical CPUs, and on a host
/// configured with more, sched_getaffinity fails with EINVAL. Falling back to
/// cpu0 there would be worse than useless: cpu0 need not even be in the mask, so
/// detection would describe a core the process cannot run on. Grow a dynamically
/// sized set until it fits instead.
inline std::vector<int> allowed_cpus() {
    std::vector<int> out;
#if defined(CPU_ALLOC)
    for (int ncpus = CPU_SETSIZE; ncpus <= (1 << 20); ncpus *= 2) {
        cpu_set_t* set = CPU_ALLOC(ncpus);
        if (set == nullptr) break;
        const std::size_t sz = CPU_ALLOC_SIZE(ncpus);
        CPU_ZERO_S(sz, set);
        errno = 0;
        const bool ok = (::sched_getaffinity(0, sz, set) == 0);
        if (ok) {
            for (int i = 0; i < ncpus; ++i)
                if (CPU_ISSET_S(static_cast<std::size_t>(i), sz, set)) out.push_back(i);
        }
        const bool too_small = (!ok && errno == EINVAL);
        CPU_FREE(set);
        if (ok || !too_small) break;  // success, or a real failure
    }
#else
    cpu_set_t set;  // libc without CPU_ALLOC
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) == 0)
        for (int i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &set)) out.push_back(i);
#endif
    if (out.empty()) out.push_back(0);  // affinity unavailable: assume cpu0
    return out;
}

/// Scan the cpus this process may run on and keep, per level, the entry with the
/// SMALLEST per-core budget.
///
/// Taking the minimum means a model's blocks fit whichever core the work lands
/// on rather than overflowing the smaller kind. Under `taskset` the scan is the
/// pinned set; unpinned on a hybrid machine it is the whole machine. Either way
/// it is a property of the machine and the affinity mask, not of the scheduler's
/// whim.
inline void fill_caches_sysfs(device_attributes& a) {
    std::size_t best_l1 = static_cast<std::size_t>(-1);
    std::size_t best_l2 = static_cast<std::size_t>(-1);
    std::size_t best_l3 = static_cast<std::size_t>(-1);

    const std::vector<int> cpus = allowed_cpus();

    for (int cpu : cpus) {
        const std::string base =
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/index";
        for (int i = 0; i < 10; ++i) {
            const std::string dir = base + std::to_string(i);
            const std::string lvl = read_sysfs(dir + "/level");
            if (lvl.empty()) break;  // no more index<N> entries
            const std::string type = read_sysfs(dir + "/type");
            const std::size_t size = parse_size(read_sysfs(dir + "/size"));
            const std::size_t line = parse_size(read_sysfs(dir + "/coherency_line_size"));
            const int level = std::atoi(lvl.c_str());
            if (size == 0) continue;

            // 0 is STORED as 0 when the topology could not be read: reporting an
            // unreadable topology as "shared by exactly one core" would claim
            // knowledge of a private cache we do not have. Only the local divisor
            // below treats it as 1.
            const std::size_t sharers =
                distinct_cores(parse_cpu_list(read_sysfs(dir + "/shared_cpu_list")));
            const std::size_t per_core = size / (sharers ? sharers : 1);

            if (a.cache_line_bytes == 0 && line != 0) a.cache_line_bytes = line;
            const bool is_instruction = (type == "Instruction");
            if (level == 1 && type == "Data") {
                if (per_core < best_l1) {
                    best_l1 = per_core;
                    a.l1d_bytes = size;
                    a.l1d_sharing_cores = sharers;
                    a.l1d_assoc = parse_size(read_sysfs(dir + "/ways_of_associativity"));
                }
            } else if (level == 2 && !is_instruction) {
                if (per_core < best_l2) {
                    best_l2 = per_core;
                    a.l2_bytes = size;
                    a.l2_sharing_cores = sharers;
                }
            } else if (level == 3 && !is_instruction) {
                if (per_core < best_l3) {
                    best_l3 = per_core;
                    a.l3_bytes = size;
                    a.l3_sharing_cores = sharers;
                }
            }
        }
    }

    // Physical cores among the CPUs this process may run on -- consistent with
    // the cache scan above, so both describe the same set of hardware.
    a.physical_cores = static_cast<unsigned>(distinct_cores(cpus));

    // NUMA domains: count node<N> directories.
    unsigned nodes = 0;
    for (int n = 0; n < 256; ++n) {
        const std::string path =
            "/sys/devices/system/node/node" + std::to_string(n) + "/cpulist";
        std::ifstream probe(path);
        if (!probe) continue;
        ++nodes;
    }
    a.numa_domains = nodes;
}

/// Brand from /proc/cpuinfo. "model name" on x86, "Model" or "CPU part" on ARM.
inline std::string brand_proc_cpuinfo() {
    std::ifstream in("/proc/cpuinfo");
    if (!in) return {};
    std::string line;
    while (std::getline(in, line)) {
        for (const char* key : {"model name", "Model", "Hardware"}) {
            const std::size_t klen = std::strlen(key);
            if (line.compare(0, klen, key) == 0) {
                const std::size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                const std::size_t first = line.find_first_not_of(" \t", colon + 1);
                if (first == std::string::npos) continue;
                return line.substr(first);
            }
        }
    }
    return {};
}

#endif  // __linux__

// ---------------------------------------------------------------------------
// macOS: sysctl
// ---------------------------------------------------------------------------
#if defined(__APPLE__)

inline std::size_t sysctl_size(const char* name) {
    std::size_t v = 0, len = sizeof(v);
    if (::sysctlbyname(name, &v, &len, nullptr, 0) == 0) return v;
    return 0;
}

inline std::string sysctl_string(const char* name) {
    std::size_t len = 0;
    if (::sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    std::string buf(len, '\0');
    if (::sysctlbyname(name, buf.data(), &len, nullptr, 0) != 0) return {};
    if (!buf.empty() && buf.back() == '\0') buf.pop_back();
    return buf;
}

/// Apple Silicon reports per-performance-level caches; hw.l*cachesize is the
/// Intel-era spelling and is absent or zero on M-series.
///
/// perflevel0 is the PERFORMANCE core cluster. Taking it rather than the
/// efficiency cluster matches the sysfs backend's intent only loosely -- that one
/// takes the smallest per-core budget. The asymmetry is deliberate: macOS gives
/// no portable way to pin to a cluster, so a measurement here lands wherever the
/// scheduler puts it, and reporting the P-core figures at least names a real
/// configuration rather than a blend of two.
inline void fill_caches_apple(device_attributes& a) {
    a.l1d_bytes = sysctl_size("hw.perflevel0.l1dcachesize");
    if (a.l1d_bytes == 0) a.l1d_bytes = sysctl_size("hw.l1dcachesize");
    a.l2_bytes = sysctl_size("hw.perflevel0.l2cachesize");
    if (a.l2_bytes == 0) a.l2_bytes = sysctl_size("hw.l2cachesize");
    a.l3_bytes = sysctl_size("hw.l3cachesize");  // typically absent on M-series
    a.cache_line_bytes = sysctl_size("hw.cachelinesize");

    a.physical_cores = static_cast<unsigned>(sysctl_size("hw.physicalcpu"));
    // macOS exposes no NUMA interface; every shipping Mac is single-domain.
    a.numa_domains = 1;
}

#endif  // __APPLE__

// ---------------------------------------------------------------------------
// Windows: GetLogicalProcessorInformationEx
// ---------------------------------------------------------------------------
#if defined(_WIN32)

/// Count set bits across a processor group affinity mask.
inline unsigned popcount_affinity(KAFFINITY mask) {
    unsigned n = 0;
    while (mask) {
        n += static_cast<unsigned>(mask & 1u);
        mask >>= 1;
    }
    return n;
}

/// Walk GetLogicalProcessorInformationEx for caches, physical cores and NUMA.
///
/// This is the path mtl5 could not take (its core must not include <windows.h>)
/// and it covers the one configuration CPUID cannot reach at all: Windows on
/// ARM64. It also knows about sharing, like sysfs and unlike CPUID.
inline void fill_caches_win32(device_attributes& a) {
    for (const LOGICAL_PROCESSOR_RELATIONSHIP rel :
         {RelationCache, RelationProcessorCore, RelationNumaNode}) {
        DWORD len = 0;
        ::GetLogicalProcessorInformationEx(rel, nullptr, &len);
        if (len == 0) continue;

        std::vector<unsigned char> buf(len);
        if (!::GetLogicalProcessorInformationEx(
                rel,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()),
                &len)) {
            continue;
        }

        std::size_t best_l1 = static_cast<std::size_t>(-1);
        std::size_t best_l2 = static_cast<std::size_t>(-1);
        std::size_t best_l3 = static_cast<std::size_t>(-1);
        unsigned cores = 0;
        unsigned nodes = 0;

        for (DWORD off = 0; off < len;) {
            auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                buf.data() + off);
            if (info->Size == 0) break;  // malformed: do not spin forever

            if (info->Relationship == RelationCache) {
                const CACHE_RELATIONSHIP& c = info->Cache;
                // CacheData and CacheUnified only: the I-cache must never be
                // mistaken for L1d.
                if (c.Type != CacheInstruction) {
                    const std::size_t size = static_cast<std::size_t>(c.CacheSize);
                    // GroupMask counts LOGICAL processors, so an SMT pair reads
                    // as 2 where sysfs would say 1 core. Divide by threads per
                    // core to get cores; without that figure, leave sharing at 0
                    // (unknown) rather than record a doubled count.
                    const unsigned sharing_lp = popcount_affinity(c.GroupMask.Mask);
                    const unsigned lp = std::thread::hardware_concurrency();
                    const unsigned tpc =
                        (a.physical_cores > 0 && lp > 0) ? (lp / a.physical_cores) : 0;
                    const std::size_t sharers =
                        (tpc > 0) ? std::max(1u, sharing_lp / tpc) : 0;
                    const std::size_t per_core = size / (sharers ? sharers : 1);

                    if (a.cache_line_bytes == 0 && c.LineSize != 0) {
                        a.cache_line_bytes = c.LineSize;
                    }
                    if (c.Level == 1 && c.Type == CacheData) {
                        if (per_core < best_l1) {
                            best_l1 = per_core;
                            a.l1d_bytes = size;
                            a.l1d_sharing_cores = sharers;
                            a.l1d_assoc = c.Associativity;
                        }
                    } else if (c.Level == 2) {
                        if (per_core < best_l2) {
                            best_l2 = per_core;
                            a.l2_bytes = size;
                            a.l2_sharing_cores = sharers;
                        }
                    } else if (c.Level == 3) {
                        if (per_core < best_l3) {
                            best_l3 = per_core;
                            a.l3_bytes = size;
                            a.l3_sharing_cores = sharers;
                        }
                    }
                }
            } else if (info->Relationship == RelationProcessorCore) {
                ++cores;
            } else if (info->Relationship == RelationNumaNode) {
                ++nodes;
            }

            off += info->Size;
        }

        // Cores are counted before caches are interpreted, since the sharing
        // divisor needs threads-per-core. The loop order above (RelationCache
        // first) means the first pass sees physical_cores == 0 and records
        // sharing as unknown; the core pass then fills it in and a second cache
        // pass would resolve it. Rather than iterate twice, cores are gathered
        // here and the caller re-runs the cache pass once cores are known.
        if (cores > 0) a.physical_cores = cores;
        if (nodes > 0) a.numa_domains = nodes;
    }
}

inline std::string brand_registry() {
    HKEY key{};
    if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                        KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    char buf[256] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const LSTATUS rc =
        ::RegQueryValueExA(key, "ProcessorNameString", nullptr, &type,
                           reinterpret_cast<LPBYTE>(buf), &size);
    ::RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return {};
    std::string s(buf);
    const std::size_t first = s.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return s.substr(first);
}

#endif  // _WIN32

}  // namespace detect

/// Detect the host CPU. Best effort: any field that could not be determined
/// stays 0 (or empty), and callers must fall back rather than believe the zero.
inline device_attributes detect_cpu() {
    device_attributes a = minimal_cpu_attributes();

#if defined(__linux__)
    detect::fill_caches_sysfs(a);
    a.source = "sysfs";
#  if PPE_HAS_X86_CPUID
    // Last resort if sysfs is unavailable (a stripped container). Per-current-
    // core and sharing-blind, which is why it is the fallback and not the
    // default.
    if (a.l1d_bytes == 0) {
        detect::fill_caches_x86(a);
        a.source = "cpuid";
    }
#  endif
    a.name = detect::brand_proc_cpuinfo();
#elif defined(__APPLE__)
    detect::fill_caches_apple(a);
    a.source = "sysctl";
    a.name = detect::sysctl_string("machdep.cpu.brand_string");
    if (a.name.empty()) a.name = detect::sysctl_string("hw.model");
#elif defined(_WIN32)
    // Two passes: the cache sharing divisor needs threads-per-core, which is
    // only known once physical cores have been counted.
    detect::fill_caches_win32(a);
    detect::fill_caches_win32(a);
    a.source = "win32";
    a.name = detect::brand_registry();
#elif PPE_HAS_X86_CPUID
    detect::fill_caches_x86(a);
    a.source = "cpuid";
#endif

#if PPE_HAS_X86_CPUID
    a.vendor = detect::vendor_string_x86();
    if (a.name.empty()) a.name = detect::brand_string_x86();
#endif

    if (a.name.empty()) a.name = std::string("host CPU (") + build_isa() + ")";
    return a;
}

/// Detect once per process and reuse. The hierarchy cannot change under a
/// running binary, and the sysfs reads should not sit in any hot path.
/// Thread-safe initialization is guaranteed by the static local.
inline const device_attributes& cached_cpu() {
    static const device_attributes a = detect_cpu();
    return a;
}

}  // namespace ppe
