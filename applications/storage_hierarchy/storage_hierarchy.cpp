// storage_hierarchy -- the same curve, one level down.
//
// applications/memory_hierarchy sweeps working-set size against latency and
// bandwidth. This sweeps BLOCK SIZE and QUEUE DEPTH against the same two
// quantities, on a file. The shapes rhyme because the underlying structure is
// the same: a level of the hierarchy characterized by latency, bandwidth and
// capacity as a function of request size and concurrency.
//
// What differs is where the knees come from. In DRAM they mark cache capacity;
// here they mark the device's minimum useful transfer (below which per-request
// overhead dominates) and its internal parallelism (above which more in-flight
// requests stop helping). An NVMe queue-depth saturation point and an L3
// capacity cliff are the same kind of finding at different scales.
//
// THE PAGE CACHE IS THE ADVERSARY. Reading a file just written, through the
// buffered path, measures memcpy from DRAM and reports it as storage bandwidth:
// a huge, smooth, plausible number describing nothing about the device. This
// probe requests a cache bypass (O_DIRECT / F_NOCACHE / FILE_FLAG_NO_BUFFERING),
// reports which mode it actually got, and refuses to present a buffered result
// as a device measurement. See ppe/detect/fileio.hpp.
//
// IT WRITES A FILE. Size is bounded, the path is explicit, and the file is
// removed on exit including on error. It writes only to a file it created.

#include <ppe/cli.hpp>
#include <ppe/detect/fileio.hpp>
#include <ppe/harness.hpp>
#include <ppe/platform.hpp>
#include <ppe/provenance.hpp>
#include <ppe/version.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kMiB = 1024u * 1024u;

void print_help() {
    std::printf(
        "storage_hierarchy -- latency and bandwidth vs block size and queue depth (PPE %s)\n"
        "\n"
        "Usage: storage_hierarchy [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help          show this help and exit\n"
        "      --dir PATH      where to place the test file (default: temp dir)\n"
        "      --mib N         test file size in MiB (default 256)\n"
        "      --queue N       max concurrent readers for the depth sweep (default 16)\n"
        "      --reads N       random reads per sample (default 512; lower is faster)\n"
        "      --no-direct     do not request a cache bypass (measures the page cache)\n"
        "      --keep          do not delete the test file on exit\n"
        "      --csv PATH      write results as CSV, with provenance comments\n"
        "      --json          emit the provenance record as JSON and exit\n"
        "\n"
        "Writes a file of --mib MiB and deletes it on exit. Without a working cache\n"
        "bypass the numbers describe the page cache, not the device; the report says\n"
        "which mode it got.\n",
        ppe::version_string);
}

struct row {
    std::string probe;
    std::size_t block_bytes;
    unsigned    queue_depth;
    double      value;
    std::string unit;
};

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

/// Create the test file. Content is incompressible-ish random bytes: a file of
/// zeros can be serviced from a sparse extent or deduplicated by the filesystem,
/// and then the probe measures the filesystem's cleverness rather than the
/// device.
bool create_test_file(const std::filesystem::path& path, std::size_t bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    constexpr std::size_t chunk = 1u << 20;
    std::vector<char> buf(chunk);
    std::mt19937_64 g(0xA5A5A5A5);
    for (std::size_t i = 0; i < chunk; i += sizeof(std::uint64_t)) {
        const std::uint64_t v = g();
        std::memcpy(buf.data() + i, &v, sizeof(v));
    }

    for (std::size_t written = 0; written < bytes; written += chunk) {
        const std::size_t n = (bytes - written < chunk) ? (bytes - written) : chunk;
        out.write(buf.data(), static_cast<std::streamsize>(n));
        if (!out) return false;
    }
    out.flush();
    return static_cast<bool>(out);
}

/// Sequential read bandwidth at one block size, single reader.
double seq_read_gbs(const ppe::file_handle& f, std::size_t file_bytes,
                    std::size_t block) {
    void* buf = ppe::aligned_alloc_bytes(block);
    if (buf == nullptr) return 0.0;

    std::uint64_t total = 0;
    const double seconds = ppe::time_median(
        [&] {
            std::uint64_t off = 0;
            while (off + block <= file_bytes) {
                const std::ptrdiff_t got = ppe::read_at(f, buf, block, off);
                if (got <= 0) break;
                total += static_cast<std::uint64_t>(got);
                off += block;
            }
        },
        3);

    ppe::aligned_free_bytes(buf);
    if (seconds <= 0.0 || total == 0) return 0.0;
    // Bytes actually read in ONE pass, not the accumulated total across
    // repetitions: time_median reports the per-call time.
    const double per_pass = static_cast<double>((file_bytes / block) * block);
    return per_pass / seconds / 1e9;
}

/// Random-read latency at one block size, ONE request outstanding.
///
/// One at a time is what makes this a latency: the next offset is chosen from a
/// precomputed list, but the read is issued only after the previous returns, so
/// the device never has more than one request to work on.
double random_read_latency_us(const ppe::file_handle& f, std::size_t file_bytes,
                              std::size_t block, unsigned reads) {
    void* buf = ppe::aligned_alloc_bytes(block);
    if (buf == nullptr) return 0.0;

    const std::size_t slots = file_bytes / block;
    if (slots < 2) { ppe::aligned_free_bytes(buf); return 0.0; }

    std::vector<std::uint64_t> offsets(reads);
    std::mt19937_64 g(0xC0FFEE);
    for (auto& o : offsets) {
        o = static_cast<std::uint64_t>(g() % slots) * block;
    }

    std::uint64_t total = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const std::uint64_t off : offsets) {
        const std::ptrdiff_t got = ppe::read_at(f, buf, block, off);
        if (got <= 0) break;
        total += static_cast<std::uint64_t>(got);
    }
    const auto t1 = std::chrono::steady_clock::now();

    ppe::aligned_free_bytes(buf);
    if (total == 0) return 0.0;
    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    return seconds / reads * 1e6;
}

/// Aggregate random-read bandwidth with `depth` concurrent readers.
///
/// Measured on ONE wall clock spanning all threads, for the reason
/// memory_hierarchy documents: per-thread intervals report N serialized runs as
/// N concurrent ones when the threads do not actually overlap.
double random_read_depth_gbs(const ppe::file_handle& f, std::size_t file_bytes,
                             std::size_t block, unsigned depth, unsigned reads_each) {
    const std::size_t slots = file_bytes / block;
    if (slots < 2 || depth == 0) return 0.0;

    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::uint64_t> got(depth, 0);
    std::vector<std::thread> pool;
    pool.reserve(depth);

    for (unsigned t = 0; t < depth; ++t) {
        pool.emplace_back([&, t] {
            void* buf = ppe::aligned_alloc_bytes(block);
            if (buf == nullptr) { ready.fetch_add(1); return; }

            std::vector<std::uint64_t> offsets(reads_each);
            std::mt19937_64 g(0xBEEF + t);
            for (auto& o : offsets) {
                o = static_cast<std::uint64_t>(g() % slots) * block;
            }

            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }

            std::uint64_t local = 0;
            for (const std::uint64_t off : offsets) {
                const std::ptrdiff_t n = ppe::read_at(f, buf, block, off);
                if (n <= 0) break;
                local += static_cast<std::uint64_t>(n);
            }
            got[t] = local;
            ppe::aligned_free_bytes(buf);
        });
    }

    while (ready.load() < depth) { /* spin until every thread has its buffer */ }
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : pool) th.join();
    const auto t1 = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    const double moved = static_cast<double>(std::accumulate(got.begin(), got.end(),
                                                             std::uint64_t{0}));
    return seconds > 0.0 ? moved / seconds / 1e9 : 0.0;
}

bool write_csv(const char* path, const ppe::provenance& prov,
               const std::vector<row>& rows, const std::string& io_mode) {
    std::FILE* fp = std::fopen(path, "w");
    if (fp == nullptr) {
        std::fprintf(stderr, "error: cannot open %s for writing\n", path);
        return false;
    }
    std::fputs(ppe::to_csv_comment(prov).c_str(), fp);
    // The IO mode belongs in the provenance of a storage result exactly as the
    // ISA does for a compute one: it is the difference between measuring a
    // device and measuring DRAM.
    std::fprintf(fp, "# io_mode=%s\n", io_mode.c_str());
    std::fputs("probe,block_bytes,queue_depth,value,unit\n", fp);
    for (const row& r : rows) {
        std::fprintf(fp, "%s,%zu,%u,%.6f,%s\n", r.probe.c_str(), r.block_bytes,
                     r.queue_depth, r.value, r.unit.c_str());
    }
    std::fclose(fp);
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

    const std::size_t file_bytes =
        static_cast<std::size_t>(parse_int(argc, argv, "--mib", 256)) * kMiB;
    const unsigned max_depth =
        static_cast<unsigned>(parse_int(argc, argv, "--queue", 16));
    // Random reads are the slow part of this probe: on rotating storage a
    // single 4 KiB read costs milliseconds, so the default is a compromise
    // between resolution and a sweep that finishes.
    const unsigned rand_reads =
        static_cast<unsigned>(parse_int(argc, argv, "--reads", 512));
    const bool want_direct = !ppe::has_flag(argc, argv, "--no-direct");
    const bool keep = ppe::has_flag(argc, argv, "--keep");

    std::error_code ec;
    std::filesystem::path dir;
    if (const char* d = parse_str(argc, argv, "--dir"); d != nullptr) {
        dir = d;
    } else {
        dir = std::filesystem::temp_directory_path(ec);
        if (ec) {
            std::fprintf(stderr, "error: no temp directory available: %s\n",
                         ec.message().c_str());
            return 1;
        }
    }

    const std::filesystem::path path = dir / "ppe_storage_probe.bin";

    std::fputs(ppe::to_text(prov).c_str(), stdout);
    std::printf("file    : %s (%zu MiB)\n", path.string().c_str(), file_bytes / kMiB);

    if (!create_test_file(path, file_bytes)) {
        std::fprintf(stderr, "error: could not write %s\n", path.string().c_str());
        std::filesystem::remove(path, ec);
        return 1;
    }

    // Wait for the file to actually reach the device before measuring reads
    // from it. Without this the sweep races the writeback of its own test file
    // and reports the contention as the device's read speed -- measured at 5x
    // slow on this machine. See ppe::sync_file.
    if (!ppe::sync_file(path.string())) {
        std::printf("  warning: could not fsync the test file; the read sweep may\n"
                    "           be competing with its own writeback\n");
    }

    // RAII-ish cleanup for every exit below.
    struct remover {
        const std::filesystem::path& p;
        bool keep;
        ~remover() {
            if (!keep) {
                std::error_code e;
                std::filesystem::remove(p, e);
            }
        }
    } cleanup{path, keep};

    ppe::file_handle f = ppe::open_for_read(path.string(), want_direct);
    if (!f.valid()) {
        std::fprintf(stderr, "error: could not open %s for reading\n",
                     path.string().c_str());
        return 1;
    }

    std::printf("io mode : %s%s\n", f.mode.c_str(),
                f.direct_io ? " (cache bypassed)" : "");
    if (!f.note.empty()) std::printf("          %s\n", f.note.c_str());
    if (!f.direct_io) {
        std::printf(
            "\n  WARNING: reads are going through the page cache. The file was just\n"
            "  written, so much of it is resident in DRAM and the numbers below\n"
            "  describe memcpy, not the device. They are a valid measurement of the\n"
            "  BUFFERED path -- which is what most applications see -- but they are\n"
            "  not a device characterization. Do not compare them with direct-mode\n"
            "  results from another machine.\n");
    }
    std::printf("\n");

    std::vector<row> rows;

    // -- Block size sweep -------------------------------------------------
    // From one page to a large sequential request. Below the device's minimum
    // useful transfer, per-request overhead dominates and bandwidth collapses
    // while latency stays flat -- the storage analogue of a cache-line effect.
    std::printf("%-12s %14s %16s\n", "block", "seq GB/s", "rand latency us");
    for (std::size_t block = 4096; block <= 4 * kMiB; block *= 4) {
        if (block > file_bytes / 4) break;
        const double bw = seq_read_gbs(f, file_bytes, block);
        // Fewer samples for large blocks: each one transfers more, so the
        // per-request overhead being measured is a smaller share of a longer
        // operation and needs less averaging to resolve.
        const unsigned reads = (block >= kMiB) ? (rand_reads / 8 ? rand_reads / 8 : 1)
                                               : rand_reads;
        const double lat = random_read_latency_us(f, file_bytes, block, reads);

        rows.push_back({"seq_bandwidth", block, 1, bw, "GB/s"});
        rows.push_back({"rand_latency", block, 1, lat, "us"});
        std::printf("%-12s %14.3f %16.2f\n", human_size(block).c_str(), bw, lat);
        std::fflush(stdout);
    }

    // -- Queue depth sweep ------------------------------------------------
    // A rotating disk saturates at depth 1-2; an NVMe device keeps scaling into
    // the tens because it has many independent channels. Where this flattens IS
    // the device's internal parallelism.
    const std::size_t depth_block = 4096;
    std::printf("\nQueue depth at %s random reads:\n", human_size(depth_block).c_str());
    std::printf("%-10s %16s %14s\n", "depth", "aggregate GB/s", "vs depth 1");
    double single = 0.0;
    for (unsigned d = 1; d <= max_depth; d *= 2) {
        const double gbs =
            random_read_depth_gbs(f, file_bytes, depth_block, d, rand_reads / 2 + 1);
        if (d == 1) single = gbs;
        rows.push_back({"rand_bandwidth", depth_block, d, gbs, "GB/s"});
        std::printf("%-10u %16.3f %13.2fx\n", d, gbs, single > 0.0 ? gbs / single : 0.0);
        std::fflush(stdout);
    }

    ppe::close_file(f);

    if (const char* csv = parse_str(argc, argv, "--csv"); csv != nullptr) {
        if (!write_csv(csv, prov, rows, f.mode)) return 1;
        std::printf("\nwrote %zu rows to %s\n", rows.size(), csv);
    }

    if (keep) std::printf("\nkept %s\n", path.string().c_str());
    return 0;
}
