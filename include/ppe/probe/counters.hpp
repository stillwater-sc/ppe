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
// THE API EXISTS ON EVERY PLATFORM. Only the syscall is behind a guard: burying
// the type itself would make anything using it -- including its test -- compile
// on Linux alone, which is exactly the bug tools/lint/platform_includes.py was
// extended to catch.
#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#if defined(__linux__)
#  include <asm/unistd.h>
#  include <linux/perf_event.h>
#  include <sys/ioctl.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

namespace ppe::probe {

/// Why counters are or are not usable here.
struct counter_support {
    bool available = false;
    int  paranoid = -99;      ///< observed perf_event_paranoid, -99 if unread
    std::string note;         ///< actionable reason when unavailable
};

/// Counts unhalted core cycles on the calling thread.
///
/// Constructed open, closed on destruction. `ok()` is false when the kernel
/// refused, and `note()` says what to do about it.
class cycle_counter {
public:
    cycle_counter() { open_counter(); }
    ~cycle_counter() { close_counter(); }

    cycle_counter(const cycle_counter&) = delete;
    cycle_counter& operator=(const cycle_counter&) = delete;

    bool ok() const { return fd_ >= 0; }
    const std::string& note() const { return note_; }
    int paranoid() const { return paranoid_; }

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
    int paranoid_ = -99;
    std::string note_;

    void open_counter() {
#if defined(__linux__)
        {
            std::ifstream in("/proc/sys/kernel/perf_event_paranoid");
            if (in) in >> paranoid_;
        }

        perf_event_attr attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = PERF_COUNT_HW_CPU_CYCLES;
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
    return s;
}

/// Sustained core clock in GHz, MEASURED. Returns 0 when counters are denied.
///
/// Counts unhalted cycles across a busy interval and divides by the wall time
/// it took. The load matters: an idle core clocks down, so the loop must keep
/// the core busy for the whole window or the figure describes the idle state.
/// A dependent integer chain does that without touching memory, so what is
/// measured is the core's clock rather than the memory system's response.
inline double measure_clock_ghz(double seconds = 0.2) {
    cycle_counter c;
    if (!c.ok()) return 0.0;

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

    const double elapsed = std::chrono::duration<double>(t1 - t0).count();
    if (elapsed <= 0.0 || cycles == 0) return 0.0;
    return static_cast<double>(cycles) / elapsed / 1e9;
}

}  // namespace ppe::probe
