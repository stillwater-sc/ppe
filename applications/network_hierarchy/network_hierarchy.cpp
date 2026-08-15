// network_hierarchy -- the same curve, one level further out.
//
// memory_hierarchy sweeps working-set size; storage_hierarchy sweeps block size
// and queue depth; this sweeps MESSAGE SIZE and CONCURRENT CONNECTIONS against
// the same two quantities. The knees mean something different again -- the
// per-message overhead floor, and the point where more connections stop buying
// throughput -- but it is the same investigation at a further scale.
//
// THREE WAYS TO RUN IT
//
//   (default)          loopback: a server thread inside this process on an
//                      ephemeral 127.0.0.1 port. Self-contained, no setup.
//   --server           listen for a remote client and serve until stopped.
//   --connect HOST     measure against a remote --server.
//
// WHAT LOOPBACK MEASURES, AND WHAT IT DOES NOT. Traffic over 127.0.0.1 never
// reaches a wire: it is a memcpy through the kernel's network stack, with all
// the protocol processing and none of the physical transit. That bounds what any
// local IPC over TCP can achieve, which is a genuinely useful number -- but it
// is not a measurement of a NIC, a switch, or a cable, and comparing a loopback
// figure with a cross-host one is comparing two different levels.
//
// NAGLE. Every socket sets TCP_NODELAY. Without it a small-message ping-pong
// stalls on the interaction between Nagle's algorithm and the receiver's delayed
// ACK, reporting tens of milliseconds of "latency" on an interface capable of
// single-digit microseconds -- a stable, reproducible, completely wrong number.
// See ppe/detect/net.hpp.

#include <ppe/cli.hpp>
#include <ppe/detect/net.hpp>
#include <ppe/platform.hpp>
#include <ppe/provenance.hpp>
#include <ppe/version.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kMiB = 1024u * 1024u;

void print_help() {
    std::printf(
        "network_hierarchy -- latency and bandwidth vs message size (PPE %s)\n"
        "\n"
        "Usage: network_hierarchy [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help          show this help and exit\n"
        "      --server        run as a server and serve until stopped\n"
        "      --connect HOST  measure against a remote server (IPv4 address)\n"
        "      --port N        port for --server / --connect (default 47654)\n"
        "      --bind ADDR     address for --server to bind (default all)\n"
        "      --mib N         bytes per bandwidth sample, in MiB (default 64)\n"
        "      --iters N       round trips per latency sample (default 1000)\n"
        "      --conns N       max concurrent connections in the sweep (default 8)\n"
        "      --csv PATH      write results as CSV, with provenance comments\n"
        "      --json          emit the provenance record as JSON and exit\n"
        "\n"
        "With no mode flag it measures LOOPBACK: a server thread in this process.\n"
        "That is the kernel network stack, not a NIC. Use --server / --connect on\n"
        "two hosts to measure a real link.\n",
        ppe::version_string);
}

struct row {
    std::string probe;
    std::size_t msg_bytes;
    unsigned    connections;
    double      value;
    std::string unit;
};

std::string human_size(std::size_t bytes) {
    char buf[32];
    if (bytes >= kMiB) {
        std::snprintf(buf, sizeof(buf), "%.3g MiB", static_cast<double>(bytes) / kMiB);
    } else if (bytes >= 1024) {
        std::snprintf(buf, sizeof(buf), "%.3g KiB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%zu B", bytes);
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

/// Round-trip time in microseconds, ONE message in flight.
///
/// One at a time is what makes this a latency rather than a throughput: the next
/// send happens only after the echo of the previous arrives, so the link never
/// has more than one message to work on. The result is a full round trip, not
/// half of one -- halving it to quote a "one-way" figure assumes a symmetric
/// path, which is exactly the assumption a measurement should not smuggle in.
double pingpong_rtt_us(const char* host, unsigned short port, std::size_t msg,
                       unsigned iters) {
    ppe::socket_t s = ppe::connect_to(host, port);
    if (s == ppe::kInvalidSocket) return 0.0;

    ppe::net_request req;
    req.mode = ppe::net_mode::ping_pong;
    req.msg_bytes = static_cast<std::uint32_t>(msg);
    req.iterations = iters;
    unsigned char header[ppe::kHeaderBytes];
    ppe::encode(req, header);
    if (!ppe::send_all(s, header, sizeof(header))) { ppe::close_socket(s); return 0.0; }

    std::vector<char> out(msg, 'p');
    std::vector<char> in(msg);

    // Warm-up round trip: the first exchange pays for connection setup state,
    // route lookup and cold buffers, none of which is the steady-state latency.
    if (!ppe::send_all(s, out.data(), msg) || !ppe::recv_all(s, in.data(), msg)) {
        ppe::close_socket(s);
        return 0.0;
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned i = 1; i < iters; ++i) {
        if (!ppe::send_all(s, out.data(), msg)) { ppe::close_socket(s); return 0.0; }
        if (!ppe::recv_all(s, in.data(), msg)) { ppe::close_socket(s); return 0.0; }
    }
    const auto t1 = std::chrono::steady_clock::now();

    ppe::close_socket(s);
    if (iters < 2) return 0.0;
    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    return seconds / (iters - 1) * 1e6;
}

/// One-way streaming bandwidth in GB/s at a given message size.
double stream_gbs(const char* host, unsigned short port, std::size_t msg,
                  std::uint64_t total) {
    ppe::socket_t s = ppe::connect_to(host, port);
    if (s == ppe::kInvalidSocket) return 0.0;

    ppe::net_request req;
    req.mode = ppe::net_mode::stream;
    req.msg_bytes = static_cast<std::uint32_t>(msg);
    req.total_bytes = total;
    unsigned char header[ppe::kHeaderBytes];
    ppe::encode(req, header);
    if (!ppe::send_all(s, header, sizeof(header))) { ppe::close_socket(s); return 0.0; }

    std::vector<char> buf(msg, 's');
    const auto t0 = std::chrono::steady_clock::now();

    std::uint64_t left = total;
    while (left > 0) {
        const std::size_t n = static_cast<std::size_t>(left < msg ? left : msg);
        if (!ppe::send_all(s, buf.data(), n)) { ppe::close_socket(s); return 0.0; }
        left -= n;
    }

    // Wait for the receiver's acknowledgement before stopping the clock. Timing
    // to the last send() would measure how fast bytes entered the local socket
    // buffer, which on loopback is memcpy speed and on a real link is the send
    // window -- neither is the transfer.
    unsigned char ack[8];
    const bool ok = ppe::recv_all(s, ack, sizeof(ack));
    const auto t1 = std::chrono::steady_clock::now();
    ppe::close_socket(s);
    if (!ok) return 0.0;

    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    return seconds > 0.0 ? static_cast<double>(total) / seconds / 1e9 : 0.0;
}

/// Aggregate bandwidth across `conns` simultaneous connections.
///
/// Measured on ONE wall clock spanning every connection, for the reason
/// memory_hierarchy documents at length: per-thread intervals report N
/// serialized transfers as N concurrent ones.
double stream_conns_gbs(const char* host, unsigned short port, std::size_t msg,
                        std::uint64_t total_each, unsigned conns) {
    if (conns == 0) return 0.0;

    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<char> ok(conns, 0);
    std::vector<std::thread> pool;
    pool.reserve(conns);

    for (unsigned c = 0; c < conns; ++c) {
        pool.emplace_back([&, c] {
            ppe::socket_t s = ppe::connect_to(host, port);
            if (s == ppe::kInvalidSocket) { ready.fetch_add(1); return; }

            ppe::net_request req;
            req.mode = ppe::net_mode::stream;
            req.msg_bytes = static_cast<std::uint32_t>(msg);
            req.total_bytes = total_each;
            unsigned char header[ppe::kHeaderBytes];
            ppe::encode(req, header);
            if (!ppe::send_all(s, header, sizeof(header))) {
                ppe::close_socket(s);
                ready.fetch_add(1);
                return;
            }

            std::vector<char> buf(msg, 's');
            // Connect and send the header BEFORE the barrier, so the timed
            // region contains transfer only, not connection establishment.
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }

            std::uint64_t left = total_each;
            bool good = true;
            while (left > 0 && good) {
                const std::size_t n = static_cast<std::size_t>(left < msg ? left : msg);
                good = ppe::send_all(s, buf.data(), n);
                left -= n;
            }
            unsigned char ack[8];
            good = good && ppe::recv_all(s, ack, sizeof(ack));
            ok[c] = good ? 1 : 0;
            ppe::close_socket(s);
        });
    }

    while (ready.load() < conns) { /* spin until every connection is established */ }
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : pool) th.join();
    const auto t1 = std::chrono::steady_clock::now();

    const unsigned good = static_cast<unsigned>(
        std::accumulate(ok.begin(), ok.end(), 0));
    if (good == 0) return 0.0;

    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    const double moved = static_cast<double>(total_each) * good;
    return seconds > 0.0 ? moved / seconds / 1e9 : 0.0;
}

/// Accept and serve connections until `stop` is set.
///
/// The `stop` check after accept() is what makes shutdown work: see
/// stop_loopback_server. A connection that arrives once stopping has begun is
/// the wake-up, not a client, and must be closed rather than served -- serving
/// it would block in recv_all waiting for a header that is never sent.
/// THREAD PER CONNECTION, and that is a measurement requirement rather than a
/// scalability preference.
///
/// The first version served connections one at a time from the accept loop. The
/// concurrency sweep then measured THE SERVER'S SERIALIZATION rather than the
/// link: aggregate bandwidth across 8 connections came out at 0.67x of one
/// connection, because there was never more than one transfer in flight. A
/// sweep whose independent variable the program itself has pinned to 1 reports
/// a smooth, plausible curve about nothing.
void serve_loop(ppe::socket_t listener, std::atomic<bool>& stop) {
    std::vector<std::thread> workers;
    while (!stop.load(std::memory_order_acquire)) {
        ppe::socket_t c = ppe::accept_one(listener);
        if (c == ppe::kInvalidSocket) break;  // listener closed
        if (stop.load(std::memory_order_acquire)) {
            ppe::close_socket(c);
            break;
        }
        workers.emplace_back([c] {
            std::vector<char> scratch;
            ppe::serve_connection(c, scratch);
            ppe::close_socket(c);
        });
    }
    for (auto& w : workers) w.join();
}

/// Stop the in-process loopback server and join its thread.
///
/// CLOSING THE LISTENER IS NOT ENOUGH. A thread blocked in accept() is not
/// reliably woken by another thread closing the socket it is waiting on -- the
/// behaviour is unspecified on POSIX and the call simply stays blocked on Linux,
/// which hung this program at exit after the sweep had already finished. The
/// portable way to unblock an accept is to give it a connection to accept.
void stop_loopback_server(ppe::socket_t listener, unsigned short port,
                          std::atomic<bool>& stop, std::thread& server) {
    if (!server.joinable()) return;

    stop.store(true, std::memory_order_release);

    // Wake the accept with a throwaway connection. serve_loop sees `stop` and
    // closes it without reading, so no protocol exchange is expected.
    ppe::socket_t waker = ppe::connect_to("127.0.0.1", port);
    if (waker != ppe::kInvalidSocket) ppe::close_socket(waker);

    server.join();
    // Only now: closing it earlier would race the accept this just unblocked.
    ppe::close_socket(listener);
}

bool write_csv(const char* path, const ppe::provenance& prov,
               const std::vector<row>& rows, const std::string& target) {
    std::FILE* fp = std::fopen(path, "w");
    if (fp == nullptr) {
        std::fprintf(stderr, "error: cannot open %s for writing\n", path);
        return false;
    }
    std::fputs(ppe::to_csv_comment(prov).c_str(), fp);
    // Which level was measured is provenance, not a footnote: a loopback figure
    // and a cross-host figure are not comparable.
    std::fprintf(fp, "# network_target=%s\n", target.c_str());
    std::fprintf(fp, "# tcp_nodelay=1\n");
    std::fputs("probe,msg_bytes,connections,value,unit\n", fp);
    for (const row& r : rows) {
        std::fprintf(fp, "%s,%zu,%u,%.6f,%s\n", r.probe.c_str(), r.msg_bytes,
                     r.connections, r.value, r.unit.c_str());
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

    ppe::net_context net;
    if (!net.ok) {
        std::fprintf(stderr, "error: could not initialize sockets\n");
        return 1;
    }

    const unsigned short port =
        static_cast<unsigned short>(parse_int(argc, argv, "--port", 47654));

    // -- Server mode ------------------------------------------------------
    if (ppe::has_flag(argc, argv, "--server")) {
        const char* bind_addr = parse_str(argc, argv, "--bind");
        unsigned short bound = 0;
        ppe::socket_t listener = ppe::listen_on(port, bind_addr, &bound);
        if (listener == ppe::kInvalidSocket) {
            std::fprintf(stderr, "error: could not listen on port %u\n", port);
            return 1;
        }
        std::printf("PPE %s network server listening on %s:%u\n", ppe::version_string,
                    bind_addr ? bind_addr : "0.0.0.0", bound);
        std::printf("Run the client elsewhere:  network_hierarchy --connect <this host>"
                    " --port %u\n", bound);
        std::fflush(stdout);

        std::atomic<bool> stop{false};
        serve_loop(listener, stop);
        ppe::close_socket(listener);
        return 0;
    }

    const std::size_t total =
        static_cast<std::size_t>(parse_int(argc, argv, "--mib", 64)) * kMiB;
    const unsigned iters =
        static_cast<unsigned>(parse_int(argc, argv, "--iters", 1000));
    const unsigned max_conns =
        static_cast<unsigned>(parse_int(argc, argv, "--conns", 8));

    // -- Choose a target --------------------------------------------------
    const char* remote = parse_str(argc, argv, "--connect");
    std::string target;
    const char* host = "127.0.0.1";
    unsigned short target_port = port;

    // Loopback server, when no remote was given.
    std::atomic<bool> stop{false};
    ppe::socket_t listener = ppe::kInvalidSocket;
    std::thread server;

    if (remote != nullptr) {
        host = remote;
        target = std::string("remote ") + remote + ":" + std::to_string(port);
    } else {
        unsigned short bound = 0;
        // Ephemeral port on loopback: no fixed port to collide with another
        // instance, and nothing reachable from outside this machine.
        listener = ppe::listen_on(0, "127.0.0.1", &bound);
        if (listener == ppe::kInvalidSocket) {
            std::fprintf(stderr, "error: could not open a loopback listener\n");
            return 1;
        }
        target_port = bound;
        target = "loopback 127.0.0.1:" + std::to_string(bound);
        server = std::thread([&] { serve_loop(listener, stop); });
    }

    std::fputs(ppe::to_text(prov).c_str(), stdout);
    std::printf("target  : %s\n", target.c_str());
    std::printf("tcp     : TCP_NODELAY set (Nagle disabled)\n");
    if (remote == nullptr) {
        std::printf(
            "\n  NOTE: loopback traffic never reaches a wire. This is the kernel\n"
            "  network stack -- protocol processing plus a memcpy -- which bounds\n"
            "  what local IPC over TCP can do, but says nothing about a NIC. Use\n"
            "  --server on one host and --connect on another to measure a link.\n");
    }
    std::printf("\n");

    std::vector<row> rows;

    // -- Message size sweep -----------------------------------------------
    // Small messages are dominated by per-message cost -- syscall, protocol
    // headers, wakeup -- so latency is flat and bandwidth is terrible. The size
    // at which bandwidth stops improving is where that overhead stops mattering.
    std::printf("%-10s %16s %16s\n", "message", "RTT (us)", "stream GB/s");
    for (std::size_t msg = 64; msg <= 1 * kMiB; msg *= 8) {
        const double rtt = pingpong_rtt_us(host, target_port, msg, iters);
        const double bw = stream_gbs(host, target_port, msg, total);

        rows.push_back({"rtt", msg, 1, rtt, "us"});
        rows.push_back({"stream_bandwidth", msg, 1, bw, "GB/s"});
        std::printf("%-10s %16.2f %16.3f\n", human_size(msg).c_str(), rtt, bw);
        std::fflush(stdout);
    }

    // -- Connection concurrency -------------------------------------------
    const std::size_t conn_msg = 64 * 1024;
    std::printf("\nConcurrent connections at %s messages:\n",
                human_size(conn_msg).c_str());
    std::printf("%-10s %16s %14s\n", "conns", "aggregate GB/s", "vs 1 conn");
    double single = 0.0;
    for (unsigned c = 1; c <= max_conns; c *= 2) {
        const double gbs = stream_conns_gbs(host, target_port, conn_msg,
                                            total / (c ? c : 1), c);
        if (c == 1) single = gbs;
        rows.push_back({"stream_bandwidth", conn_msg, c, gbs, "GB/s"});
        std::printf("%-10u %16.3f %13.2fx\n", c, gbs, single > 0.0 ? gbs / single : 0.0);
        std::fflush(stdout);
    }

    if (const char* csv = parse_str(argc, argv, "--csv"); csv != nullptr) {
        if (!write_csv(csv, prov, rows, target)) return 1;
        std::printf("\nwrote %zu rows to %s\n", rows.size(), csv);
    }

    stop_loopback_server(listener, target_port, stop, server);
    return 0;
}
