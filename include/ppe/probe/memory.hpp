// memory.hpp -- the two canonical memory probes, callable from anywhere.
//
// These began inside applications/memory_hierarchy and applications/
// layer_bandwidth. They are here because the topology report needs to run them
// per cluster, and a third copy of a probe whose correctness took four bug
// fixes to establish would be the worst possible place to put one.
//
// Every hard-won property is preserved, and each is a bug that was actually
// hit:
//
//   SINGLE CYCLE. The chase walks one cycle over the whole working set
//   (Sattolo), not an arbitrary permutation. A permutation decomposes into
//   disjoint cycles, and a chase starting in one visits only that cycle's slots
//   -- touching a fraction of the set while appearing to sweep all of it.
//
//   REAL LINE SIZE. Slot spacing is the machine's cache line. Spacing by 64 on a
//   128-byte-line machine makes consecutive slots share a line, so half the hops
//   hit and the reported latency is biased low -- 13% at half the true size,
//   measured.
//
//   LANE-WISE ACCUMULATION. Floating-point addition is not associative, so a
//   compiler may not fuse independent scalar accumulator chains into a vector
//   without -ffast-math. Four scalar chains stay scalar and retire two 8-byte
//   loads per cycle whatever the ISA offers, which reported L1d and L2 read as
//   identical. Accumulating by lane index preserves each lane's order, needs no
//   reassociation, and doubled the measured L1 bandwidth.
//
//   SAMPLES LONG ENOUGH TO BE MEASUREMENTS. One pass over an L1-resident buffer
//   takes a few hundred nanoseconds, at steady_clock's resolution limit. Each
//   timed sample repeats the kernel until it has moved enough traffic.
#pragma once

#include <ppe/harness.hpp>

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

namespace ppe::probe {

namespace detail {

template <typename T>
void keep(T value) {
    static volatile T sink;
    sink = value;
}

/// Traffic one timed sample should move.
constexpr std::size_t kTrafficPerSample = 32u * 1024u * 1024u;

inline std::size_t passes_for(std::size_t bytes) {
    if (bytes == 0) return 1;
    const std::size_t p = kTrafficPerSample / bytes;
    return p < 1 ? 1 : (p > 200000 ? 200000 : p);
}

/// Sattolo's algorithm: a single cycle over every slot, by construction.
inline std::vector<std::size_t> build_cycle(std::size_t slots, std::uint64_t seed) {
    std::vector<std::size_t> order(slots);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::mt19937_64 g(seed);
    for (std::size_t i = slots - 1; i > 0; --i) {
        const std::size_t j = static_cast<std::size_t>(g() % i);  // strictly less than i
        std::swap(order[i], order[j]);
    }
    return order;
}

}  // namespace detail

/// Nanoseconds per dependent access over a working set of `bytes`.
///
/// One outstanding access at a time -- the load's result IS the next address --
/// which is what makes this a latency rather than a throughput.
inline double chase_latency_ns(std::size_t bytes, std::size_t line_bytes,
                               std::uint64_t seed = 0xC0FFEE) {
    if (line_bytes < sizeof(std::size_t) || line_bytes % sizeof(std::size_t) != 0) {
        return 0.0;
    }
    const std::size_t stride = line_bytes / sizeof(std::size_t);
    const std::size_t slots = bytes / line_bytes;
    if (slots < 2) return 0.0;

    std::vector<std::size_t> buf(slots * stride, 0);
    const std::vector<std::size_t> order = detail::build_cycle(slots, seed);
    for (std::size_t i = 0; i < slots; ++i) {
        buf[order[i] * stride] = order[(i + 1) % slots] * stride;
    }

    const std::size_t accesses = std::max<std::size_t>(slots * 4, 1u << 18);
    std::size_t sink = 0;
    const double seconds = ppe::time_median(
        [&] {
            std::size_t idx = 0;
            for (std::size_t k = 0; k < accesses; ++k) idx = buf[idx];
            sink += idx;
        },
        3);

    detail::keep(sink);
    return seconds > 0.0 ? seconds / static_cast<double>(accesses) * 1e9 : 0.0;
}

/// Streaming-read bandwidth in GB/s over a working set of `bytes`.
inline double stream_read_gbs(std::size_t bytes) {
    const std::size_t n = bytes / sizeof(double);
    if (n < 64) return 0.0;

    std::vector<double> a(n, 1.0);
    const std::size_t passes = detail::passes_for(bytes);
    double sink = 0.0;

    const double seconds = ppe::time_median(
        [&] {
            for (std::size_t p = 0; p < passes; ++p) {
                alignas(64) double s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                for (std::size_t i = 0; i + 7 < n; i += 8) {
                    for (int j = 0; j < 8; ++j) s[j] += a[i + j];
                }
                for (int j = 0; j < 8; ++j) sink += s[j];
            }
        },
        3);

    detail::keep(sink);
    if (seconds <= 0.0) return 0.0;
    return static_cast<double>(bytes) * static_cast<double>(passes) / seconds / 1e9;
}

}  // namespace ppe::probe
