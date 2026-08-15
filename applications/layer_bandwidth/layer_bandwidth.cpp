// layer_bandwidth -- achieved bandwidth per layer, as named numbers (#5).
//
// applications/memory_hierarchy sweeps bandwidth against working-set size and
// the cache knees are visible in the curve -- but it never NAMES a per-level
// bandwidth. "So what is L2 bandwidth on this machine?" is left to the reader's
// eye. This places a working set deliberately inside each detected level and
// reports read, write and copy for it.
//
// That is only possible because detection returns real capacities (phase 2).
// Before that there was nothing to place a working set inside.
//
// THREE THINGS THAT MAKE A BANDWIDTH NUMBER WRONG WHILE LOOKING RIGHT
//
// WRITE-ALLOCATE. A store to a line not resident pulls that line in first, so
// the memory system moves about twice what a naive bytes-written/seconds
// reports. The convention here is STREAM's -- count the bytes the PROGRAM asked
// to move -- and the DRAM rows say so explicitly, because that is where the
// read-for-ownership traffic is not already absorbed by cache residency.
//
// THE BUILD ISA IS A CEILING. L1d read bandwidth is set by load units times
// vector width. A binary compiled for SSE2 issues 16-byte loads and cannot
// reach an AVX2 machine's L1 bandwidth no matter how the loop is written. Both
// ISAs are reported, and a mismatch is called out.
//
// PRIVATE VERSUS SHARED. L1d and usually L2 are private, so aggregate bandwidth
// scales with cores; L3 and DRAM are shared, so it saturates. The two numbers
// answer different questions and both are reported. For a shared level the
// per-thread working set is divided by the thread count, or the threads would
// collectively overflow the level they are supposed to be measuring.
//
// Prefetchers are an ally here, unlike in the latency probe: sequential
// streaming is the access pattern that measures bandwidth.

#include <ppe/cli.hpp>
#include <ppe/detect/clock.hpp>
#include <ppe/detect/cpu.hpp>
#include <ppe/detect/fileio.hpp>
#include <ppe/detect/isa.hpp>
#include <ppe/harness.hpp>
#include <ppe/platform.hpp>
#include <ppe/provenance.hpp>
#include <ppe/trace.hpp>
#include <ppe/version.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kMiB = 1024u * 1024u;

/// Fraction of a level's capacity to occupy.
///
/// Not 100%: a working set equal to capacity does not sit in that level.
/// Associativity conflicts and the tail of the previous level both intrude, and
/// the measured figure slides toward the next level down. Half is comfortably
/// resident while still large enough that loop overhead is amortized.
constexpr double kOccupancy = 0.5;

/// Above this relative spread, a number has not been resolved by this run.
constexpr double kUnresolvedSpread = 0.20;

/// Traffic each TIMED CALL should move, so the sample is long enough to be a
/// measurement rather than a clock reading.
///
/// A single pass over a 24 KiB L1-resident buffer takes a few hundred
/// nanoseconds -- at or below steady_clock's useful resolution, and dominated by
/// scheduling noise. The first version of this tool timed one pass per sample
/// and reported L1d read at 75.9 GB/s against L2 read at 78.2, which is not a
/// hierarchy: it was measuring timer overhead at both levels. Repeating the
/// kernel inside the timed region until it has moved this much fixes it.
constexpr std::size_t kTrafficPerSample = 64u * 1024u * 1024u;

/// Passes needed for one timed call to move kTrafficPerSample over `bytes`.
std::size_t passes_for(std::size_t bytes) {
    if (bytes == 0) return 1;
    const std::size_t p = kTrafficPerSample / bytes;
    return p < 1 ? 1 : (p > 200000 ? 200000 : p);
}

template <typename T>
void keep(T value) {
    static volatile T sink;
    sink = value;
}

void print_help() {
    std::printf(
        "layer_bandwidth -- achieved bandwidth per hierarchy layer (PPE %s)\n"
        "\n"
        "Usage: layer_bandwidth [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help        show this help and exit\n"
        "      --threads N   threads for the aggregate columns (default: physical cores)\n"
        "      --dram-mib N  working set for the DRAM rows (default: 4x L3, min 64)\n"
        "      --storage-mib N  test file size for the storage row (default 256)\n"
        "      --no-storage  skip the storage layer (writes no file)\n"
        "      --csv PATH    write results as CSV, with provenance comments\n"
        "      --trace PATH  write a Chrome Trace Event JSON of the run\n"
        "      --json        emit the provenance record as JSON and exit\n"
        "\n"
        "Pin the process (taskset) before believing any number. On a hybrid CPU an\n"
        "unpinned run measures whichever core type it happened to land on, while\n"
        "detection describes the cores it may run on -- see memory_hierarchy.\n",
        ppe::version_string);
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

std::string human(std::size_t bytes) {
    char buf[32];
    if (bytes >= kMiB) {
        std::snprintf(buf, sizeof(buf), "%.3g MiB", static_cast<double>(bytes) / kMiB);
    } else {
        std::snprintf(buf, sizeof(buf), "%.3g KiB", static_cast<double>(bytes) / 1024.0);
    }
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Kernels. Each returns the timing for ONE pass over `n` doubles.
// ---------------------------------------------------------------------------

/// Read: LANE-WISE accumulation into an array, not scalar accumulators.
///
/// One accumulator serializes the loop on the adder's latency and measures the
/// FPU. But four scalar accumulators are not enough either, and the reason is
/// subtle: floating-point addition is not associative, so a compiler may not
/// combine independent scalar chains into a vector without -ffast-math. Four
/// scalar chains therefore stay scalar, and the loop retires two 8-byte loads
/// per cycle regardless of the ISA -- which is why an earlier version of this
/// tool reported L1d read and L2 read as both 16 B/cycle. That is not a
/// hierarchy; it is a load-issue limit measured twice.
///
/// Accumulating into s[j] where j is the lane index preserves each lane's
/// summation order, so vectorizing it needs no reassociation and the compiler
/// is free to do it. Measured on this machine: 16 B/cycle scalar against
/// 32 B/cycle lane-wise, on the identical buffer.
ppe::timing read_pass(std::vector<double>& a, std::size_t reps, std::size_t passes) {
    const std::size_t n = a.size();
    double sink = 0.0;
    const ppe::timing t = ppe::time_with_spread(
        [&] {
            for (std::size_t p = 0; p < passes; ++p) {
                alignas(64) double s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                for (std::size_t i = 0; i + 7 < n; i += 8) {
                    for (int j = 0; j < 8; ++j) s[j] += a[i + j];
                }
                for (int j = 0; j < 8; ++j) sink += s[j];
            }
        },
        reps);
    keep(sink);
    return t;
}

/// Write: store a constant across the buffer.
ppe::timing write_pass(std::vector<double>& a, std::size_t reps, std::size_t passes) {
    const std::size_t n = a.size();
    const ppe::timing t = ppe::time_with_spread(
        [&] {
            for (std::size_t p = 0; p < passes; ++p) {
                for (std::size_t i = 0; i < n; ++i) a[i] = static_cast<double>(p);
            }
        },
        reps);
    keep(a.empty() ? 0.0 : a[n / 2]);
    return t;
}

/// Copy: a[i] = b[i]. Traffic is 2x n doubles under the STREAM convention.
ppe::timing copy_pass(std::vector<double>& dst, std::vector<double>& src,
                      std::size_t reps, std::size_t passes) {
    const std::size_t n = std::min(dst.size(), src.size());
    const ppe::timing t = ppe::time_with_spread(
        [&] {
            for (std::size_t p = 0; p < passes; ++p) {
                for (std::size_t i = 0; i < n; ++i) dst[i] = src[i];
            }
        },
        reps);
    keep(dst.empty() ? 0.0 : dst[n / 2]);
    return t;
}

// ---------------------------------------------------------------------------
// Aggregate: T threads, each on its own buffer, one wall clock spanning all.
// ---------------------------------------------------------------------------

enum class kernel { read, write, copy };

/// Aggregate GB/s across `threads`, each streaming its own buffer.
///
/// Timed on ONE clock from the barrier release to the last join. Per-thread
/// intervals would report N serialized passes as N concurrent ones whenever the
/// threads do not actually overlap -- the bug documented at length in
/// applications/memory_hierarchy.
double aggregate_gbs(std::size_t bytes_per_thread, unsigned threads, kernel k,
                     unsigned passes) {
    const std::size_t n = bytes_per_thread / sizeof(double);
    if (n < 64 || threads == 0) return 0.0;

    std::vector<std::vector<double>> bufs(threads);
    std::vector<std::vector<double>> srcs(k == kernel::copy ? threads : 0);
    for (unsigned t = 0; t < threads; ++t) {
        bufs[t].assign(n, 1.0);
        if (k == kernel::copy) srcs[t].assign(n, 2.0);
    }

    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<double> checksums(threads, 0.0);
    std::vector<std::thread> pool;
    pool.reserve(threads);

    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            std::vector<double>& a = bufs[t];

            // Warm up before the barrier: first touch faults the pages in, and a
            // thread still faulting while the others are timed drags the
            // aggregate down.
            double warm = 0.0;
            for (std::size_t i = 0; i < n; ++i) warm += a[i];
            checksums[t] = warm;

            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }

            double acc = 0.0;
            for (unsigned p = 0; p < passes; ++p) {
                if (k == kernel::read) {
                    // Lane-wise, for the reason documented on read_pass.
                    alignas(64) double s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                    for (std::size_t i = 0; i + 7 < n; i += 8) {
                        for (int j = 0; j < 8; ++j) s[j] += a[i + j];
                    }
                    for (int j = 0; j < 8; ++j) acc += s[j];
                } else if (k == kernel::write) {
                    for (std::size_t i = 0; i < n; ++i) a[i] = 1.0;
                    acc += a[n / 2];
                } else {
                    std::vector<double>& s = srcs[t];
                    for (std::size_t i = 0; i < n; ++i) a[i] = s[i];
                    acc += a[n / 2];
                }
            }
            checksums[t] += acc;
        });
    }

    while (ready.load() < threads) { /* spin until every thread is warm */ }
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : pool) th.join();
    const auto t1 = std::chrono::steady_clock::now();

    keep(std::accumulate(checksums.begin(), checksums.end(), 0.0));

    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    if (seconds <= 0.0) return 0.0;

    const double per_pass = static_cast<double>(bytes_per_thread) *
                            (k == kernel::copy ? 2.0 : 1.0);
    return per_pass * threads * passes / seconds / 1e9;
}

// ---------------------------------------------------------------------------

struct layer {
    const char* name;
    std::size_t capacity;       ///< 0 for DRAM and storage
    std::size_t sharing_cores;  ///< 0/1 = private
    std::size_t working_set;
    bool        shared;
    bool        is_dram;
};

struct result {
    std::string layer;
    std::string kernel;
    std::size_t working_set = 0;
    unsigned    threads = 0;
    double      gbs = 0.0;
    double      bytes_per_cycle = 0.0;
    double      spread = 0.0;
};

/// GB/s for one timing over `bytes` moved.
double to_gbs(const ppe::timing& t, double bytes) {
    return t.median > 0.0 ? bytes / t.median / 1e9 : 0.0;
}

void print_row(const char* layer_name, const char* kern, std::size_t ws, double gbs,
               double bpc, double spread, double aggregate) {
    std::printf("%-6s %-6s %10s %10.2f", layer_name, kern, human(ws).c_str(), gbs);
    if (bpc > 0.0) {
        std::printf(" %10.2f", bpc);
    } else {
        std::printf(" %10s", "-");
    }
    std::printf(" %7.1f%%", spread * 100.0);
    if (aggregate > 0.0) {
        std::printf(" %12.2f", aggregate);
    } else {
        std::printf(" %12s", "-");
    }
    if (spread > kUnresolvedSpread) std::printf("   UNRESOLVED");
    std::printf("\n");
    std::fflush(stdout);
}

bool write_csv(const char* path, const ppe::provenance& prov,
               const std::vector<result>& rows, const std::string& io_mode) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        std::fprintf(stderr, "error: cannot open %s for writing\n", path);
        return false;
    }
    std::fputs(ppe::to_csv_comment(prov).c_str(), f);
    std::fprintf(f, "# storage_io_mode=%s\n", io_mode.c_str());
    std::fprintf(f, "# occupancy=%.2f\n", kOccupancy);
    std::fprintf(f, "# write_convention=STREAM (bytes the program moved; a\n");
    std::fprintf(f, "#   write-allocate machine without non-temporal stores moves ~2x\n");
    std::fputs("layer,kernel,working_set_bytes,threads,gbs,bytes_per_cycle,spread\n", f);
    for (const result& r : rows) {
        std::fprintf(f, "%s,%s,%zu,%u,%.6f,%.6f,%.6f\n", r.layer.c_str(),
                     r.kernel.c_str(), r.working_set, r.threads, r.gbs,
                     r.bytes_per_cycle, r.spread);
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

    const char* trace_path = parse_str(argc, argv, "--trace");
    if (trace_path != nullptr) {
        ppe::trace::global().enable();
        ppe::trace::global().name_thread("main");
    }

    const ppe::isa_capabilities isa = ppe::detect_isa();
    const ppe::clock_claim clk = ppe::detect_clock();

    std::fputs(ppe::to_text(prov).c_str(), stdout);
    std::printf("machine ISA : %s (%u-bit vectors)\n", isa.name.c_str(), isa.vector_bits);
    std::printf("build ISA   : %s\n", ppe::build_isa());
    if (isa.vector_bits > 0 && std::string_view(ppe::build_isa()) != isa.name) {
        std::printf(
            "              ^ THIS BINARY CANNOT REACH THE MACHINE'S L1 BANDWIDTH.\n"
            "                Cache bandwidth is load units x vector width; a narrower\n"
            "                build issues narrower loads. Rebuild with the release\n"
            "                preset (PPE_NATIVE_ARCH=ON) before comparing L1/L2 rows\n"
            "                against another machine.\n");
    }
    if (clk.ghz > 0.0) {
        std::printf("clock       : %.3f GHz (%s, a claim) -- bytes/cycle derived from it\n",
                    clk.ghz, clk.source.c_str());
    } else {
        std::printf("clock       : not detected -- bytes/cycle omitted\n");
    }

    const unsigned cores =
        prov.cpu.physical_cores > 0 ? prov.cpu.physical_cores
                                    : std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads =
        static_cast<unsigned>(parse_int(argc, argv, "--threads", static_cast<int>(cores)));

    // -- Layer table -------------------------------------------------------
    std::vector<layer> layers;
    const struct { const char* name; std::size_t cap; std::size_t sharing; } cache_levels[] = {
        {"L1d", prov.cpu.l1d_bytes, prov.cpu.l1d_sharing_cores},
        {"L2",  prov.cpu.l2_bytes,  prov.cpu.l2_sharing_cores},
        {"L3",  prov.cpu.l3_bytes,  prov.cpu.l3_sharing_cores},
    };
    for (const auto& c : cache_levels) {
        if (c.cap == 0) continue;
        layer l;
        l.name = c.name;
        l.capacity = c.cap;
        l.sharing_cores = c.sharing;
        l.shared = c.sharing > 1;
        l.is_dram = false;
        l.working_set = static_cast<std::size_t>(static_cast<double>(c.cap) * kOccupancy);
        layers.push_back(l);
    }

    // DRAM: well past the last level, so nothing is cache resident.
    const std::size_t last_level = prov.cpu.l3_bytes > 0 ? prov.cpu.l3_bytes
                                                         : prov.cpu.l2_bytes;
    std::size_t dram_ws = std::max<std::size_t>(64 * kMiB, last_level * 4);
    dram_ws = static_cast<std::size_t>(parse_int(argc, argv, "--dram-mib",
                                                 static_cast<int>(dram_ws / kMiB))) * kMiB;
    {
        layer l;
        l.name = "DRAM";
        l.capacity = 0;
        l.sharing_cores = 0;
        l.shared = true;   // every core contends for the same controllers
        l.is_dram = true;
        l.working_set = dram_ws;
        layers.push_back(l);
    }

    if (layers.size() == 1) {
        std::printf(
            "\nWARNING: no cache capacities detected, so only the DRAM row below is\n"
            "         placed deliberately. The cache rows are absent rather than\n"
            "         guessed.\n");
    }

    std::printf("\nWorking sets are %.0f%% of each level's capacity. For a SHARED level the\n"
                "aggregate columns divide that between threads, or the threads would\n"
                "collectively overflow the level they are measuring.\n\n",
                kOccupancy * 100.0);

    std::printf("%-6s %-6s %10s %10s %10s %8s %12s\n", "layer", "kernel", "set",
                "GB/s", "B/cycle", "spread", "aggregate");

    std::vector<result> rows;

    for (const layer& l : layers) {
        ppe::trace::scope span("layer", "bandwidth");

        const std::size_t n = l.working_set / sizeof(double);
        if (n < 64) continue;

        const std::size_t reps = l.working_set > 8 * kMiB ? 3 : 5;
        // Each timed sample repeats the kernel until it has moved
        // kTrafficPerSample, so a small L1-resident buffer is not being timed
        // one sub-microsecond pass at a time.
        const std::size_t inner = passes_for(l.working_set);

        std::vector<double> a(n, 1.0);
        std::vector<double> b(n, 2.0);

        // For a shared level the aggregate run must divide the level between
        // threads. For a private level each thread gets the full working set,
        // because each has its own copy of that cache.
        const std::size_t agg_ws =
            l.shared && !l.is_dram
                ? std::max<std::size_t>(l.working_set / (threads ? threads : 1), 4096)
                : l.working_set;

        // Same reasoning for the aggregate run, sized against its own
        // per-thread working set.
        const unsigned passes = static_cast<unsigned>(passes_for(agg_ws));

        struct { const char* label; kernel k; double bytes; } kernels[] = {
            {"read", kernel::read, static_cast<double>(l.working_set)},
            {"write", kernel::write, static_cast<double>(l.working_set)},
            {"copy", kernel::copy, 2.0 * static_cast<double>(l.working_set)},
        };

        for (const auto& kn : kernels) {
            ppe::timing t;
            if (kn.k == kernel::read) t = read_pass(a, reps, inner);
            else if (kn.k == kernel::write) t = write_pass(a, reps, inner);
            else t = copy_pass(a, b, reps, inner);

            const double gbs = to_gbs(t, kn.bytes * static_cast<double>(inner));
            const double bpc = clk.ghz > 0.0 ? gbs / clk.ghz : 0.0;
            const double agg = aggregate_gbs(agg_ws, threads, kn.k, passes);

            print_row(l.name, kn.label, l.working_set, gbs, bpc, t.relative_spread(),
                      agg);

            rows.push_back({l.name, kn.label, l.working_set, 1, gbs, bpc,
                            t.relative_spread()});
            rows.push_back({l.name, std::string(kn.label) + "_aggregate", agg_ws,
                            threads, agg, 0.0, 0.0});
        }
    }

    std::printf(
        "\nWrite and copy count the bytes the PROGRAM moved (the STREAM convention).\n"
        "On a write-allocate machine without non-temporal stores the memory system\n"
        "moves about twice that for the DRAM rows, because each stored line is read\n"
        "for ownership first. Cache-resident rows are unaffected: the lines are\n"
        "already present after the warm-up pass.\n");

    // -- Storage layer -----------------------------------------------------
    std::string io_mode = "skipped";
    if (!ppe::has_flag(argc, argv, "--no-storage")) {
        ppe::trace::scope span("storage", "bandwidth");
        const std::size_t file_bytes =
            static_cast<std::size_t>(parse_int(argc, argv, "--storage-mib", 256)) * kMiB;

        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec) {
            std::printf("\nstorage : no temp directory available (%s)\n",
                        ec.message().c_str());
        } else {
            const std::filesystem::path path = dir / "ppe_layer_bandwidth.bin";
            bool ok = false;
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                if (out) {
                    std::vector<char> chunk(kMiB, 'z');
                    for (std::size_t w = 0; w < file_bytes; w += kMiB) {
                        out.write(chunk.data(), static_cast<std::streamsize>(kMiB));
                    }
                    out.flush();
                    ok = static_cast<bool>(out);
                }
            }
            // Without this the read sweep races the writeback of its own file
            // and reports the contention as the device's read speed -- measured
            // at 5x slow, see applications/storage_hierarchy.
            if (ok) ok = ppe::sync_file(path.string());

            if (ok) {
                ppe::file_handle f = ppe::open_for_read(path.string(), true);
                if (f.valid()) {
                    io_mode = f.mode;
                    constexpr std::size_t block = kMiB;
                    void* buf = ppe::aligned_alloc_bytes(block);
                    if (buf != nullptr) {
                        const ppe::timing t = ppe::time_with_spread(
                            [&] {
                                std::uint64_t off = 0;
                                while (off + block <= file_bytes) {
                                    if (ppe::read_at(f, buf, block, off) <= 0) break;
                                    off += block;
                                }
                            },
                            3);
                        const double gbs = to_gbs(t, static_cast<double>(file_bytes));
                        std::printf("\n%-6s %-6s %10s %10.2f %10s %7.1f%% %12s   (%s)\n",
                                    "store", "read", human(file_bytes).c_str(), gbs, "-",
                                    t.relative_spread() * 100.0, "-", f.mode.c_str());
                        if (!f.direct_io) {
                            std::printf(
                                "        ^ page cache NOT bypassed: this is memcpy from\n"
                                "          DRAM, not the device.\n");
                        }
                        rows.push_back({"storage", "read", file_bytes, 1, gbs, 0.0,
                                        t.relative_spread()});
                        ppe::aligned_free_bytes(buf);
                    }
                    ppe::close_file(f);
                }
            }
            std::filesystem::remove(path, ec);
        }
    }

    if (const char* csv = parse_str(argc, argv, "--csv"); csv != nullptr) {
        if (!write_csv(csv, prov, rows, io_mode)) return 1;
        std::printf("\nwrote %zu rows to %s\n", rows.size(), csv);
    }

    if (trace_path != nullptr) {
        ppe::trace::recorder::stats st;
        if (!ppe::trace::global().write_chrome_json(trace_path, prov, &st)) {
            std::fprintf(stderr, "error: cannot write trace to %s\n", trace_path);
            return 1;
        }
        std::printf("\ntrace : %zu events -> %s\n", st.recorded, trace_path);
    }

    return 0;
}
