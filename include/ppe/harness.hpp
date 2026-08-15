// harness.hpp -- workload-general measurement discipline: timing, repetition,
// and verification.
//
// Ported from mtl5/ppe/include/ppe/harness.hpp (Stillwater, MIT), keeping the
// parts that are not about GEMM. The GEMM-shaped parts of that header --
// gemm_ops() and the kernel/type/n `measurement` record -- deliberately stayed
// behind: they belong to the study that owns them.
//
// This duplication is deliberate and bounded. mtl5/ppe is standalone by design
// (nothing there includes <mtl/...>) because a teaching progression that
// acquires an external dependency stops being readable top to bottom. Fifty
// lines of stable utility code copied with attribution is a better trade than a
// shared library between the two repositories. What must NOT be duplicated is
// the machine model -- that is the part that goes stale as microarchitectures
// land, and it lives here. See docs/plans/first-application.md.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <type_traits>
#include <vector>

namespace ppe {

/// Median of the per-call wall clock over `reps` calls, after one warm-up.
///
/// Median rather than mean: on any machine that is not perfectly quiet the
/// distribution has a long right tail from scheduling and interrupts, and a
/// mean reports the interference rather than the work. The warm-up call is not
/// optional -- the first touch of a buffer faults its pages in, and timing that
/// measures the allocator.
template <typename F>
double time_median(F&& f, std::size_t reps) {
    f();  // warm up: caches, page faults, branch predictors
    std::vector<double> t;
    t.reserve(reps);
    for (std::size_t r = 0; r < reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto t1 = std::chrono::steady_clock::now();
        t.push_back(std::chrono::duration<double>(t1 - t0).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

/// Repetitions that keep each measurement around a tenth of a second without
/// spending minutes on the slow end of a sweep.
inline std::size_t reps_for(double seconds_estimate) {
    if (seconds_estimate <= 0.0) return 5;
    const std::size_t r = static_cast<std::size_t>(0.1 / seconds_estimate);
    return std::min<std::size_t>(std::max<std::size_t>(r, 1), 15);
}

/// Fill with small values so integer accumulation cannot overflow at the sizes
/// tested, and so floating-point results stay exactly representable -- which
/// lets verification use exact equality for integers and a tight relative
/// tolerance for floats.
///
/// Seeded explicitly rather than from a random device: a measurement you cannot
/// re-run on the same data is not a measurement you can bisect.
template <typename T>
void fill(std::vector<T>& v, std::uint64_t seed) {
    std::mt19937_64 g(seed);
    for (auto& x : v) {
        const int r = int(g() % 9) - 4;  // [-4, 4]
        x = static_cast<T>(r);
    }
}

/// Exact for integers; relative tolerance for floating point.
///
/// Verification is not optional in a performance harness. An optimization that
/// computes the wrong answer is usually faster, and a sweep that only records
/// times will happily report it as progress.
template <typename T>
bool matches(const std::vector<T>& got, const std::vector<T>& ref) {
    if (got.size() != ref.size()) return false;
    if constexpr (std::is_integral_v<T>) {
        for (std::size_t i = 0; i < got.size(); ++i)
            if (got[i] != ref[i]) return false;
    } else {
        double worst = 0.0, scale = 0.0;
        for (std::size_t i = 0; i < got.size(); ++i) {
            const double g = double(got[i]), r = double(ref[i]);
            worst = std::max(worst, std::fabs(g - r));
            scale = std::max(scale, std::fabs(r));
        }
        return worst <= 1e-6 * (scale > 0.0 ? scale : 1.0);
    }
    return true;
}

}  // namespace ppe
