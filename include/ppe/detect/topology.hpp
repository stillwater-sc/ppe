// topology.hpp -- the machine as a structure, not a single set of numbers (#6).
//
// WHY THIS EXISTS. ppe::device_attributes holds ONE l1d_bytes, ONE l2_bytes.
// detect/cpu.hpp keeps, per level, the entry with the smallest per-core budget
// across the CPUs the process may run on -- the right call for a model that must
// not overflow whichever core the work lands on, and precisely wrong for
// describing a machine. On an i7-12700K that reports 32 KiB L1d and 2 MiB L2
// (the E-cluster) unpinned, and 48 KiB / 1.25 MiB pinned to a P-core. Both are
// correct. Neither is the machine.
//
// A CLUSTER IS THE SET OF CORES SHARING ONE L2 INSTANCE. That definition is what
// separates the cases worth seeing:
//
//   Alder Lake      8 P-clusters of 1 core (private 1.25 MiB L2), plus
//                   1 E-cluster of 4 cores sharing 2 MiB, all under one L3
//   Jetson Orin Nano  two clusters, one of 4 cores and one of 2
//   Apple silicon   P and E clusters, 128-byte lines, no L3
//
// Grouping by core TYPE alone would merge a 4+2 machine into one group of six
// and lose exactly the organisation this is for. Grouping by L2 instance keeps
// them apart, and identical clusters are collapsed at render time with a
// multiplier rather than in the data.
//
// AFFINITY. Unlike detect_cpu(), this describes the WHOLE MACHINE rather than
// the cores this process may run on. A topology report that changed under
// taskset would be describing the scheduler's permissions, not the hardware.
#pragma once

#include <ppe/detect/cpu.hpp>
#include <ppe/platform.hpp>

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

// <set> and <utility> are needed by the Linux AND Windows backends, so they are
// unconditional. They were once inside the __linux__ branch, which built fine on
// the machine this was written on and broke both Windows jobs: a
// platform-conditional include has to cover every platform that uses the symbol,
// and "it compiles here" does not check that.
#if defined(__linux__)
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

/// Cores sharing one L2 instance.
struct core_cluster {
    std::vector<int> cpu_ids;            ///< logical processor ids
    unsigned    physical_cores = 0;      ///< SMT siblings collapsed
    unsigned    logical_processors = 0;
    int         package = 0;

    std::size_t l1d_bytes = 0;
    std::size_t l1i_bytes = 0;
    std::size_t l2_bytes = 0;
    std::size_t l1d_sharing_cores = 0;
    std::size_t l2_sharing_cores = 0;

    /// Linux cpu_capacity, or a synthesized rank elsewhere. 0 = unknown.
    /// Higher is faster; it is what separates big from LITTLE.
    std::size_t capacity = 0;

    /// "performance" / "efficiency" / "" when the machine is homogeneous.
    std::string role;

    /// True when this cluster's shape matches another's -- used to collapse
    /// eight identical P-core clusters into one rendered line.
    bool same_shape_as(const core_cluster& o) const {
        return physical_cores == o.physical_cores &&
               logical_processors == o.logical_processors &&
               l1d_bytes == o.l1d_bytes && l1i_bytes == o.l1i_bytes &&
               l2_bytes == o.l2_bytes && l2_sharing_cores == o.l2_sharing_cores &&
               capacity == o.capacity && package == o.package;
    }
};

struct platform_topology {
    std::string name;        ///< CPU brand
    std::string vendor;
    std::string source;      ///< "sysfs", "sysctl", "win32", or ""

    unsigned packages = 0;
    unsigned numa_domains = 0;
    unsigned physical_cores = 0;
    unsigned logical_processors = 0;

    std::size_t l3_bytes = 0;
    std::size_t l3_sharing_cores = 0;
    std::size_t cache_line_bytes = 0;

    /// Where cluster `capacity` came from, since the role labels are only as
    /// trustworthy as it is. "cpu_capacity", "acpi_cppc", "cpufreq",
    /// "perflevel", "efficiency_class", or "" when nothing was found.
    std::string capacity_source;

    std::vector<core_cluster> clusters;

    bool heterogeneous() const {
        for (std::size_t i = 1; i < clusters.size(); ++i) {
            if (clusters[i].capacity != clusters[0].capacity ||
                clusters[i].l2_bytes != clusters[0].l2_bytes ||
                clusters[i].l1d_bytes != clusters[0].l1d_bytes) {
                return true;
            }
        }
        return false;
    }
};

namespace detect {

/// Assign "performance" / "efficiency" roles once every cluster is known.
///
/// Only when the machine is actually heterogeneous: labelling every cluster of
/// a uniform part "performance" would imply a distinction that does not exist.
inline void assign_roles(platform_topology& t) {
    if (t.clusters.size() < 2) return;

    std::size_t hi = 0, lo = static_cast<std::size_t>(-1);
    for (const core_cluster& c : t.clusters) {
        if (c.capacity == 0) return;  // no capacity data: do not guess
        hi = std::max(hi, c.capacity);
        lo = std::min(lo, c.capacity);
    }
    if (hi == lo) return;  // homogeneous

    for (core_cluster& c : t.clusters) {
        c.role = (c.capacity == hi) ? "performance"
                                    : (c.capacity == lo ? "efficiency" : "mid");
    }
}

#if defined(__linux__)

/// A CPU's relative capability, from the best source available.
///
/// The canonical file is cpu_capacity, but it is an ARM/EAS thing and is absent
/// on x86 -- including on the hybrid x86 parts where the distinction matters
/// most. Falling back through ACPI CPPC and then cpufreq covers those: measured
/// on an i7-12700K, nominal_perf reads 45 on a P-core against 27 on an E-core,
/// and cpuinfo_max_freq 4.9 GHz against 3.8.
///
/// Writes the source it used into `src` so the report can say what the role
/// labels rest on rather than presenting an inference as a fact.
inline std::size_t cpu_capacity_of(int cpu, std::string& src) {
    const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/";
    if (const std::size_t v = parse_size(read_sysfs(base + "cpu_capacity")); v != 0) {
        src = "cpu_capacity";
        return v;
    }
    if (const std::size_t v = parse_size(read_sysfs(base + "acpi_cppc/nominal_perf"));
        v != 0) {
        src = "acpi_cppc";
        return v;
    }
    if (const std::size_t v = parse_size(read_sysfs(base + "cpufreq/cpuinfo_max_freq"));
        v != 0) {
        src = "cpufreq";
        return v;
    }
    return 0;
}

/// Every online CPU, from sysfs -- NOT the affinity mask.
inline std::vector<int> all_cpus() {
    std::vector<int> out;
    for (int i = 0; i < 4096; ++i) {
        const std::string path =
            "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/core_id";
        std::ifstream probe(path);
        if (!probe) continue;
        out.push_back(i);
    }
    return out;
}

struct cpu_caches {
    std::size_t l1d = 0, l1i = 0, l2 = 0, l3 = 0;
    std::size_t l1d_share = 0, l2_share = 0, l3_share = 0;
    std::string l2_shared_list;  ///< identity of this CPU's L2 instance
    std::size_t line = 0;
};

inline cpu_caches read_cpu_caches(int cpu) {
    cpu_caches c;
    const std::string base =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/index";
    for (int i = 0; i < 10; ++i) {
        const std::string dir = base + std::to_string(i);
        const std::string lvl = read_sysfs(dir + "/level");
        if (lvl.empty()) break;
        const std::string type = read_sysfs(dir + "/type");
        const std::size_t size = parse_size(read_sysfs(dir + "/size"));
        const std::string shared = read_sysfs(dir + "/shared_cpu_list");
        const std::size_t sharers = distinct_cores(parse_cpu_list(shared));
        const int level = std::atoi(lvl.c_str());
        if (c.line == 0) {
            c.line = parse_size(read_sysfs(dir + "/coherency_line_size"));
        }
        if (level == 1 && type == "Data") { c.l1d = size; c.l1d_share = sharers; }
        else if (level == 1 && type == "Instruction") { c.l1i = size; }
        else if (level == 2 && type != "Instruction") {
            c.l2 = size;
            c.l2_share = sharers;
            c.l2_shared_list = shared;
        } else if (level == 3 && type != "Instruction") {
            c.l3 = size;
            c.l3_share = sharers;
        }
    }
    return c;
}

inline platform_topology topology_sysfs() {
    platform_topology t;
    t.source = "sysfs";
    t.name = brand_proc_cpuinfo();

    const std::vector<int> cpus = all_cpus();
    t.logical_processors = static_cast<unsigned>(cpus.size());

    std::set<int> packages;
    // Cluster identity is the L2 instance's shared_cpu_list. CPUs whose L2 is
    // private each get their own cluster, which is correct: a P-core with a
    // private L2 IS a cluster of one.
    std::vector<std::string> keys;
    std::vector<core_cluster> clusters;

    for (const int cpu : cpus) {
        const cpu_caches cc = read_cpu_caches(cpu);
        const std::string topo =
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        const std::string pkg_s = read_sysfs(topo + "physical_package_id");
        const int pkg = pkg_s.empty() ? 0 : std::atoi(pkg_s.c_str());
        packages.insert(pkg);

        // Fall back to the cpu id when L2 has no shared list, so each such CPU
        // still forms its own cluster rather than all of them collapsing into
        // one anonymous group.
        const std::string key = cc.l2_shared_list.empty()
                                    ? ("cpu" + std::to_string(cpu))
                                    : cc.l2_shared_list;

        std::size_t idx = keys.size();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) { idx = i; break; }
        }
        if (idx == keys.size()) {
            keys.push_back(key);
            core_cluster nc;
            nc.package = pkg;
            nc.l1d_bytes = cc.l1d;
            nc.l1i_bytes = cc.l1i;
            nc.l2_bytes = cc.l2;
            nc.l1d_sharing_cores = cc.l1d_share;
            nc.l2_sharing_cores = cc.l2_share;
            nc.capacity = cpu_capacity_of(cpu, t.capacity_source);
            clusters.push_back(nc);
        }
        clusters[idx].cpu_ids.push_back(cpu);

        if (t.cache_line_bytes == 0 && cc.line != 0) t.cache_line_bytes = cc.line;
        if (cc.l3 != 0 && t.l3_bytes == 0) {
            t.l3_bytes = cc.l3;
            t.l3_sharing_cores = cc.l3_share;
        }
    }

    for (core_cluster& c : clusters) {
        c.logical_processors = static_cast<unsigned>(c.cpu_ids.size());
        c.physical_cores = static_cast<unsigned>(distinct_cores(c.cpu_ids));
        t.physical_cores += c.physical_cores;
    }

    t.clusters = std::move(clusters);
    t.packages = static_cast<unsigned>(packages.size());

    unsigned nodes = 0;
    for (int n = 0; n < 256; ++n) {
        std::ifstream probe("/sys/devices/system/node/node" + std::to_string(n) +
                            "/cpulist");
        if (probe) ++nodes;
    }
    t.numa_domains = nodes;
    return t;
}

#endif  // __linux__

#if defined(__APPLE__)

/// macOS exposes performance LEVELS rather than clusters: hw.nperflevels, with
/// hw.perflevelN.* describing each. A perflevel is the closest thing available
/// to a cluster and is what distinguishes P from E on Apple silicon.
inline platform_topology topology_sysctl() {
    platform_topology t;
    t.source = "sysctl";
    t.name = sysctl_string("machdep.cpu.brand_string");
    if (t.name.empty()) t.name = sysctl_string("hw.model");

    t.logical_processors = static_cast<unsigned>(sysctl_size("hw.logicalcpu"));
    t.physical_cores = static_cast<unsigned>(sysctl_size("hw.physicalcpu"));
    t.cache_line_bytes = sysctl_size("hw.cachelinesize");
    t.l3_bytes = sysctl_size("hw.l3cachesize");
    t.packages = static_cast<unsigned>(std::max<std::size_t>(1, sysctl_size("hw.packages")));
    t.numa_domains = 1;

    t.capacity_source = "perflevel";
    const std::size_t levels = std::max<std::size_t>(1, sysctl_size("hw.nperflevels"));
    for (std::size_t i = 0; i < levels; ++i) {
        const std::string p = "hw.perflevel" + std::to_string(i) + ".";
        core_cluster c;
        c.physical_cores = static_cast<unsigned>(sysctl_size((p + "physicalcpu").c_str()));
        c.logical_processors =
            static_cast<unsigned>(sysctl_size((p + "logicalcpu").c_str()));
        c.l1d_bytes = sysctl_size((p + "l1dcachesize").c_str());
        c.l1i_bytes = sysctl_size((p + "l1icachesize").c_str());
        c.l2_bytes = sysctl_size((p + "l2cachesize").c_str());
        // hw.perflevelN.l1dcachesize is documented as the L1 data cache size
        // *for a CPU in this performance level* -- a per-CPU quantity, unlike
        // l2cachesize which describes the shared instance. So the sharing count
        // is 1 by the sysctl's own semantics, not by an assumption about the
        // hardware. Recorded explicitly rather than left at 0, which would
        // render as "sharing unknown" and imply a detection failure.
        if (c.l1d_bytes != 0) c.l1d_sharing_cores = 1;
        // Cores sharing an L2 within a perflevel: macOS reports
        // cpusperl2 on some versions; when absent, leave unknown rather than
        // assume private.
        c.l2_sharing_cores = sysctl_size((p + "cpusperl2").c_str());
        // perflevel 0 is the fastest by definition, so rank descending.
        c.capacity = levels - i;  // perflevel 0 is the fastest by definition
        if (c.physical_cores == 0 && levels == 1) {
            c.physical_cores = t.physical_cores;
            c.logical_processors = t.logical_processors;
            c.l1d_bytes = sysctl_size("hw.l1dcachesize");
            c.l2_bytes = sysctl_size("hw.l2cachesize");
        }
        if (c.physical_cores > 0) t.clusters.push_back(c);
    }
    return t;
}

#endif  // __APPLE__

#if defined(_WIN32)

/// Windows carries an EfficiencyClass per processor core (higher is faster) and
/// cache relationships with group masks. Clusters are formed by grouping cores
/// under a shared L2 mask, falling back to efficiency class when L2 is private.
inline platform_topology topology_win32() {
    platform_topology t;
    t.source = "win32";
    t.name = brand_registry();

    t.capacity_source = "efficiency_class";
    std::vector<std::pair<KAFFINITY, BYTE>> cores;  // mask, efficiency class
    std::vector<std::pair<KAFFINITY, std::size_t>> l1ds;
    std::vector<std::pair<KAFFINITY, std::size_t>> l1is;
    std::vector<std::pair<KAFFINITY, std::size_t>> l2s;
    KAFFINITY l3_mask = 0;
    std::set<int> nodes;

    for (const LOGICAL_PROCESSOR_RELATIONSHIP rel :
         {RelationProcessorCore, RelationCache, RelationNumaNode,
          RelationProcessorPackage}) {
        DWORD len = 0;
        ::GetLogicalProcessorInformationEx(rel, nullptr, &len);
        if (len == 0) continue;
        std::vector<unsigned char> buf(len);
        if (!::GetLogicalProcessorInformationEx(
                rel, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()),
                &len)) {
            continue;
        }
        for (DWORD off = 0; off < len;) {
            auto* info =
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
            if (info->Size == 0) break;
            if (info->Relationship == RelationProcessorCore) {
                cores.emplace_back(info->Processor.GroupMask[0].Mask,
                                   info->Processor.EfficiencyClass);
                ++t.physical_cores;
                t.logical_processors += popcount_affinity(info->Processor.GroupMask[0].Mask);
            } else if (info->Relationship == RelationCache) {
                const CACHE_RELATIONSHIP& c = info->Cache;
                // Level 1 was previously skipped entirely, so every Windows
                // cluster reported "L1d n/a". The instruction cache is kept
                // separately and must never be mistaken for L1d: they are the
                // same size on many parts, so the error would be invisible.
                if (c.Level == 1 && c.Type == CacheData) {
                    l1ds.emplace_back(c.GroupMask.Mask,
                                      static_cast<std::size_t>(c.CacheSize));
                } else if (c.Level == 1 && c.Type == CacheInstruction) {
                    l1is.emplace_back(c.GroupMask.Mask,
                                      static_cast<std::size_t>(c.CacheSize));
                } else if (c.Type == CacheInstruction) { /* higher-level I-cache */ }
                else if (c.Level == 2) {
                    l2s.emplace_back(c.GroupMask.Mask, static_cast<std::size_t>(c.CacheSize));
                } else if (c.Level == 3 && t.l3_bytes == 0) {
                    t.l3_bytes = c.CacheSize;
                    l3_mask = c.GroupMask.Mask;
                }
                if (t.cache_line_bytes == 0 && c.LineSize != 0) t.cache_line_bytes = c.LineSize;
            } else if (info->Relationship == RelationNumaNode) {
                nodes.insert(static_cast<int>(info->NumaNode.NodeNumber));
            } else if (info->Relationship == RelationProcessorPackage) {
                ++t.packages;
            }
            off += info->Size;
        }
    }
    t.numa_domains = static_cast<unsigned>(nodes.size());
    if (t.packages == 0) t.packages = 1;

    // Physical cores covered by a group mask. Cores, not logical processors:
    // an SMT pair shares its L1d, so counting processors would halve every
    // private cache on a hyperthreaded machine.
    auto cores_under = [&cores](KAFFINITY mask) {
        std::size_t n = 0;
        for (const auto& core : cores) {
            if ((core.first & mask) != 0) ++n;
        }
        return n;
    };

    // One cluster per L2 instance; cores are attached to the L2 whose mask
    // covers them.
    for (const auto& l2 : l2s) {
        core_cluster c;
        c.l2_bytes = l2.second;
        for (const auto& core : cores) {
            if ((core.first & l2.first) != 0) {
                ++c.physical_cores;
                c.logical_processors += popcount_affinity(core.first);
                // EfficiencyClass: higher is faster on Windows.
                c.capacity = std::max<std::size_t>(c.capacity, core.second + 1);
            }
        }
        c.l2_sharing_cores = cores_under(l2.first);

        // Attach the L1 instances belonging to this cluster. Any L1d whose mask
        // overlaps the L2's belongs to a core in this cluster; its own mask
        // gives the real sharing count, which is 1 for a private L1d and more
        // on the rare part that shares one.
        for (const auto& l1 : l1ds) {
            if ((l1.first & l2.first) != 0) {
                c.l1d_bytes = l1.second;
                c.l1d_sharing_cores = cores_under(l1.first);
                break;
            }
        }
        for (const auto& l1 : l1is) {
            if ((l1.first & l2.first) != 0) {
                c.l1i_bytes = l1.second;
                break;
            }
        }

        if (c.physical_cores > 0) t.clusters.push_back(c);
    }

    // L3 sharing, from the same mask arithmetic. Previously left at 0, which
    // renders as "sharing unknown" on a machine where it is perfectly knowable.
    if (l3_mask != 0) t.l3_sharing_cores = cores_under(l3_mask);
    return t;
}

#endif  // _WIN32

}  // namespace detect

/// Detect the machine's topology. Best effort; an empty cluster list means
/// nothing could be determined and callers must not invent one.
inline platform_topology detect_topology() {
    platform_topology t;
#if defined(__linux__)
    t = detect::topology_sysfs();
#elif defined(__APPLE__)
    t = detect::topology_sysctl();
#elif defined(_WIN32)
    t = detect::topology_win32();
#endif
#if PPE_HAS_X86_CPUID
    if (t.vendor.empty()) t.vendor = detect::vendor_string_x86();
    if (t.name.empty()) t.name = detect::brand_string_x86();
#endif
    if (t.name.empty()) t.name = std::string("host CPU (") + build_isa() + ")";
    detect::assign_roles(t);
    return t;
}

}  // namespace ppe
