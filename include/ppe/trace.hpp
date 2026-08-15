// trace.hpp -- event capture for the measurement pipeline.
//
// detect -> model -> measure -> TRACE -> visualize. This is the fourth stage:
// the event stream that studies annotate themselves with and that schedule and
// occupancy views are drawn from.
//
// A TRACER THAT PERTURBS WHAT IT MEASURES IS WORSE THAN NO TRACER, because the
// perturbation is invisible in the output. Four properties follow from that, and
// each costs something in the design:
//
//   NO ALLOCATION ON THE HOT PATH. Buffers are reserved when a thread first
//   records. An allocation inside a span would put the allocator's cost inside
//   the measurement, and a page fault inside a microsecond-scale span dominates
//   it entirely.
//
//   NO LOCKS ON THE HOT PATH. Each thread owns its buffer; the registry mutex is
//   taken when a thread first registers and again at export, never per event. A
//   shared buffer would serialize the threads whose concurrency is the thing
//   under study -- the same error as a serialized server in a connection sweep.
//
//   NO STRING COPYING. Names are `const char*` with static storage. Copying a
//   name per event would allocate, and interning it would hash. Passing a
//   pointer to a temporary is the one way to misuse this API, so it is spelled
//   out here: names MUST outlive the recorder.
//
//   BOUNDED, WITH DROPS COUNTED AND REPORTED. Capacity is fixed. When a buffer
//   fills, events are dropped and the drop is counted -- and the export refuses
//   to stay quiet about it. A trace with silent gaps produces a schedule
//   animation with invisible holes, which is a picture that lies.
//
// MEASURED OVERHEAD, on an i7-12700K P-core, 2M spans, -O2:
//
//     disabled    0.2 ns per span
//     enabled    32.3 ns per span
//
// Disabled cost is a latched relaxed atomic load; the compiler removes the rest.
// Enabled cost is dominated by two steady_clock reads, which is the floor for
// any wall-clock span and the reason spans should wrap work measured in
// microseconds, not nanoseconds. A span around a 100 ns operation is measuring
// the tracer.
//
// Those numbers are in this comment rather than in a test because they are
// properties of the runner: asserting them on shared CI hardware would be a
// flake generator. tests/trace.cpp asserts the bookkeeping instead, which has
// right answers everywhere.
#pragma once

#include <ppe/provenance.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ppe::trace {

/// One completed span. 32 bytes, trivially copyable, no owned memory.
struct event {
    const char*   name = nullptr;   ///< static storage, never copied
    const char*   category = nullptr;
    std::uint64_t start_ns = 0;     ///< relative to the recorder's epoch
    std::uint64_t duration_ns = 0;
};

namespace detail {

struct thread_buffer {
    std::vector<event> events;
    std::size_t        dropped = 0;
    std::uint32_t      tid = 0;
    std::string        name;
};

/// Minimal JSON string escaping for names that came from the OS (a CPU brand
/// can contain quotes and backslashes on some parts).
inline std::string json_escape_c(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[7];
                    std::snprintf(esc, sizeof(esc), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace detail

/// Process-wide event sink.
///
/// One recorder per process in practice; it is a class rather than a set of free
/// functions so a test can build an isolated one.
class recorder {
public:
    /// `capacity_per_thread` events are reserved on each thread's first record.
    /// 1 << 16 spans is about 2 MB per thread.
    explicit recorder(std::size_t capacity_per_thread = 1u << 16)
        : capacity_(capacity_per_thread),
          epoch_(std::chrono::steady_clock::now()) {}

    void enable(bool on = true) { enabled_.store(on, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    /// Nanoseconds since this recorder's epoch.
    std::uint64_t now_ns() const {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - epoch_)
                .count());
    }

    /// Record a completed span. `name` and `category` must have static storage.
    void record(const char* name, const char* category, std::uint64_t start_ns,
                std::uint64_t duration_ns) {
        if (!enabled()) return;
        detail::thread_buffer* b = local_buffer();
        if (b == nullptr) return;
        if (b->events.size() >= capacity_) {
            ++b->dropped;
            return;
        }
        b->events.push_back(event{name, category, start_ns, duration_ns});
    }

    /// Name the calling thread, for the trace viewer's lane labels.
    void name_thread(std::string name) {
        detail::thread_buffer* b = local_buffer();
        if (b != nullptr) b->name = std::move(name);
    }

    /// Total events retained and total dropped, across all threads.
    struct stats {
        std::size_t recorded = 0;
        std::size_t dropped = 0;
        std::size_t threads = 0;
    };

    stats collect_stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        stats s;
        s.threads = buffers_.size();
        for (const auto& b : buffers_) {
            s.recorded += b->events.size();
            s.dropped += b->dropped;
        }
        return s;
    }

    /// Write the trace in Chrome Trace Event Format.
    ///
    /// Read by Perfetto UI, chrome://tracing and speedscope, so a trace is
    /// viewable without this project shipping a viewer. `ts` and `dur` are
    /// MICROSECONDS in that format -- nanoseconds here are converted, and
    /// getting that wrong silently rescales every duration by 1000.
    ///
    /// Returns false on an I/O failure. Drops are written into the trace's own
    /// metadata AND returned in `out_stats`, so a caller cannot present a
    /// truncated trace as complete by ignoring a return value.
    bool write_chrome_json(const char* path, const provenance& prov,
                           stats* out_stats = nullptr) const {
        std::lock_guard<std::mutex> lock(mutex_);

        stats s;
        s.threads = buffers_.size();
        for (const auto& b : buffers_) {
            s.recorded += b->events.size();
            s.dropped += b->dropped;
        }
        if (out_stats != nullptr) *out_stats = s;

        std::FILE* f = std::fopen(path, "w");
        if (f == nullptr) return false;

        std::fputs("{\n  \"displayTimeUnit\": \"ns\",\n  \"traceEvents\": [\n", f);

        bool first = true;
        for (const auto& b : buffers_) {
            // Metadata event naming the lane.
            if (!b->name.empty()) {
                std::fprintf(f,
                             "%s    {\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,"
                             "\"tid\":%u,\"args\":{\"name\":\"%s\"}}",
                             first ? "" : ",\n", b->tid,
                             detail::json_escape_c(b->name).c_str());
                first = false;
            }
            for (const event& e : b->events) {
                std::fprintf(f,
                             "%s    {\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"X\","
                             "\"ts\":%.3f,\"dur\":%.3f,\"pid\":1,\"tid\":%u}",
                             first ? "" : ",\n",
                             e.name ? e.name : "unnamed",
                             e.category ? e.category : "",
                             static_cast<double>(e.start_ns) / 1000.0,
                             static_cast<double>(e.duration_ns) / 1000.0, b->tid);
                first = false;
            }
        }

        std::fputs("\n  ],\n  \"otherData\": {\n", f);
        // Provenance travels inside the trace, like it does inside a result CSV:
        // a trace that cannot be attributed to a build and a machine is a
        // picture, not evidence.
        std::fprintf(f, "    \"ppe_version\": \"%s\",\n", prov.ppe_version.c_str());
        std::fprintf(f, "    \"git_commit\": \"%s\",\n", prov.git_commit.c_str());
        std::fprintf(f, "    \"git_dirty\": \"%s\",\n", prov.git_dirty.c_str());
        std::fprintf(f, "    \"compiler\": \"%s\",\n", prov.compiler.c_str());
        std::fprintf(f, "    \"build_isa\": \"%s\",\n", prov.isa.c_str());
        std::fprintf(f, "    \"device\": \"%s\",\n",
                     detail::json_escape_c(prov.cpu.name).c_str());
        std::fprintf(f, "    \"utc\": \"%s\",\n", prov.utc_timestamp.c_str());
        std::fprintf(f, "    \"events_recorded\": %zu,\n", s.recorded);
        // Written even when zero: a reader should be able to confirm
        // completeness from the file rather than infer it from absence.
        std::fprintf(f, "    \"events_dropped\": %zu\n", s.dropped);
        std::fputs("  }\n}\n", f);

        std::fclose(f);
        return true;
    }

private:
    /// Per-thread buffer for THIS recorder.
    ///
    /// Keyed on a never-reused instance id, NOT on `this`. A recorder is often a
    /// local, and a second one constructed after the first is destroyed
    /// routinely lands on the same address -- a cache keyed on the pointer then
    /// matches the dead recorder and hands back a pointer into its freed buffer.
    /// That is a use-after-free, and it presents as the new recorder silently
    /// recording nothing, since the events land in memory nobody reads. Found by
    /// tests/trace.cpp, which constructs several recorders in sequence.
    ///
    /// The one-entry fast path covers the normal case of a single recorder; the
    /// vector behind it keeps alternating recorders from recreating a buffer on
    /// every call, which would leak one per alternation.
    detail::thread_buffer* local_buffer() {
        struct tls_entry {
            std::uint64_t id;
            detail::thread_buffer* buf;
        };
        thread_local std::uint64_t last_id = 0;
        thread_local detail::thread_buffer* last_buf = nullptr;
        thread_local std::vector<tls_entry> entries;

        if (last_buf != nullptr && last_id == id_) return last_buf;
        for (const tls_entry& e : entries) {
            if (e.id == id_) {
                last_id = e.id;
                last_buf = e.buf;
                return e.buf;
            }
        }

        auto buf = std::make_unique<detail::thread_buffer>();
        buf->events.reserve(capacity_);  // the only allocation, off the hot path
        buf->tid = next_tid_.fetch_add(1, std::memory_order_relaxed);

        detail::thread_buffer* raw = buf.get();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            buffers_.push_back(std::move(buf));
        }
        entries.push_back(tls_entry{id_, raw});
        last_id = id_;
        last_buf = raw;
        return raw;
    }

    static std::uint64_t next_instance_id() {
        static std::atomic<std::uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    std::uint64_t id_ = next_instance_id();
    std::size_t capacity_;
    std::chrono::steady_clock::time_point epoch_;
    std::atomic<bool> enabled_{false};
    std::atomic<std::uint32_t> next_tid_{1};

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<detail::thread_buffer>> buffers_;
};

/// The process-wide recorder.
inline recorder& global() {
    static recorder r;
    return r;
}

/// RAII span. Records on destruction.
///
/// THE DISABLED PATH READS NO CLOCK. An earlier version took the timestamp
/// unconditionally, on the reasoning that a branch was no cheaper than a
/// steady_clock read and that identical code shapes were worth something.
/// Measured, that cost 29.6 ns per span with tracing OFF against 41.7 ns with it
/// on -- so merely instrumenting a function perturbed it by ~30 ns whether or
/// not anyone was tracing, which is most of the way to the overhead the whole
/// design exists to avoid. Now a disabled span is one relaxed atomic load.
///
/// `active_` is latched at construction: enabling tracing part-way through a
/// span would otherwise record one with a start timestamp that was never taken.
class scope {
public:
    scope(const char* name, const char* category = "ppe", recorder& r = global())
        : rec_(r), name_(name), category_(category), active_(r.enabled()),
          start_ns_(active_ ? r.now_ns() : 0) {}

    ~scope() {
        if (!active_) return;
        rec_.record(name_, category_, start_ns_, rec_.now_ns() - start_ns_);
    }

    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;

private:
    recorder& rec_;
    const char* name_;
    const char* category_;
    bool active_;
    std::uint64_t start_ns_;
};

}  // namespace ppe::trace
