// accelerator.hpp -- GPU and KPU attributes, in the same shape as the CPU's.
//
// This is what makes PPE a PLATFORM performance engineering repository rather
// than a CPU one. The framing is unchanged from docs/architecture: a device is a
// hierarchy of levels, each with capacity, and the interesting differences
// between a CPU, a GPU and a KPU are what sits at each level and how many
// engines share it.
//
// NO SDK AT BUILD TIME. PPE builds on four platforms with a compiler and CMake
// and nothing else, and that must not change to detect a GPU. So:
//
//   * PCI enumeration (Linux /sys/class/drm) identifies a GPU with no vendor
//     software at all -- the answer to "is there one, and whose".
//   * The vendor runtime, if installed, is opened at RUN time with dlopen and
//     called through hand-declared prototypes. No headers, no link dependency,
//     and a machine without drivers reports "not detected" instead of failing
//     to build.
//
// A MISSING DEVICE IS A NORMAL OUTCOME. Most machines have no discrete GPU and
// almost none have a KPU. Detection reports what it found; absence is data, not
// an error.
//
// THE KPU IS NOT PROBED, IT IS CONFIGURED. A KPU is a domain-flow architecture
// that today exists as the kpu-sim simulator and as RTL, not as a device on a
// bus. Its attributes come from a kpu-sim system configuration
// (kpu-sim/configs/systems/*.json), which already describes exactly the
// hierarchy this repository models: memory banks with bandwidth and latency, L3
// tiles, L2 banks, scratchpads, and a systolic compute fabric. Reading that file
// is the data-level coupling docs/plans/first-application.md describes for
// mtl5 -- no shared library, no build dependency, just a format.
#pragma once

#include <ppe/json.hpp>
#include <ppe/platform.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

// The vendor runtime is opened at run time, which needs the platform's dynamic
// loader header on EVERY platform that has a path below -- Windows included.
// Omitting the Windows branch here is what broke both Windows jobs; see
// tools/lint/platform_includes.py.
#if defined(__linux__) || defined(__APPLE__)
#  include <dlfcn.h>
#  include <unistd.h>
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

/// One level of an accelerator's memory hierarchy.
struct accel_memory_level {
    std::string name;            ///< "HBM", "L3 tile", "L2 bank", "scratchpad"
    std::size_t bytes = 0;       ///< total across instances
    unsigned    instances = 0;   ///< how many of them
    double      bandwidth_gbs = 0.0;   ///< 0 = unknown
    double      latency_ns = 0.0;      ///< 0 = unknown
};

/// A compute engine group: SMs on a GPU, systolic tiles on a KPU.
struct accel_compute {
    std::string kind;            ///< "SM", "CU", "systolic"
    unsigned    count = 0;
    unsigned    rows = 0;        ///< systolic geometry, 0 when not applicable
    unsigned    cols = 0;
    std::string datatype;        ///< the fabric's native type, when stated
};

struct accelerator {
    device_kind kind = device_kind::gpu;
    std::string vendor;
    std::string name;
    std::string source;          ///< "drm", "cuda", "kpu-config"

    /// PCI ids, when the device sits on a bus. 0 for a simulated KPU.
    std::uint16_t pci_vendor = 0;
    std::uint16_t pci_device = 0;

    std::vector<accel_compute> compute;
    std::vector<accel_memory_level> memory;

    std::size_t total_memory_bytes = 0;
    double      clock_mhz = 0.0;
    std::string capability;      ///< "sm_86", or a config's schema version

    /// Set when the device was identified but its attributes could not be read
    /// -- a GPU with no driver installed, for instance. The distinction between
    /// "no GPU" and "a GPU I cannot interrogate" matters to anyone deciding
    /// whether a number is missing or a machine is.
    std::string note;
};

namespace detect {

// ---------------------------------------------------------------------------
// PCI vendor identification, no driver required
// ---------------------------------------------------------------------------

inline const char* pci_vendor_name(std::uint16_t id) {
    switch (id) {
        case 0x10de: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
        case 0x13b5: return "ARM";
        case 0x5143: return "Qualcomm";
        case 0x1ed3: return "Stillwater";  // reserved for KPU hardware
        default: return "unknown";
    }
}

#if defined(__linux__)

inline std::string read_file_trimmed(const std::string& path) {
    std::ifstream in(path);
    std::string s;
    if (in) std::getline(in, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

/// Which kernel driver is bound, e.g. "i915", "xe", "amdgpu", "nvidia".
///
/// The driver decides which sysfs attributes exist, so it is the key to reading
/// any of them -- and it is more reliable than the PCI id for that purpose,
/// since Intel's i915 and xe expose different trees for the same silicon.
inline std::string drm_driver(const std::string& device_dir) {
    char buf[512];
    const std::string link = device_dir + "driver";
    const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string s(buf);
    const std::size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

/// PCI address of a DRM device, e.g. "0000:00:02.0".
inline std::string drm_pci_address(int card) {
    char buf[512];
    const std::string link = "/sys/class/drm/card" + std::to_string(card) + "/device";
    const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string s(buf);
    const std::size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

/// Intel attributes from the i915 / xe sysfs trees.
///
/// FREQUENCIES ONLY, and that is the honest limit: neither driver publishes the
/// execution-unit count, which is the number an occupancy or roofline model
/// actually wants. Getting it needs Level Zero (a runtime dependency) or a PCI
/// id table (a database that is wrong for every part released after it was
/// written). Reporting the clocks and saying what is missing beats inventing an
/// EU count from a lookup table.
///
/// i915 puts them at the card root; xe moved them under device/tile0/gt0/freq0.
/// Both are tried, because the same silicon reports through either depending on
/// which driver the kernel bound.
inline void fill_intel_gpu(int card, const std::string& device_dir, accelerator& a) {
    const std::string root = "/sys/class/drm/card" + std::to_string(card) + "/";
    struct { const char* path; const char* label; } candidates[] = {
        {"gt_RP0_freq_mhz", "max"},      // i915: RP0 is the hardware maximum
        {"gt_max_freq_mhz", "max"},
        {"device/tile0/gt0/freq0/rp0_freq", "max"},  // xe
        {"gt_RPn_freq_mhz", "min"},
        {"gt_min_freq_mhz", "min"},
        {"device/tile0/gt0/freq0/rpn_freq", "min"},
    };
    double max_mhz = 0.0, min_mhz = 0.0;
    for (const auto& c : candidates) {
        const std::string v = read_file_trimmed(root + c.path);
        if (v.empty()) continue;
        const double mhz = std::strtod(v.c_str(), nullptr);
        if (mhz <= 0.0) continue;
        if (std::string(c.label) == "max" && max_mhz == 0.0) max_mhz = mhz;
        if (std::string(c.label) == "min" && min_mhz == 0.0) min_mhz = mhz;
    }
    if (max_mhz > 0.0) {
        a.clock_mhz = max_mhz;
        a.source = drm_driver(device_dir);
        char note[256];
        std::snprintf(note, sizeof(note),
                      "GT clock %.0f-%.0f MHz; execution-unit count is not published "
                      "by this driver (needs Level Zero)",
                      min_mhz, max_mhz);
        a.note = note;
    }
}

/// The fields of a KFD topology node this reader consumes.
struct kfd_node {
    unsigned long long simd_count = 0;
    unsigned long long simd_per_cu = 0;
    unsigned long long clk_khz = 0;        ///< max_engine_clk_fcompute
    unsigned long long lds_kb = 0;
    unsigned long long location_id = 0;
    unsigned long long wave_front_size = 0;
};

/// Parse a KFD node `properties` file: whitespace-separated key/value lines.
///
/// Split out from the sysfs walk so it can be tested without an AMD GPU. The
/// machine this is developed on has none and neither does any CI runner, so the
/// alternative was shipping a parser nobody had ever run -- see tests/kfd.cpp.
inline kfd_node parse_kfd_properties(std::istream& in) {
    kfd_node k;
    std::string key;
    unsigned long long value = 0;
    while (in >> key >> value) {
        if (key == "simd_count") k.simd_count = value;
        else if (key == "simd_per_cu") k.simd_per_cu = value;
        else if (key == "max_engine_clk_fcompute") k.clk_khz = value;
        else if (key == "lds_size_in_kb") k.lds_kb = value;
        else if (key == "location_id") k.location_id = value;
        else if (key == "wave_front_size") k.wave_front_size = value;
    }
    return k;
}

/// Fold a parsed KFD node into an accelerator record.
inline void apply_kfd_node(const kfd_node& k, accelerator& a) {
    accel_compute c;
    c.kind = "CU";
    // simd_count counts SIMDs, not compute units: a CU contains simd_per_cu of
    // them (2 on RDNA, 4 on GCN). Reporting simd_count as a CU count would
    // overstate the geometry by that factor.
    c.count = k.simd_per_cu > 0 ? static_cast<unsigned>(k.simd_count / k.simd_per_cu)
                                : static_cast<unsigned>(k.simd_count);
    a.compute.push_back(c);

    // max_engine_clk_fcompute is in kHz.
    if (k.clk_khz > 0) a.clock_mhz = static_cast<double>(k.clk_khz) / 1000.0;

    if (k.lds_kb > 0) {
        accel_memory_level lvl;
        lvl.name = "LDS/CU";
        lvl.bytes = static_cast<std::size_t>(k.lds_kb) * 1024u;
        lvl.instances = c.count;
        a.memory.push_back(std::move(lvl));
    }
    a.source = "kfd";
    a.note.clear();
    if (k.wave_front_size > 0) {
        a.capability = "wave" + std::to_string(k.wave_front_size);
    }
}

/// AMD attributes from amdgpu sysfs and, when present, the KFD topology.
///
/// The KFD tree is the rich source -- it publishes compute-unit geometry and the
/// engine clock in a stable key/value format -- but it only exists when the
/// amdkfd driver is loaded, which a display-only or virtualized AMD GPU will not
/// have. amdgpu sysfs alone still gives VRAM size and its vendor.
inline void fill_amd_gpu(int card, const std::string& device_dir, accelerator& a,
                         const std::string& kfd_root =
                             "/sys/class/kfd/kfd/topology/nodes/") {
    const std::string vram = read_file_trimmed(device_dir + "mem_info_vram_total");
    if (!vram.empty()) {
        a.total_memory_bytes = std::strtoull(vram.c_str(), nullptr, 10);
        accel_memory_level lvl;
        lvl.name = read_file_trimmed(device_dir + "mem_info_vram_vendor");
        if (lvl.name.empty()) lvl.name = "VRAM";
        lvl.bytes = a.total_memory_bytes;
        lvl.instances = 1;
        a.memory.push_back(std::move(lvl));
    }

    // Match this card to a KFD node by location_id, which encodes the PCI
    // bus/device/function. Matching by enumeration order would attach the wrong
    // node on a machine with more than one AMD GPU -- exactly the machine where
    // it matters.
    const std::string addr = drm_pci_address(card);
    unsigned want_loc = 0;
    if (addr.size() >= 12) {
        const unsigned bus = static_cast<unsigned>(std::strtoul(addr.substr(5, 2).c_str(), nullptr, 16));
        const unsigned dev = static_cast<unsigned>(std::strtoul(addr.substr(8, 2).c_str(), nullptr, 16));
        const unsigned fn = static_cast<unsigned>(std::strtoul(addr.substr(11, 1).c_str(), nullptr, 16));
        want_loc = (bus << 8) | (dev << 3) | fn;
    }

    for (int node = 0; node < 64; ++node) {
        const std::string props =
            kfd_root + std::to_string(node) + "/properties";
        std::ifstream in(props);
        if (!in) continue;

        const kfd_node k = parse_kfd_properties(in);
        // simd_count is 0 on the CPU node the KFD topology also publishes.
        if (k.simd_count == 0) continue;
        if (want_loc != 0 && k.location_id != want_loc) continue;

        apply_kfd_node(k, a);
        break;
    }
    if (a.source != "kfd" && a.note.empty()) {
        a.note = "amdgpu sysfs only; load amdkfd for compute-unit geometry";
    }
}

/// Enumerate GPUs via the DRM subsystem.
///
/// This is the floor: it works on a machine with no vendor runtime, no CUDA, no
/// ROCm, and answers "is there a GPU and whose is it" from PCI ids alone.
inline std::vector<accelerator> gpus_from_drm() {
    std::vector<accelerator> out;
    for (int card = 0; card < 16; ++card) {
        const std::string base =
            "/sys/class/drm/card" + std::to_string(card) + "/device/";
        const std::string vendor_s = read_file_trimmed(base + "vendor");
        if (vendor_s.empty()) continue;

        accelerator a;
        a.kind = device_kind::gpu;
        a.source = "drm";
        a.pci_vendor = static_cast<std::uint16_t>(std::strtoul(vendor_s.c_str(), nullptr, 0));
        a.pci_device = static_cast<std::uint16_t>(
            std::strtoul(read_file_trimmed(base + "device").c_str(), nullptr, 0));
        a.vendor = pci_vendor_name(a.pci_vendor);

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s GPU [%04x:%04x]", a.vendor.c_str(),
                      a.pci_vendor, a.pci_device);
        a.name = buf;

        a.note = "identified from PCI ids; install the vendor runtime for attributes";

        // Vendor-specific sysfs, which needs no runtime at all. What each
        // vendor publishes differs enough that a common path would report the
        // intersection, which is almost nothing.
        if (a.pci_vendor == 0x8086) {
            fill_intel_gpu(card, base, a);
        } else if (a.pci_vendor == 0x1002) {
            fill_amd_gpu(card, base, a);
        } else {
            const std::string vram = read_file_trimmed(base + "mem_info_vram_total");
            if (!vram.empty()) {
                a.total_memory_bytes = std::strtoull(vram.c_str(), nullptr, 10);
            }
        }
        out.push_back(std::move(a));
    }
    return out;
}

#endif  // __linux__

// ---------------------------------------------------------------------------
// NVIDIA, through the CUDA driver API loaded at runtime
// ---------------------------------------------------------------------------
#if defined(__linux__) || defined(_WIN32)

/// CUdevice_attribute values used below. Taken from the CUDA driver API's
/// documented enum, declared here rather than included so that no CUDA SDK is
/// needed to build. These values are ABI-stable across CUDA versions.
enum : int {
    kCuAttrMaxSharedPerBlock = 8,
    kCuAttrWarpSize = 10,
    kCuAttrClockRate = 13,          // kHz
    kCuAttrMultiprocessorCount = 16,
    kCuAttrMemoryClockRate = 36,    // kHz
    kCuAttrGlobalMemoryBusWidth = 37,
    kCuAttrL2CacheSize = 38,
    kCuAttrComputeCapabilityMajor = 75,
    kCuAttrComputeCapabilityMinor = 76,
};

inline std::vector<accelerator> gpus_from_cuda() {
    std::vector<accelerator> out;

#if defined(__linux__)
    void* lib = ::dlopen("libcuda.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (lib == nullptr) lib = ::dlopen("libcuda.so", RTLD_LAZY | RTLD_LOCAL);
    if (lib == nullptr) return out;
    auto sym = [lib](const char* n) { return ::dlsym(lib, n); };
    auto close = [lib] { ::dlclose(lib); };
#else
    HMODULE lib = ::LoadLibraryA("nvcuda.dll");
    if (lib == nullptr) return out;
    auto sym = [lib](const char* n) -> void* {
        return reinterpret_cast<void*>(::GetProcAddress(lib, n));
    };
    auto close = [lib] { ::FreeLibrary(lib); };
#endif

    using cuInit_t = int (*)(unsigned);
    using cuDeviceGetCount_t = int (*)(int*);
    using cuDeviceGet_t = int (*)(int*, int);
    using cuDeviceGetName_t = int (*)(char*, int, int);
    using cuDeviceGetAttribute_t = int (*)(int*, int, int);
    using cuDeviceTotalMem_t = int (*)(std::size_t*, int);

    auto cu_init = reinterpret_cast<cuInit_t>(sym("cuInit"));
    auto cu_count = reinterpret_cast<cuDeviceGetCount_t>(sym("cuDeviceGetCount"));
    auto cu_get = reinterpret_cast<cuDeviceGet_t>(sym("cuDeviceGet"));
    auto cu_name = reinterpret_cast<cuDeviceGetName_t>(sym("cuDeviceGetName"));
    auto cu_attr = reinterpret_cast<cuDeviceGetAttribute_t>(sym("cuDeviceGetAttribute"));
    // The _v2 suffix is the current ABI for this call; the unsuffixed symbol is
    // the pre-CUDA-3.2 32-bit version and would truncate memory sizes.
    auto cu_mem = reinterpret_cast<cuDeviceTotalMem_t>(sym("cuDeviceTotalMem_v2"));

    if (cu_init == nullptr || cu_count == nullptr || cu_get == nullptr ||
        cu_name == nullptr || cu_attr == nullptr) {
        close();
        return out;
    }
    // A driver present but no device, or a driver too old, both land here.
    if (cu_init(0) != 0) { close(); return out; }

    int n = 0;
    if (cu_count(&n) != 0 || n <= 0) { close(); return out; }

    for (int i = 0; i < n; ++i) {
        int dev = 0;
        if (cu_get(&dev, i) != 0) continue;

        accelerator a;
        a.kind = device_kind::gpu;
        a.vendor = "NVIDIA";
        a.source = "cuda";
        a.pci_vendor = 0x10de;

        char namebuf[256] = {};
        if (cu_name(namebuf, sizeof(namebuf) - 1, dev) == 0) a.name = namebuf;
        if (a.name.empty()) a.name = "NVIDIA GPU";

        auto attr = [&](int which) {
            int v = 0;
            return cu_attr(&v, which, dev) == 0 ? v : 0;
        };

        accel_compute c;
        c.kind = "SM";
        c.count = static_cast<unsigned>(attr(kCuAttrMultiprocessorCount));
        a.compute.push_back(c);

        a.clock_mhz = attr(kCuAttrClockRate) / 1000.0;

        const int major = attr(kCuAttrComputeCapabilityMajor);
        const int minor = attr(kCuAttrComputeCapabilityMinor);
        if (major > 0) a.capability = "sm_" + std::to_string(major) + std::to_string(minor);

        if (cu_mem != nullptr) {
            std::size_t bytes = 0;
            if (cu_mem(&bytes, dev) == 0) a.total_memory_bytes = bytes;
        }

        // Peak DRAM bandwidth from the memory clock and bus width. This is the
        // theoretical figure the hardware is specified at, not a measurement --
        // the same distinction the CPU peak model draws, and worth keeping
        // visible for the same reason.
        const double mem_clock_khz = attr(kCuAttrMemoryClockRate);
        const double bus_bits = attr(kCuAttrGlobalMemoryBusWidth);
        double peak_bw = 0.0;
        if (mem_clock_khz > 0.0 && bus_bits > 0.0) {
            peak_bw = mem_clock_khz * 1e3 * 2.0 * bus_bits / 8.0 / 1e9;  // DDR
        }

        accel_memory_level dram;
        dram.name = "device memory";
        dram.bytes = a.total_memory_bytes;
        dram.instances = 1;
        dram.bandwidth_gbs = peak_bw;
        a.memory.push_back(dram);

        if (const int l2 = attr(kCuAttrL2CacheSize); l2 > 0) {
            accel_memory_level lvl;
            lvl.name = "L2";
            lvl.bytes = static_cast<std::size_t>(l2);
            lvl.instances = 1;
            a.memory.push_back(lvl);
        }
        if (const int smem = attr(kCuAttrMaxSharedPerBlock); smem > 0) {
            accel_memory_level lvl;
            lvl.name = "shared/block";
            lvl.bytes = static_cast<std::size_t>(smem);
            lvl.instances = c.count;
            a.memory.push_back(lvl);
        }
        out.push_back(std::move(a));
    }

    close();
    return out;
}

#else  // no CUDA path on this platform

inline std::vector<accelerator> gpus_from_cuda() { return {}; }

#endif

// ---------------------------------------------------------------------------
// KPU, from a kpu-sim system configuration
// ---------------------------------------------------------------------------

/// Sum an array of {capacity_kb|capacity_mb} entries into a level.
inline accel_memory_level level_from_array(const json::value& arr, const char* name,
                                           const char* kb_key, const char* mb_key) {
    accel_memory_level lvl;
    lvl.name = name;
    for (const json::value& e : arr.items()) {
        ++lvl.instances;
        if (kb_key != nullptr && !e[kb_key].is_null()) {
            lvl.bytes += e[kb_key].size_bytes_from_kb();
        } else if (mb_key != nullptr && !e[mb_key].is_null()) {
            lvl.bytes += e[mb_key].size_bytes_from_mb();
        }
        // Bandwidth and latency, where the config states them, are per instance
        // and identical across instances in every shipped config; taking the
        // first is right until one of them is heterogeneous, at which point this
        // needs per-instance records rather than a sum.
        if (lvl.bandwidth_gbs == 0.0) lvl.bandwidth_gbs = e["bandwidth_gbps"].number();
        if (lvl.latency_ns == 0.0) lvl.latency_ns = e["latency_ns"].number();
    }
    return lvl;
}

/// Parse a kpu-sim system configuration into accelerators.
///
/// Reads the shape kpu-sim documents in configs/schema.md: accelerators[] with
/// kpu_config.memory.{banks,l3_tiles,l2_banks,scratchpads} and
/// compute_fabric.tiles[]. Unknown fields are ignored, so a newer config does
/// not break an older reader.
inline std::vector<accelerator> kpus_from_config(const std::string& path,
                                                 std::string* error = nullptr) {
    std::vector<accelerator> out;

    std::ifstream in(path);
    if (!in) {
        if (error != nullptr) *error = "cannot open " + path;
        return out;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    const json::parse_result pr = json::parse(ss.str());
    if (!pr.ok) {
        if (error != nullptr) {
            *error = path + ": " + pr.error + " at byte " + std::to_string(pr.offset);
        }
        return out;
    }

    const std::string system_name = pr.root["system"]["name"].str("KPU system");

    for (const json::value& acc : pr.root["accelerators"].items()) {
        if (acc["type"].str() != "KPU") continue;

        accelerator a;
        a.kind = device_kind::kpu;
        a.vendor = "Stillwater";
        a.source = "kpu-config";
        a.name = system_name + " / " + acc["id"].str("kpu");
        a.note = acc["description"].str();

        const json::value& mem = acc["kpu_config"]["memory"];
        if (!mem["banks"].is_null()) {
            accel_memory_level banks =
                level_from_array(mem["banks"], "memory bank", nullptr, "capacity_mb");
            banks.name = mem["type"].str("memory") + " bank";
            a.total_memory_bytes = banks.bytes;
            a.memory.push_back(std::move(banks));
        }
        if (!mem["l3_tiles"].is_null()) {
            a.memory.push_back(
                level_from_array(mem["l3_tiles"], "L3 tile", "capacity_kb", nullptr));
        }
        if (!mem["l2_banks"].is_null()) {
            a.memory.push_back(
                level_from_array(mem["l2_banks"], "L2 bank", "capacity_kb", nullptr));
        }
        if (!mem["scratchpads"].is_null()) {
            a.memory.push_back(
                level_from_array(mem["scratchpads"], "scratchpad", "capacity_kb", nullptr));
        }

        for (const json::value& tile : acc["kpu_config"]["compute_fabric"]["tiles"].items()) {
            accel_compute c;
            c.kind = tile["type"].str("tile");
            c.count = 1;
            c.rows = static_cast<unsigned>(tile["systolic_rows"].number());
            c.cols = static_cast<unsigned>(tile["systolic_cols"].number());
            c.datatype = tile["datatype"].str();

            // Collapse identical tiles: a fabric of sixteen 16x16 systolic tiles
            // should read as that, not as sixteen entries.
            bool merged = false;
            for (accel_compute& e : a.compute) {
                if (e.kind == c.kind && e.rows == c.rows && e.cols == c.cols &&
                    e.datatype == c.datatype) {
                    ++e.count;
                    merged = true;
                    break;
                }
            }
            if (!merged) a.compute.push_back(std::move(c));
        }
        out.push_back(std::move(a));
    }

    if (out.empty() && error != nullptr) {
        *error = path + ": parsed, but it declares no KPU accelerator";
    }
    return out;
}

}  // namespace detect

/// Detect every accelerator this machine exposes.
///
/// `kpu_config` is a path to a kpu-sim system configuration; empty means no KPU
/// is described, which is the normal case. PPE_KPU_CONFIG is honoured so a
/// machine with a standing simulator configuration does not need the flag.
inline std::vector<accelerator> detect_accelerators(const std::string& kpu_config = {}) {
    std::vector<accelerator> out;

    // The vendor runtime first: when both find the same device, the runtime's
    // record is the informative one and the PCI entry would be a duplicate.
    std::vector<accelerator> cuda = detect::gpus_from_cuda();
    const bool have_nvidia_runtime = !cuda.empty();
    for (accelerator& a : cuda) out.push_back(std::move(a));

#if defined(__linux__)
    for (accelerator& a : detect::gpus_from_drm()) {
        if (have_nvidia_runtime && a.pci_vendor == 0x10de) continue;
        out.push_back(std::move(a));
    }
#endif

    std::string path = kpu_config;
    if (path.empty()) {
        if (const char* env = std::getenv("PPE_KPU_CONFIG"); env != nullptr) path = env;
    }
    if (!path.empty()) {
        std::string err;
        for (accelerator& a : detect::kpus_from_config(path, &err)) {
            out.push_back(std::move(a));
        }
        if (!err.empty()) {
            accelerator a;
            a.kind = device_kind::kpu;
            a.source = "kpu-config";
            a.name = "KPU (configuration unreadable)";
            a.note = err;
            out.push_back(std::move(a));
        }
    }
    return out;
}

}  // namespace ppe
