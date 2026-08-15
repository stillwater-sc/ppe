// memory_hierarchy -- measure what the memory hierarchy actually does.
//
// Detection asks the OS what the cache hierarchy is. This measures what it
// behaves like. The two together are the machine model; where they disagree,
// the measurement is the one describing the machine you are running on. (The
// comparison itself arrives in phase 2, once the detection backends exist --
// see docs/plans/first-application.md.)
//
// Three sweeps, all against working-set size:
//
//   latency    dependent pointer chase -- one outstanding miss at a time, so
//              the number reported is a real load-to-use latency
//   bandwidth  streaming read -- independent accesses, so the machine is free
//              to run as many misses in parallel as it can sustain
//   threads    the bandwidth sweep at one working set, against thread count,
//              which is where a shared level stops scaling
//
// Latency and bandwidth are deliberately the first things measured here, and
// compute peak is not. mtl5/ppe's peak.hpp documents a peak probe that reported
// 3.8 million GOP/s when the compiler folded its loop away, and 17.7 GOP/s once
// the operands were made opaque -- below what the kernels it was meant to bound
// actually achieved. Latency and bandwidth can be measured honestly with the
// techniques below; compute peak is its own project (phase 3).
//
// WHAT THIS MEASURES HONESTLY, AND WHAT IT CONFLATES
//
// The pointer chase walks a single random cycle over cache-line-spaced slots.
// Random defeats the hardware prefetcher, and the dependency between loads
// (each load's address IS the previous load's result) means exactly one access
// is outstanding, which is what makes the result a latency rather than a
// throughput. But at large working sets it also misses in the TLB on nearly
// every access, so the DRAM plateau it reports includes page-walk cost. That is
// a property of the machine and not an artefact -- but it is not the same
// number a hugepage-backed chase would give, and the two should not be compared.
//
// Frequency scaling is not controlled here. Pin the process and fix the clock
// before treating any of these numbers as the machine's.

#include <ppe/cli.hpp>
#include <ppe/harness.hpp>
#include <ppe/platform.hpp>
#include <ppe/provenance.hpp>
#include <ppe/version.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// Cache line size is assumed here rather than detected; phase 2 replaces this
// with ppe::detect_cpu().cache_line_bytes. 64 is right for every x86-64 part
// and for Apple silicon's 128-byte lines it merely means the chase touches
// every other line, which costs resolution rather than correctness.
constexpr std::size_t kAssumedLineBytes = 64;

constexpr std::size_t kMiB = 1024u * 1024u;

/// Keep a computed value alive so the compiler cannot delete the loop that
/// produced it. A volatile store is the portable way to say "this is observed";
/// it costs one store outside the timed region.
template <typename T>
void keep(T value) {
    static volatile T sink;
    sink = value;
}

void print_help() {
    std::printf(
        "memory_hierarchy -- measure latency and bandwidth vs working-set size (PPE %s)\n"
        "\n"
        "Usage: memory_hierarchy [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help         show this help and exit\n"
        "      --max-mib N    largest working set, in MiB (default 64)\n"
        "      --min-kib N    smallest working set, in KiB (default 4)\n"
        "      --threads N    max threads for the scaling sweep (default: hardware)\n"
        "      --csv PATH     write results as CSV, with provenance comments\n"
        "      --json         emit the provenance record as JSON and exit\n"
        "      --no-threads   skip the thread scaling sweep\n"
        "\n"
        "Pin the process (taskset) and fix the clock before believing any number\n"
        "this prints. On a hybrid CPU an unpinned run can migrate mid-sweep and\n"
        "report two different machines in one table.\n",
        ppe::version_string);
}

// ---------------------------------------------------------------------------
// Latency: dependent pointer chase over a single random cycle
// ---------------------------------------------------------------------------

/// Build a single cycle visiting every slot exactly once (Sattolo's algorithm).
///
/// A single cycle, not an arbitrary permutation: a permutation decomposes into
/// several disjoint cycles, and a chase starting in one of them would only ever
/// visit that cycle's slots -- touching a fraction of the working set while
/// appearing to sweep all of it, and reporting the latency of a smaller set.
std::vector<std::size_t> build_cycle(std::size_t slots, std::uint64_t seed) {
    std::vector<std::size_t> order(slots);
    std::iota(order.begin(), order.end(), std::size_t{0});

    std::mt19937_64 g(seed);
    for (std::size_t i = slots - 1; i > 0; --i) {
        // Sattolo: j strictly less than i, which is what makes the result one
        // cycle rather than a general permutation.
        const std::size_t j = static_cast<std::size_t>(g() % i);
        std::swap(order[i], order[j]);
    }
    return order;
}

/// Nanoseconds per dependent access over a working set of `bytes`.
double measure_latency_ns(std::size_t bytes, std::uint64_t seed) {
    const std::size_t stride = kAssumedLineBytes / sizeof(std::size_t);
    const std::size_t slots = bytes / kAssumedLineBytes;
    if (slots < 2) return 0.0;

    // Each slot holds the element index of the next slot in the cycle, so the
    // chase is `idx = buf[idx]` -- the load's result IS the next address.
    std::vector<std::size_t> buf(slots * stride, 0);
    const std::vector<std::size_t> order = build_cycle(slots, seed);
    for (std::size_t i = 0; i < slots; ++i) {
        buf[order[i] * stride] = order[(i + 1) % slots] * stride;
    }

    // Enough accesses to amortize timer overhead, and enough passes over a
    // small set that it is measured warm rather than while filling.
    const std::size_t accesses = std::max<std::size_t>(slots * 4, 1u << 18);

    std::size_t sink = 0;
    const double seconds = ppe::time_median(
        [&] {
            std::size_t idx = 0;
            for (std::size_t k = 0; k < accesses; ++k) {
                idx = buf[idx];
            }
            sink += idx;  // consume: the chase is dead code otherwise
        },
        3);

    keep(sink);
    return seconds / static_cast<double>(accesses) * 1e9;
}

// ---------------------------------------------------------------------------
// Bandwidth: streaming read, independent accesses
// ---------------------------------------------------------------------------

/// GB/s for a streaming read over a working set of `bytes`, on one thread.
double measure_bandwidth_gbs(std::size_t bytes) {
    const std::size_t n = bytes / sizeof(double);
    if (n < 64) return 0.0;

    std::vector<double> a(n, 1.0);
    double sink = 0.0;

    // Four independent accumulators: a single one serializes the sweep on the
    // floating-point add's latency and measures the adder rather than memory.
    const double seconds = ppe::time_median(
        [&] {
            double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
            for (std::size_t i = 0; i + 3 < n; i += 4) {
                s0 += a[i];
                s1 += a[i + 1];
                s2 += a[i + 2];
                s3 += a[i + 3];
            }
            sink += s0 + s1 + s2 + s3;
        },
        3);

    keep(sink);
    return seconds > 0.0 ? static_cast<double>(bytes) / seconds / 1e9 : 0.0;
}

/// Aggregate GB/s with `threads` threads, each streaming its OWN buffer of
/// `bytes_per_thread`.
///
/// Own buffer, not a shared one: sharing would measure how well the cache
/// replicates a read-only line, which is a different and much flatter curve
/// than the per-core bandwidth this sweep is after.
double measure_bandwidth_threaded_gbs(std::size_t bytes_per_thread, unsigned threads) {
    const std::size_t n = bytes_per_thread / sizeof(double);
    if (n < 64 || threads == 0) return 0.0;

    std::vector<std::vector<double>> buffers(threads);
    for (auto& b : buffers) b.assign(n, 1.0);

    // Spin barrier rather than std::barrier: libc++ gates <barrier> behind
    // availability macros on older Apple toolchains, and this needs to compile
    // everywhere the CI matrix builds.
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> pool;
    pool.reserve(threads);

    // Per-thread slots rather than a shared accumulator: no atomics on the hot
    // path, and no data race to reason about.
    std::vector<double> elapsed(threads, 0.0);
    std::vector<double> checksums(threads, 0.0);

    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            std::vector<double>& a = buffers[t];

            // Warm up before the barrier: first touch faults pages in, and a
            // thread still faulting while others are timed drags the aggregate.
            double warm = 0.0;
            for (std::size_t i = 0; i < n; ++i) warm += a[i];
            checksums[t] = warm;

            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }

            const auto t0 = std::chrono::steady_clock::now();
            double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
            for (std::size_t i = 0; i + 3 < n; i += 4) {
                s0 += a[i];
                s1 += a[i + 1];
                s2 += a[i + 2];
                s3 += a[i + 3];
            }
            const auto t1 = std::chrono::steady_clock::now();

            elapsed[t] = std::chrono::duration<double>(t1 - t0).count();
            checksums[t] += s0 + s1 + s2 + s3;
        });
    }

    while (ready.load() < threads) { /* spin until every thread is warm */ }

    // The aggregate is measured on ONE clock spanning all threads, from release
    // to the last join -- NOT from the per-thread intervals.
    //
    // Per-thread intervals are wrong here in a way that inflates the answer
    // rather than adding noise. Each thread times only its own execution, so
    // when the threads run SEQUENTIALLY -- which is exactly what happens when
    // they are pinned to fewer cores than there are threads -- every thread
    // records a short interval that overlaps none of the others. Multiplying
    // that by the thread count then reports N serialized runs as N concurrent
    // ones. Measured on this box, 8 threads under `taskset -c 4` reported
    // 7.18x scaling on a single core.
    //
    // The wall-clock window cannot lie that way: it is the time the machine
    // actually took to move threads * bytes_per_thread.
    const auto wall0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : pool) th.join();
    const auto wall1 = std::chrono::steady_clock::now();

    keep(std::accumulate(checksums.begin(), checksums.end(), 0.0));

    const double wall = std::chrono::duration<double>(wall1 - wall0).count();
    if (wall <= 0.0) return 0.0;

    const double moved = static_cast<double>(bytes_per_thread) * threads;
    return moved / wall / 1e9;
}

// ---------------------------------------------------------------------------
// Sweep construction and reporting
// ---------------------------------------------------------------------------

struct row {
    std::string probe;
    std::size_t working_set_bytes;
    unsigned    threads;
    double      value;
    std::string unit;
};

/// Two points per octave: powers of two alone put at most one sample between
/// adjacent cache levels, which is not enough to see where a knee begins.
std::vector<std::size_t> sweep_sizes(std::size_t min_bytes, std::size_t max_bytes) {
    std::vector<std::size_t> sizes;
    for (std::size_t s = min_bytes; s <= max_bytes; s *= 2) {
        sizes.push_back(s);
        const std::size_t mid = s + s / 2;
        if (mid <= max_bytes) sizes.push_back(mid);
    }
    return sizes;
}

std::string human_size(std::size_t bytes) {
    char buf[32];
    if (bytes >= kMiB) {
        std::snprintf(buf, sizeof(buf), "%.3g MiB", static_cast<double>(bytes) / kMiB);
    } else {
        std::snprintf(buf, sizeof(buf), "%.3g KiB", static_cast<double>(bytes) / 1024.0);
    }
    return std::string(buf);
}

int parse_int(int argc, char** argv, std::string_view flag, int fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (flag == argv[i]) {
            const int v = std::atoi(argv[i + 1]);
            if (v > 0) return v;
        }
    }
    return fallback;
}

const char* parse_str(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (flag == argv[i]) return argv[i + 1];
    }
    return nullptr;
}

/// A step up in latency between two adjacent working sets: `below` still fits
/// the level, `above` does not.
struct knee {
    std::size_t below = 0;
    std::size_t above = 0;
    double      ns_below = 0.0;
    double      ns_above = 0.0;
};

/// Find where latency steps up by more than 1.5x.
///
/// This is an INFERENCE, not a cache-size detector: a step can also come from
/// TLB reach or from a prefetcher giving up, and a level whose size falls
/// between two samples is bracketed rather than pinpointed.
std::vector<knee> find_knees(const std::vector<row>& rows) {
    std::vector<knee> knees;
    const row* prev = nullptr;
    for (const row& r : rows) {
        if (r.probe != "latency") continue;
        if (prev != nullptr && prev->value > 0.0 && r.value > prev->value * 1.5) {
            knees.push_back({prev->working_set_bytes, r.working_set_bytes,
                             prev->value, r.value});
        }
        prev = &r;
    }
    return knees;
}

void report_knees(const std::vector<knee>& knees) {
    std::printf("\nInferred level boundaries (latency steps > 1.5x):\n");
    if (knees.empty()) {
        std::printf("  none -- the sweep may not span a level boundary\n");
        return;
    }
    for (const knee& k : knees) {
        std::printf("  %-12s -> %-12s  %.2f ns -> %.2f ns  (%.1fx)\n",
                    human_size(k.below).c_str(), human_size(k.above).c_str(),
                    k.ns_below, k.ns_above, k.ns_above / k.ns_below);
    }
}

/// Compare what the OS CLAIMS against what the sweep MEASURED.
///
/// This is the point of the whole exercise. Detection asks the OS; the sweep
/// asks the hardware. Where they agree, the machine model is trustworthy; where
/// they disagree, the measurement describes the machine you are running on and
/// the claim describes something else -- a VM's passthrough, a hybrid part whose
/// cores differ, or a cache the process cannot actually use all of.
///
/// A claimed size is consistent with a knee when it falls in [below, above): the
/// working set at `below` still fit the level, and the one at `above` did not.
void report_comparison(const ppe::device_attributes& cpu, const std::vector<knee>& knees,
                       bool pinned_hint) {
    std::printf("\nClaimed (%s) vs measured:\n",
                cpu.source.empty() ? "no backend" : cpu.source.c_str());
    std::printf("%-6s %14s   %s\n", "level", "claimed", "measured");

    struct entry { const char* label; std::size_t bytes; std::size_t sharers; };
    const entry levels[] = {
        {"L1d", cpu.l1d_bytes, cpu.l1d_sharing_cores},
        {"L2",  cpu.l2_bytes,  cpu.l2_sharing_cores},
        {"L3",  cpu.l3_bytes,  cpu.l3_sharing_cores},
    };

    bool any_disagreement = false;
    for (const entry& e : levels) {
        if (e.bytes == 0) {
            std::printf("%-6s %14s   %s\n", e.label, "not detected",
                        "-- nothing to compare against");
            continue;
        }

        const knee* match = nullptr;
        for (const knee& k : knees) {
            if (e.bytes >= k.below && e.bytes < k.above) { match = &k; break; }
        }

        char claimed[32];
        std::snprintf(claimed, sizeof(claimed), "%s", human_size(e.bytes).c_str());

        if (match != nullptr) {
            std::printf("%-6s %14s   consistent: step at %s -> %s\n", e.label, claimed,
                        human_size(match->below).c_str(),
                        human_size(match->above).c_str());
        } else {
            std::printf("%-6s %14s   NO step bracketing this size\n", e.label, claimed);
            any_disagreement = true;
        }
    }

    if (any_disagreement) {
        std::printf(
            "\n  A claimed size with no step around it means the sweep did not see that\n"
            "  level where the OS says it is. Usual causes, in order of likelihood:\n"
            "    - the process is not pinned, so the sweep migrated between core types\n"
            "    - the sweep does not span the level (widen --min-kib / --max-mib)\n"
            "    - the level is shared and another tenant is using it\n");
    }
    if (pinned_hint) {
        std::printf(
            "\n  NOTE: detection is affinity-aware -- it describes the cores this process\n"
            "  may run on. On a hybrid machine an unpinned run detects one core type and\n"
            "  may measure another. Pin with taskset for the two halves to agree.\n");
    }
}

bool write_csv(const char* path, const ppe::provenance& prov, const std::vector<row>& rows) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        std::fprintf(stderr, "error: cannot open %s for writing\n", path);
        return false;
    }
    // Provenance travels IN the result file, not in a sidecar: a sidecar can be
    // separated from the data it describes, and then the data is unattributable.
    std::fputs(ppe::to_csv_comment(prov).c_str(), f);
    std::fputs("probe,working_set_bytes,threads,value,unit\n", f);
    for (const row& r : rows) {
        std::fprintf(f, "%s,%zu,%u,%.6f,%s\n", r.probe.c_str(), r.working_set_bytes,
                     r.threads, r.value, r.unit.c_str());
    }
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        print_help();
        return 0;
    }

    const ppe::provenance prov = ppe::collect_provenance();
    if (ppe::has_flag(argc, argv, "--json")) {
        std::fputs(ppe::to_json(prov).c_str(), stdout);
        return 0;
    }

    const std::size_t min_bytes =
        static_cast<std::size_t>(parse_int(argc, argv, "--min-kib", 4)) * 1024u;
    const std::size_t max_bytes =
        static_cast<std::size_t>(parse_int(argc, argv, "--max-mib", 64)) * kMiB;

    if (min_bytes > max_bytes) {
        std::fprintf(stderr, "error: --min-kib exceeds --max-mib\n");
        return 2;
    }

    std::fputs(ppe::to_text(prov).c_str(), stdout);
    std::printf("sweep   : %s to %s, assumed %zu-byte lines\n\n",
                human_size(min_bytes).c_str(), human_size(max_bytes).c_str(),
                kAssumedLineBytes);

    std::vector<row> rows;
    const std::vector<std::size_t> sizes = sweep_sizes(min_bytes, max_bytes);

    std::printf("%-14s %14s %14s\n", "working set", "latency (ns)", "bandwidth GB/s");
    for (const std::size_t s : sizes) {
        const double lat = measure_latency_ns(s, 0xC0FFEE);
        const double bw = measure_bandwidth_gbs(s);

        rows.push_back({"latency", s, 1, lat, "ns"});
        rows.push_back({"bandwidth", s, 1, bw, "GB/s"});

        std::printf("%-14s %14.2f %14.2f\n", human_size(s).c_str(), lat, bw);
        std::fflush(stdout);
    }

    const std::vector<knee> knees = find_knees(rows);
    report_knees(knees);

    // Affinity-aware detection plus more than one visible core means an
    // unpinned run can detect one core type and measure another.
    const bool hybrid_risk =
        (prov.cpu.source == "sysfs" || prov.cpu.source == "win32") &&
        prov.cpu.physical_cores > 1;
    report_comparison(prov.cpu, knees, hybrid_risk);

    if (!ppe::has_flag(argc, argv, "--no-threads")) {
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned max_threads =
            static_cast<unsigned>(parse_int(argc, argv, "--threads", static_cast<int>(hw)));

        // One working set per thread, sized past the last level so the sweep
        // measures the shared path to memory rather than private cache.
        const std::size_t per_thread = max_bytes;

        std::printf("\nThread scaling at %s per thread (own buffer each):\n",
                    human_size(per_thread).c_str());
        std::printf("%-10s %16s %12s\n", "threads", "aggregate GB/s", "vs 1 thread");

        double single = 0.0;
        for (unsigned t = 1; t <= max_threads; t *= 2) {
            const double gbs = measure_bandwidth_threaded_gbs(per_thread, t);
            if (t == 1) single = gbs;
            rows.push_back({"bandwidth_threaded", per_thread, t, gbs, "GB/s"});
            std::printf("%-10u %16.2f %11.2fx\n", t, gbs,
                        single > 0.0 ? gbs / single : 0.0);
            std::fflush(stdout);
        }
    }

    if (const char* csv = parse_str(argc, argv, "--csv"); csv != nullptr) {
        if (!write_csv(csv, prov, rows)) return 1;
        std::printf("\nwrote %zu rows to %s\n", rows.size(), csv);
    }

    return 0;
}
