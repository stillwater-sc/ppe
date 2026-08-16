// blocking_study -- how does GEMM tiling interact with the cache hierarchy?
//
// The question this repository was started to answer, and the reason detection
// had to come first: a blocking sweep over hardcoded block sizes is measuring
// arbitrary numbers, while a sweep whose sizes are derived from the machine's
// actual cache sizes is measuring a hypothesis.
//
// THE MODEL. A blocked GEMM with square tile b holds three tiles live at once --
// a panel of A, a panel of B, and the C tile being accumulated -- so its working
// set is
//
//     3 * b^2 * sizeof(double)  bytes
//
// Setting that equal to a cache's capacity gives the largest tile that level can
// hold:
//
//     b = sqrt(S / 24)
//
// so each detected level yields a candidate block size. The prediction is that
// performance peaks at the largest block whose working set still fits a fast
// level -- and the sweep is what tests it, including on the machines where it is
// wrong.
//
// WHAT IS STILL A PLACEHOLDER. One kernel, one thread, square tiles, and no
// packing. A production GEMM uses rectangular tiles chosen per level, packs
// panels into contiguous buffers, and has a register-blocked micro-kernel; those
// choices interact with the ones here. mtl5/ppe walks that progression in six
// documented steps and is the place to look for it.
//
// Not run for real numbers in CI: those need pinned cores and a quiet machine.

#include <ppe/cli.hpp>
#include <ppe/harness.hpp>
#include <ppe/platform.hpp>
#include <ppe/provenance.hpp>
#include <ppe/trace.hpp>
#include <ppe/version.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// Tiles held live by the blocked kernel below: A panel, B panel, C tile.
constexpr std::size_t kTilesLive = 3;

void print_help() {
    std::printf(
        "blocking_study -- GEMM blocking against the detected cache hierarchy (PPE %s)\n"
        "\n"
        "Usage: blocking_study [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help      show this help and exit\n"
        "      --size N    square matrix dimension (default 1024)\n"
        "      --trace P   write a Chrome Trace Event JSON of the sweep\n"
        "      --json      emit the provenance record as JSON and exit\n"
        "\n"
        "Block sizes are derived from the detected cache sizes: a tile of b holds\n"
        "3*b^2 doubles, so b = sqrt(S/24) is the largest tile level S can hold.\n"
        "Pin the process (taskset) before believing any number it prints.\n",
        ppe::version_string);
}

// Blocked ijk GEMM: C += A * B, all row-major, C pre-zeroed by the caller.
void gemm_blocked(const std::vector<double>& a, const std::vector<double>& b,
                  std::vector<double>& c, int n, int block) {
    for (int ii = 0; ii < n; ii += block) {
        const int i_end = (ii + block < n) ? ii + block : n;
        for (int kk = 0; kk < n; kk += block) {
            const int k_end = (kk + block < n) ? kk + block : n;
            for (int jj = 0; jj < n; jj += block) {
                const int j_end = (jj + block < n) ? jj + block : n;
                for (int i = ii; i < i_end; ++i) {
                    for (int k = kk; k < k_end; ++k) {
                        const double aik = a[static_cast<std::size_t>(i) * n + k];
                        for (int j = jj; j < j_end; ++j) {
                            c[static_cast<std::size_t>(i) * n + j] +=
                                aik * b[static_cast<std::size_t>(k) * n + j];
                        }
                    }
                }
            }
        }
    }
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

/// Bytes touched by one tile-triple at block size b.
std::size_t working_set(int block) {
    return kTilesLive * static_cast<std::size_t>(block) * static_cast<std::size_t>(block) *
           sizeof(double);
}

/// Largest block whose working set fits `bytes`, rounded down to a multiple of
/// 8 doubles -- one 64-byte cache line -- so the innermost j-loop walks whole
/// lines rather than straddling them.
int block_for_capacity(std::size_t bytes) {
    if (bytes == 0) return 0;
    double b = 0.0;
    const double target = static_cast<double>(bytes) / (kTilesLive * sizeof(double));
    b = std::sqrt(target);
    int blk = static_cast<int>(b);
    blk -= blk % 8;
    return blk < 8 ? 8 : blk;
}

struct level {
    const char* name;
    std::size_t bytes;
    std::size_t sharing_cores;
};

/// The fastest level whose capacity holds `bytes`, or nullptr for none.
const level* fits_in(const std::vector<level>& levels, std::size_t bytes) {
    for (const level& l : levels) {
        if (l.bytes > 0 && bytes <= l.bytes) return &l;
    }
    return nullptr;
}

struct sample {
    int         block = 0;
    std::size_t ws = 0;
    std::string fits;
    const char* derived_from = nullptr;  // level this block was derived for
    double      seconds = 0.0;
    double      gflops = 0.0;
    double      spread = 0.0;            // (max-min)/median over the reps
    bool        verified = false;
};

std::string human(std::size_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.3g MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.3g KiB", static_cast<double>(bytes) / 1024.0);
    }
    return std::string(buf);
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

    const int n = parse_int(argc, argv, "--size", 1024);

    std::fputs(ppe::to_text(prov).c_str(), stdout);
    std::printf("size    : %d x %d (%s per matrix)\n", n, n,
                human(static_cast<std::size_t>(n) * n * sizeof(double)).c_str());

    // -- The machine model this study is parameterized against ------------
    // Ordered fastest first, which is the order fits_in() searches.
    const std::vector<level> levels = {
        {"L1d", prov.cpu.l1d_bytes, prov.cpu.l1d_sharing_cores},
        {"L2",  prov.cpu.l2_bytes,  prov.cpu.l2_sharing_cores},
        {"L3",  prov.cpu.l3_bytes,  prov.cpu.l3_sharing_cores},
    };

    const bool have_hierarchy =
        prov.cpu.l1d_bytes > 0 || prov.cpu.l2_bytes > 0 || prov.cpu.l3_bytes > 0;

    std::printf("\nDetected hierarchy (%s):\n",
                prov.cpu.source.empty() ? "no backend" : prov.cpu.source.c_str());
    if (!have_hierarchy) {
        std::printf("  nothing detected -- falling back to a generic block ladder.\n"
                    "  The sweep below is measuring arbitrary sizes, not a hypothesis.\n");
    }
    for (const level& l : levels) {
        if (l.bytes == 0) {
            std::printf("  %-4s not detected\n", l.name);
            continue;
        }
        const int blk = block_for_capacity(l.bytes);
        std::printf("  %-4s %-10s -> block %4d  (3 tiles = %s)", l.name,
                    human(l.bytes).c_str(), blk, human(working_set(blk)).c_str());
        if (l.sharing_cores > 1) {
            std::printf("  shared by %zu cores", l.sharing_cores);
        }
        std::printf("\n");
    }

    // A shared level is reported at its full size because this study is single
    // threaded: one thread may use all of a shared L3. Under a parallel kernel
    // the per-core budget (bytes / sharing_cores) is the right divisor, and the
    // derived block would shrink accordingly.
    if (prov.cpu.l3_sharing_cores > 1) {
        std::printf("  NOTE: shared levels are counted at full size -- this study is\n"
                    "        single threaded. A parallel kernel must divide by the\n"
                    "        sharing count.\n");
    }

    // -- Candidate block sizes -------------------------------------------
    // The derived sizes are the hypothesis; the ladder around them is what
    // makes a peak visible rather than assumed. Without neighbours a "winner"
    // is just the largest of three arbitrary points.
    std::vector<int> blocks;
    std::vector<std::pair<int, const char*>> derived;

    for (const level& l : levels) {
        if (l.bytes == 0) continue;
        const int blk = block_for_capacity(l.bytes);
        if (blk <= n) {
            blocks.push_back(blk);
            derived.emplace_back(blk, l.name);
        }
    }
    for (int b = 8; b <= n; b *= 2) blocks.push_back(b);
    blocks.push_back(n);  // one tile: the unblocked baseline

    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());

    // -- Data --------------------------------------------------------------
    const std::size_t elements = static_cast<std::size_t>(n) * n;
    std::vector<double> a(elements);
    std::vector<double> b(elements);
    std::vector<double> c(elements);

    // Seeded explicitly: a measurement you cannot re-run on the same data is
    // not one you can bisect. Small integral values keep the products exactly
    // representable, so a blocking bug shows up as a real mismatch rather than
    // as rounding noise.
    ppe::fill(a, 12345);
    ppe::fill(b, 67890);

    // Reference: the whole matrix as one tile. Verification is not optional in
    // a performance sweep -- a blocking bug that drops a tile is usually
    // faster, and a sweep that records only times reports it as progress.
    std::vector<double> reference(elements, 0.0);
    {
        ppe::trace::scope span("reference", "gemm");
        gemm_blocked(a, b, reference, n, n);
    }

    // 2*n^3 flops for a square GEMM: one multiply and one add per inner step.
    const double flops = 2.0 * static_cast<double>(n) * n * n;

    std::printf("\n%-7s %11s %-8s %11s %10s %8s %9s\n", "block", "3-tile WS", "fits",
                "median s", "GFLOP/s", "spread", "verified");

    std::vector<sample> samples;
    for (const int block : blocks) {
        if (block > n) continue;
        ppe::trace::scope span("block_sweep", "gemm");

        // The zeroing is inside the timed region: gemm_blocked accumulates into
        // C, so each repetition must start from the same state to be a
        // repetition at all. It costs an O(n^2) fill against O(n^3) of work.
        const ppe::timing t = ppe::time_with_spread(
            [&] {
                std::fill(c.begin(), c.end(), 0.0);
                gemm_blocked(a, b, c, n, block);
            },
            5);

        sample s;
        s.block = block;
        s.ws = working_set(block);
        s.seconds = t.median;
        s.gflops = t.median > 0.0 ? flops / t.median / 1e9 : 0.0;
        s.spread = t.relative_spread();
        s.verified = ppe::matches(c, reference);

        const level* l = fits_in(levels, s.ws);
        s.fits = (l != nullptr) ? l->name : (have_hierarchy ? "DRAM" : "?");
        for (const auto& d : derived) {
            if (d.first == block) s.derived_from = d.second;
        }

        std::printf("%-7d %11s %-8s %11.6f %10.2f %7.1f%% %9s", s.block,
                    human(s.ws).c_str(), s.fits.c_str(), s.seconds, s.gflops,
                    s.spread * 100.0, s.verified ? "yes" : "NO");
        if (s.derived_from != nullptr) std::printf("   <- %s-sized", s.derived_from);
        std::printf("\n");
        std::fflush(stdout);

        if (!s.verified) {
            std::printf("        ^ block %d does not reproduce the reference\n", block);
        }
        samples.push_back(std::move(s));
    }

    // -- Did the model predict the winner? --------------------------------
    // NOT by picking the single best sample. Measured on this machine, the top
    // few blocks sit within ~5% of each other and the argmax moves between runs
    // -- 256 on one run, 512 on the next -- so a conclusion drawn from the
    // argmax alone is a conclusion drawn from noise, restated with full
    // confidence each time.
    //
    // Instead: take the noise floor as the largest per-block spread observed in
    // this sweep (with a 2% floor, since five repetitions cannot resolve better
    // than that), and treat every block within that band of the best as
    // indistinguishable. The model is supported if ANY derived block is in that
    // band, and contradicted only if none is.
    if (!samples.empty() && have_hierarchy) {
        double best = 0.0;
        double noise = 0.02;
        for (const sample& s : samples) {
            if (!s.verified) continue;
            if (s.gflops > best) best = s.gflops;
            if (s.spread > noise) noise = s.spread;
        }

        std::printf("\nNoise floor: %.1f%% (largest per-block spread in this sweep).\n",
                    noise * 100.0);
        std::printf("Blocks within that of the best (%.2f GFLOP/s) are indistinguishable:\n",
                    best);

        bool derived_in_band = false;
        int in_band = 0;
        int candidates = 0;
        for (const sample& s : samples) {
            if (!s.verified) continue;
            ++candidates;
            if (s.gflops < best * (1.0 - noise)) continue;
            ++in_band;
            std::printf("  block %-5d %6.2f GFLOP/s  fits %-4s", s.block, s.gflops,
                        s.fits.c_str());
            if (s.derived_from != nullptr) {
                std::printf("   <- %s-sized", s.derived_from);
                derived_in_band = true;
            }
            std::printf("\n");
        }

        // DOES THIS SWEEP DISCRIMINATE AT ALL? Asked before any conclusion is
        // drawn, because the answer here is usually no and the failure is
        // silent otherwise.
        //
        // Measured on the development machine: consecutive runs of this sweep
        // reported "consistent with the model" and "the model does not hold"
        // -- two opposite conclusions, each stated with full confidence, from a
        // band whose membership moved with the noise floor. Absolute throughput
        // moved 14.5 to 12.2 GFLOP/s between those runs as well. A sweep in
        // which most candidates are indistinguishable has not measured a
        // preference, and saying so is the result.
        if (in_band * 2 >= candidates) {
            std::printf(
                "\nTHIS SWEEP DOES NOT DISCRIMINATE: %d of %d candidates fall within\n"
                "the noise floor, so it cannot support a claim about any of them --\n"
                "including the model's. Consecutive runs of exactly this sweep have\n"
                "reached opposite conclusions.\n"
                "\nTo get a sweep that can discriminate: raise --size so each\n"
                "measurement runs long enough to dominate its own timing noise, pin\n"
                "the process (taskset), fix the clock, and quiet the machine.\n",
                in_band, candidates);
        } else if (derived_in_band) {
            std::printf(
                "\nA cache-derived block is among the indistinguishable best, and the\n"
                "band is narrow: consistent with the model.\n");
        } else {
            std::printf(
                "\nNo cache-derived block reaches the top band. The model's prediction\n"
                "-- that the largest tile fitting a fast level wins -- does not hold\n"
                "here. Square tiles and an unpacked kernel are both simplifications,\n"
                "and either can move the optimum.\n");
        }
    }

    if (trace_path != nullptr) {
        ppe::trace::recorder::stats st;
        if (!ppe::trace::global().write_chrome_json(trace_path, prov, &st)) {
            std::fprintf(stderr, "error: cannot write trace to %s\n", trace_path);
            return 1;
        }
        std::printf("\ntrace : %zu events -> %s\n", st.recorded, trace_path);
        if (st.dropped > 0) {
            std::printf("        WARNING: %zu events dropped; the trace has gaps\n",
                        st.dropped);
        }
    }

    std::printf(
        "\nStill simplified: one kernel, one thread, square tiles, no packing.\n"
        "mtl5/ppe walks the packing and register-tiling progression in six steps.\n");
    return 0;
}
