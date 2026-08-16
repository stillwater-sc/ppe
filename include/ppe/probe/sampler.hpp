// sampler.hpp -- a sampling profiler built on perf_event.
//
// The counter backend answers "how many cycles"; this answers "where did they
// go". perf_event delivers a sample every N cycles containing the instruction
// pointer, and the profile is the histogram of those addresses.
//
// WHY SAMPLING RATHER THAN INSTRUMENTATION. ppe/trace.hpp already records spans
// a programmer chose to mark, at ~32 ns each. That answers questions about code
// someone suspected. Sampling answers questions about code nobody thought to
// mark, at a cost set by the sample rate rather than by how much was
// instrumented -- and it sees the leaf functions, the library calls, and the
// compiler's own choices, which no amount of hand-placed spans will.
//
// THREE THINGS THIS GETS WRONG IF DONE CARELESSLY:
//
//   THE RING BUFFER WRAPS. The kernel writes records into a circular buffer and
//   a record can straddle the end. Reading it as a flat array yields a record
//   whose second half is the buffer's beginning -- a plausible instruction
//   pointer from nowhere. Every read here is masked and reassembled.
//
//   LOST RECORDS ARE SILENT. When the buffer fills, the kernel drops samples
//   and reports the count in a PERF_RECORD_LOST record. A profile that ignores
//   those is a profile with invisible holes weighted toward whatever was
//   hottest -- exactly the region a reader is looking at. Lost counts are
//   collected and reported.
//
//   ATTRIBUTION IS NOT SYMBOLIZATION. An address means nothing without the
//   module it came from, and a module plus offset is not a function name.
//   dladdr resolves only DYNAMIC symbols, so the first version of this
//   symbolized 0% of a normal executable's samples and merged them all into one
//   4 KiB page -- two functions with a deliberate 4:1 work ratio appeared as a
//   single line. Symbols now come from the module's ELF .symtab
//   (ppe/probe/elf_symbols.hpp), with dladdr as a fallback and module+offset
//   below that. The report states what fraction it actually named.
//
// Sampling needs the same perf_event permissions as counting; see
// ppe/probe/counters.hpp for the paranoid levels.
#pragma once

#include <ppe/probe/counters.hpp>
#include <ppe/probe/elf_symbols.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#if defined(__linux__)
#  include <dlfcn.h>
#  include <sys/mman.h>
#endif

namespace ppe::probe {

/// One resolved sample location.
struct profile_site {
    std::string symbol;      ///< dynamic symbol name, or "" when unresolved
    std::string module;      ///< the mapped file the address fell in
    std::uint64_t offset = 0;  ///< offset within that module
    std::uint64_t samples = 0;
};

struct profile_result {
    bool ok = false;
    std::string note;

    std::uint64_t samples_collected = 0;
    std::uint64_t samples_lost = 0;      ///< dropped by the kernel, buffer full
    std::uint64_t records_seen = 0;

    /// True when the thread crossed core types while sampling. The event stays
    /// bound to the PMU it was opened on, so sampling STOPS on the other kind --
    /// silently, with no lost-record to show for it. Measured on a hybrid part:
    /// runs that migrated collected ~30 samples where pinned runs collected
    /// ~670, and the shortfall landed entirely on the colder function, which
    /// simply vanished from the profile.
    bool migrated_across_pmus = false;
    std::string migration_note;

    /// Sites ordered hottest first.
    std::vector<profile_site> sites;

    /// Fraction of samples that resolved to a symbol NAME rather than only a
    /// module and offset. Reported because a profile that resolved nothing looks
    /// identical to one with no hot spots.
    double symbolized_fraction = 0.0;
};

namespace detail {

/// One line of /proc/self/maps: an address range and the file behind it.
struct mapping {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t file_offset = 0;
    std::string path;
};

inline std::vector<mapping> read_self_maps() {
    std::vector<mapping> out;
#if defined(__linux__)
    std::ifstream in("/proc/self/maps");
    std::string line;
    while (std::getline(in, line)) {
        // "7f0a1c000000-7f0a1c021000 r-xp 00001000 08:02 1234 /usr/lib/libc.so.6"
        mapping m;
        char perms[8] = {};
        char path[1024] = {};
        const int n = std::sscanf(line.c_str(),
                                  "%lx-%lx %7s %lx %*s %*s %1023[^\n]",
                                  &m.begin, &m.end, perms, &m.file_offset, path);
        if (n < 4) continue;
        // Executable mappings only: a sample's instruction pointer cannot be in
        // a data mapping, and including them would let an address match the
        // wrong region.
        if (std::strchr(perms, 'x') == nullptr) continue;
        m.path = path;
        // Trim the leading spaces sscanf leaves before the path.
        const std::size_t first = m.path.find_first_not_of(' ');
        m.path = (first == std::string::npos) ? std::string{} : m.path.substr(first);
        out.push_back(std::move(m));
    }
#endif
    return out;
}

inline const mapping* find_mapping(const std::vector<mapping>& maps, std::uint64_t ip) {
    for (const mapping& m : maps) {
        if (ip >= m.begin && ip < m.end) return &m;
    }
    return nullptr;
}

}  // namespace detail

/// Samples the instruction pointer of the calling thread.
class sampler {
public:
    /// `frequency_hz` samples per second. The kernel adjusts the cycle period to
    /// hit it, so the rate holds across frequency changes.
    explicit sampler(unsigned frequency_hz = 1000) { open_sampler(frequency_hz); }
    ~sampler() { close_sampler(); }

    sampler(const sampler&) = delete;
    sampler& operator=(const sampler&) = delete;

    bool ok() const { return fd_ >= 0; }
    const std::string& note() const { return note_; }

    void start() {
#if defined(__linux__)
        if (fd_ < 0) return;
        start_cpu_ = ::sched_getcpu();
        ::ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
        ::ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
#endif
    }

    void stop() {
#if defined(__linux__)
        if (fd_ >= 0) ::ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
#endif
    }

    /// Drain the ring buffer and resolve what it holds.
    profile_result collect() {
        profile_result r;
#if !defined(__linux__)
        r.note = note_.empty() ? "sampling needs perf_event, which is Linux-only"
                               : note_;
        return r;
#else
        if (fd_ < 0 || ring_ == nullptr) {
            r.note = note_;
            return r;
        }

        auto* meta = static_cast<perf_event_mmap_page*>(ring_);
        const std::uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
        std::uint64_t tail = meta->data_tail;

        const std::size_t data_size = data_size_;
        auto* data = static_cast<unsigned char*>(ring_) + page_size_;

        std::map<std::uint64_t, std::uint64_t> counts;  // ip -> samples

        while (tail < head) {
            // A record may straddle the end of the circular buffer, so the
            // header is copied out before it is trusted.
            perf_event_header hdr;
            copy_from_ring(data, data_size, tail, &hdr, sizeof(hdr));
            if (hdr.size == 0 || hdr.size > data_size) break;  // corrupt: stop

            ++r.records_seen;
            if (hdr.type == PERF_RECORD_SAMPLE) {
                std::uint64_t ip = 0;
                copy_from_ring(data, data_size, tail + sizeof(hdr), &ip, sizeof(ip));
                ++counts[ip];
                ++r.samples_collected;
            } else if (hdr.type == PERF_RECORD_LOST) {
                // struct: u64 id, u64 lost
                std::uint64_t lost[2] = {0, 0};
                copy_from_ring(data, data_size, tail + sizeof(hdr), lost, sizeof(lost));
                r.samples_lost += lost[1];
            }
            tail += hdr.size;
        }

        // Publishing the tail is what frees the space; without it the buffer
        // fills and every later sample is lost.
        __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);

        resolve(counts, r);

        // A profile that silently stopped collecting is worse than no profile:
        // the samples it did get are real, so it looks like a thin but valid
        // answer rather than a truncated one.
        const int end_cpu = ::sched_getcpu();
        if (start_cpu_ >= 0 && end_cpu >= 0 && start_cpu_ != end_cpu) {
            std::string a, b;
            const int dom_a = cycle_counter::hybrid_pmu_type(start_cpu_, a);
            const int dom_b = cycle_counter::hybrid_pmu_type(end_cpu, b);
            if (dom_a != dom_b) {
                r.migrated_across_pmus = true;
                r.migration_note =
                    "the thread moved from cpu " + std::to_string(start_cpu_) + " (" + a +
                    ") to cpu " + std::to_string(end_cpu) + " (" + b +
                    ") while sampling; the event stays bound to the PMU it was opened "
                    "on, so sampling stopped and this profile is truncated -- pin with "
                    "taskset";
            }
        }

        r.ok = true;
        return r;
#endif
    }

private:
    int fd_ = -1;
    void* ring_ = nullptr;
    std::size_t page_size_ = 4096;
    std::size_t data_size_ = 0;
    int start_cpu_ = -1;
    std::string note_;

#if defined(__linux__)
    /// Copy `bytes` from the circular buffer starting at `offset`, wrapping.
    static void copy_from_ring(const unsigned char* data, std::size_t size,
                               std::uint64_t offset, void* out, std::size_t bytes) {
        auto* dst = static_cast<unsigned char*>(out);
        for (std::size_t i = 0; i < bytes; ++i) {
            dst[i] = data[(offset + i) % size];
        }
    }

    void open_sampler(unsigned frequency_hz) {
        const counter_support sup = counters_available();
        if (!sup.available) {
            note_ = "sampling needs the same permissions as counting: " + sup.note;
            return;
        }

        const int cpu = ::sched_getcpu();
        std::string pmu;
        const int hybrid = cpu >= 0 ? cycle_counter::hybrid_pmu_type(cpu, pmu) : -1;

        perf_event_attr attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = PERF_COUNT_HW_CPU_CYCLES;
        if (hybrid >= 0) attr.config |= static_cast<std::uint64_t>(hybrid) << 32;

        // freq=1 asks the kernel to adjust the period to hit sample_freq, so the
        // rate survives frequency scaling -- a fixed period would sample more
        // often when the core clocks up.
        attr.sample_freq = frequency_hz;
        attr.freq = 1;
        attr.sample_type = PERF_SAMPLE_IP;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        // Without this the samples are attributed to whatever the thread was
        // doing when the counter overflowed rather than to the instruction that
        // caused it; precise_ip asks for the PEBS-corrected address where the
        // hardware supports it, and degrades to 0 where it does not.
        attr.precise_ip = 2;

        fd_ = static_cast<int>(::syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
        if (fd_ < 0) {
            // precise_ip is not available on every part or in every VM; retry
            // without it rather than reporting the machine cannot sample.
            attr.precise_ip = 0;
            fd_ = static_cast<int>(::syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
        }
        if (fd_ < 0) {
            note_ = std::string("perf_event_open for sampling failed: ") +
                    std::strerror(errno);
            return;
        }

        page_size_ = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        // One metadata page plus a power-of-two data area, as the interface
        // requires.
        data_size_ = page_size_ * 64;
        ring_ = ::mmap(nullptr, page_size_ + data_size_, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd_, 0);
        if (ring_ == MAP_FAILED) {
            ring_ = nullptr;
            note_ = std::string("mmap of the sample ring buffer failed: ") +
                    std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
        }
    }

    void close_sampler() {
        if (ring_ != nullptr) ::munmap(ring_, page_size_ + data_size_);
        ring_ = nullptr;
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    /// Turn addresses into sites, hottest first.
    static void resolve(const std::map<std::uint64_t, std::uint64_t>& counts,
                        profile_result& r) {
        const std::vector<detail::mapping> maps = detail::read_self_maps();
        std::map<std::string, profile_site> merged;
        std::uint64_t symbolized = 0;

        // One symbol table per module, read once. Parsing an ELF per sample
        // would dominate the collection this is meant to report on.
        std::map<std::string, elf_symbol_table> tables;

        for (const auto& [ip, n] : counts) {
            profile_site s;
            s.samples = n;

            const detail::mapping* m = detail::find_mapping(maps, ip);
            if (m != nullptr) {
                s.module = m->path;
                s.offset = ip - m->begin + m->file_offset;

                if (!m->path.empty() && m->path[0] == '/') {
                    auto it = tables.find(m->path);
                    if (it == tables.end()) {
                        it = tables.emplace(m->path,
                                            detail::read_elf_symbols(m->path)).first;
                    }
                    if (it->second.ok) {
                        const std::uint64_t vaddr =
                            it->second.to_file_vaddr(ip, m->begin, m->file_offset);
                        if (const elf_symbol* sym = it->second.find(vaddr);
                            sym != nullptr) {
                            s.symbol = sym->name;
                        }
                    }
                }
            }

            // dladdr as a fallback: it sees dynamic symbols the ELF read may
            // have missed for a module whose file is no longer on disk.
            if (s.symbol.empty()) {
                Dl_info info;
                if (::dladdr(reinterpret_cast<void*>(ip), &info) != 0 &&
                    info.dli_sname != nullptr) {
                    s.symbol = info.dli_sname;
                }
            }
            if (!s.symbol.empty()) symbolized += n;

            // Merge by symbol when known. WITHOUT a symbol, merge by exact
            // address rather than by page: page granularity merged two distinct
            // functions into one line and hid the ratio between them.
            const std::string key =
                !s.symbol.empty() ? s.symbol
                                  : s.module + "+" + std::to_string(s.offset);
            auto it = merged.find(key);
            if (it == merged.end()) {
                merged.emplace(key, s);
            } else {
                it->second.samples += n;
            }
        }

        for (auto& [key, site] : merged) r.sites.push_back(site);
        std::sort(r.sites.begin(), r.sites.end(),
                  [](const profile_site& a, const profile_site& b) {
                      return a.samples > b.samples;
                  });
        r.symbolized_fraction =
            r.samples_collected > 0
                ? static_cast<double>(symbolized) / static_cast<double>(r.samples_collected)
                : 0.0;
    }
#else
    void open_sampler(unsigned) {
        note_ = "sampling needs perf_event, which is Linux-only";
    }
    void close_sampler() {}
#endif
};

}  // namespace ppe::probe
