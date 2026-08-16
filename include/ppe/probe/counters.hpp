// counters.hpp -- hardware performance counters, where the kernel permits them.
//
// This is the instrument docs/plans/first-application.md named when it deferred
// the sustained clock: "a perf_event backend on Linux, used when permissions
// allow and degrading to the claim when they do not, would give a genuine
// sustained figure on the machines this project actually measures on."
//
// WHY THE CLOCK NEEDS THIS. Everything else PPE reports about frequency is a
// claim: cpufreq's scaling_max_freq is a policy ceiling, Windows' ~MHz is a
// boot-time nominal, and Apple silicon publishes nothing. None of them is what
// the core actually ran at during a measurement, and under an AVX-heavy load
// with several cores busy the difference is large. CPU_CLK_UNHALTED counts the
// cycles that really happened; divided by wall time it is a sustained clock
// rather than a specification.
//
// PERMISSIONS ARE THE NORMAL FAILURE, NOT AN ERROR. Access is gated by
// /proc/sys/kernel/perf_event_paranoid:
//
//     -1  everything
//      0  + CPU-wide events
//      1  + kernel measurements
//      2  user-space measurements only -- the common default, and enough here,
//         because these counters set exclude_kernel
//    >=3  nothing for unprivileged users (Debian/Ubuntu hardening; this
//         development machine reports 4)
//
// So the backend must report *why* it could not measure, precisely enough to
// act on, and the caller must fall back to the claim rather than reporting a
// zero. A machine that denies counters is a normal machine.
//
// HYBRID CPUs NEED THE RIGHT PMU. Alder Lake and later expose two PMUs --
// cpu_core and cpu_atom -- and PERF_TYPE_HARDWARE binds to the P-core one. On
// an E-core the counter then OPENS SUCCESSFULLY AND COUNTS ZERO, which is the
// worst possible failure: no error to check, and a caller that treats zero as
// "unavailable" falls back to a claim while believing it tried. Measured here:
// 3,281,921 cycles on cpu4 against 0 on cpu16 for identical work. So the PMU is
// selected by which one lists the current CPU in its `cpus` file, and a zero
// count is reported as its own condition rather than folded into "denied".
//
// THE API EXISTS ON EVERY PLATFORM. Only the syscall is behind a guard: burying
// the type itself would make anything using it -- including its test -- compile
// on Linux alone, which is exactly the bug tools/lint/platform_includes.py was
// extended to catch.
#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#if defined(__linux__)
#  include <asm/unistd.h>
#  include <linux/perf_event.h>
#  include <sys/ioctl.h>
#  include <sched.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

namespace ppe::probe {

/// Why counters are or are not usable here.
struct counter_support {
    bool available = false;
    int  paranoid = -99;      ///< observed perf_event_paranoid, -99 if unread
    std::string note;         ///< actionable reason when unavailable
    std::string pmu;          ///< which PMU was opened: "cpu_core", "cpu_atom", "generic"
};

/// Counts unhalted core cycles on the calling thread.
///
/// Constructed open, closed on destruction. `ok()` is false when the kernel
/// refused, and `note()` says what to do about it.
class cycle_counter {
public:
    /// `event` is a PERF_COUNT_HW_* id. Cycles by default; the FMA probe also
    /// needs PERF_COUNT_HW_INSTRUCTIONS to check that the compiler emitted the
    /// loop it was asked for.
    explicit cycle_counter(unsigned long long event = 0 /* PERF_COUNT_HW_CPU_CYCLES */) {
        event_ = event;
        open_counter();
    }
    ~cycle_counter() { close_counter(); }

    cycle_counter(const cycle_counter&) = delete;
    cycle_counter& operator=(const cycle_counter&) = delete;

    bool ok() const { return fd_ >= 0; }
    const std::string& note() const { return note_; }
    int paranoid() const { return paranoid_; }
    const std::string& pmu() const { return pmu_; }

    void start() {
#if defined(__linux__)
        if (fd_ < 0) return;
        ::ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
        ::ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
#endif
    }

    /// Stop counting and return the cycles observed since start().
    std::uint64_t stop() {
#if defined(__linux__)
        if (fd_ < 0) return 0;
        ::ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
        std::uint64_t count = 0;
        const ssize_t n = ::read(fd_, &count, sizeof(count));
        // A short read means the counter was multiplexed away or the group
        // changed shape; reporting 0 is right, since a partial count is not a
        // count and scaling it needs TOTAL_TIME_ENABLED/RUNNING this does not
        // request.
        return n == static_cast<ssize_t>(sizeof(count)) ? count : 0;
#else
        return 0;
#endif
    }

private:
    int fd_ = -1;
    unsigned long long event_ = 0;
    int paranoid_ = -99;
    std::string note_;
    std::string pmu_ = "generic";

#if defined(__linux__)
public:
    /// The PMU covering `cpu`, on a hybrid part. Returns -1 when the machine is
    /// not hybrid, in which case PERF_TYPE_HARDWARE is correct.
    ///
    /// Each PMU publishes the CPUs it covers in its `cpus` file, so this asks
    /// the kernel which one owns this core rather than inferring it from a
    /// core-type heuristic that would need updating per microarchitecture.
    static int hybrid_pmu_type(int cpu, std::string& name) {
        for (const char* pmu : {"cpu_core", "cpu_atom"}) {
            const std::string base = std::string("/sys/bus/event_source/devices/") + pmu;
            std::ifstream cpus(base + "/cpus");
            if (!cpus) continue;
            std::string list;
            std::getline(cpus, list);
            if (!cpu_list_contains(list, cpu)) continue;
            std::ifstream type(base + "/type");
            int t = -1;
            if (type && (type >> t) && t >= 0) {
                name = pmu;
                return t;
            }
        }
        return -1;
    }

private:
    /// "0-15" or "0,2,4-7" contains `cpu`?
    static bool cpu_list_contains(const std::string& list, int cpu) {
        std::size_t pos = 0;
        while (pos < list.size()) {
            std::size_t comma = list.find(',', pos);
            if (comma == std::string::npos) comma = list.size();
            const std::string part = list.substr(pos, comma - pos);
            const std::size_t dash = part.find('-');
            if (dash == std::string::npos) {
                if (!part.empty() && std::atoi(part.c_str()) == cpu) return true;
            } else {
                const int lo = std::atoi(part.substr(0, dash).c_str());
                const int hi = std::atoi(part.substr(dash + 1).c_str());
                if (cpu >= lo && cpu <= hi) return true;
            }
            pos = comma + 1;
        }
        return false;
    }
#endif

    void open_counter() {
#if defined(__linux__)
        {
            std::ifstream in("/proc/sys/kernel/perf_event_paranoid");
            if (in) in >> paranoid_;
        }

        // Pick the PMU that owns the current CPU before opening, so a hybrid
        // machine counts on the core it is actually running on.
        const int cpu = ::sched_getcpu();
        const int hybrid = cpu >= 0 ? hybrid_pmu_type(cpu, pmu_) : -1;

        perf_event_attr attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        // HYBRID ENCODING: the PMU goes in the UPPER 32 BITS OF CONFIG, with
        // type left as PERF_TYPE_HARDWARE. Determined by testing all four
        // plausible encodings on both core types of an i7-12700K:
        //
        //   type=HARDWARE, config=event              P-core 195, E-core 0
        //   type=pmu,      config=event              P-core   0, E-core 0
        //   type=HARDWARE, config=(pmu<<32)|event    P-core 132, E-core 89   <--
        //   type=pmu,      config=0x3c (raw)         P-core 137, E-core 88
        //
        // The first row is the original bug: silently zero on E-cores. The
        // second was the obvious guess and is simply wrong. The fourth works
        // but hardcodes an Intel event number, so it would need a table per
        // vendor. The third is the kernel's documented extended-type mechanism
        // and is what perf itself emits.
        attr.config = event_;
        if (hybrid >= 0) {
            attr.config |= static_cast<std::uint64_t>(hybrid) << 32;
        }
        attr.disabled = 1;
        // exclude_kernel is what makes this work at paranoid=2, the common
        // default: a user-space-only measurement needs no extra privilege. It
        // also means the count excludes time in syscalls, which is what a
        // core-clock figure should measure anyway.
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;

        // pid=0 (this thread), cpu=-1 (whichever it runs on), no group, no
        // flags. Called through syscall() because glibc provides no wrapper.
        fd_ = static_cast<int>(::syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
        if (fd_ < 0) {
            const int e = errno;
            char buf[256];
            if (paranoid_ >= 3) {
                std::snprintf(buf, sizeof(buf),
                              "perf_event_open denied (%s); perf_event_paranoid is %d, "
                              "which blocks unprivileged counters entirely -- 2 or lower "
                              "is required",
                              std::strerror(e), paranoid_);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "perf_event_open failed (%s); perf_event_paranoid is %d",
                              std::strerror(e), paranoid_);
            }
            note_ = buf;
        }
#else
        note_ = "hardware counters need perf_event, which is Linux-only; "
                "Windows and macOS expose no unprivileged equivalent";
#endif
    }

    void close_counter() {
#if defined(__linux__)
        if (fd_ >= 0) ::close(fd_);
#endif
        fd_ = -1;
    }
};

/// Can this process read hardware counters?
inline counter_support counters_available() {
    counter_support s;
    cycle_counter c;
    s.available = c.ok();
    s.paranoid = c.paranoid();
    s.note = c.note();
    s.pmu = c.pmu();
    return s;
}

/// Sustained core clock in GHz, MEASURED. Returns 0 when counters are denied.
///
/// Counts unhalted cycles across a busy interval and divides by the wall time
/// it took. The load matters: an idle core clocks down, so the loop must keep
/// the core busy for the whole window or the figure describes the idle state.
/// A dependent integer chain does that without touching memory, so what is
/// measured is the core's clock rather than the memory system's response.
inline double measure_clock_ghz(double seconds = 0.2, std::string* why = nullptr) {
    cycle_counter c;
    if (!c.ok()) {
        if (why != nullptr) *why = c.note();
        return 0.0;
    }

    // The counter is bound to a PMU when it is opened. On a hybrid part an
    // unpinned thread can then migrate to a core the OTHER PMU owns, where this
    // counter counts nothing -- so the measurement would silently describe a
    // core it never ran on, or report zero. Recording the CPU on both sides
    // turns that into a detectable condition.
#if defined(__linux__)
    const int cpu_before = ::sched_getcpu();
#else
    const int cpu_before = -1;
#endif

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(seconds));

    c.start();
    const auto t0 = std::chrono::steady_clock::now();

    // Dependent chain: each step needs the previous, so the loop cannot be
    // vectorized or elided, and the core stays busy rather than stalling.
    volatile std::uint64_t sink = 0;
    std::uint64_t x = 1;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 4096; ++i) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        }
        sink = x;
    }

    const auto t1 = std::chrono::steady_clock::now();
    const std::uint64_t cycles = c.stop();
    (void)sink;

#if defined(__linux__)
    const int cpu_after = ::sched_getcpu();
    // Only a migration ACROSS PMU DOMAINS invalidates the count. Moving between
    // two identical cores leaves the counter bound to a PMU that still owns the
    // core, so the measurement stands -- rejecting those would refuse to measure
    // on any unpinned process, which is most of them.
    if (cpu_before >= 0 && cpu_after >= 0 && cpu_before != cpu_after) {
        std::string before_pmu, after_pmu;
        const int dom_before = cycle_counter::hybrid_pmu_type(cpu_before, before_pmu);
        const int dom_after = cycle_counter::hybrid_pmu_type(cpu_after, after_pmu);
        if (dom_before != dom_after) {
            if (why != nullptr) {
                *why = "the thread migrated across core types during measurement (cpu " +
                       std::to_string(cpu_before) + " on " + before_pmu + " to cpu " +
                       std::to_string(cpu_after) + " on " + after_pmu +
                       "); the counter stays bound to the PMU it was opened on, so the "
                       "result would describe a core it did not run on -- pin with taskset";
            }
            return 0.0;
        }
    }
#endif

    const double elapsed = std::chrono::duration<double>(t1 - t0).count();
    if (cycles == 0) {
        // Distinct from "denied": the counter opened and counted nothing, which
        // on a hybrid part means it was bound to the wrong PMU. Saying so beats
        // reporting the same message as a permissions failure.
        if (why != nullptr) {
            *why = "counter opened on PMU '" + c.pmu() +
                   "' but counted zero cycles -- likely bound to the wrong PMU for "
                   "this core";
        }
        return 0.0;
    }
    if (elapsed <= 0.0) return 0.0;
    return static_cast<double>(cycles) / elapsed / 1e9;
}

}  // namespace ppe::probe
