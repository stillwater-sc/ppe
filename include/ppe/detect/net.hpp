// net.hpp -- a minimal portable TCP layer for the network probe.
//
// NAGLE IS THE ADVERSARY, and it is the network's answer to the page cache.
//
// Nagle's algorithm holds a small write until the previous one is acknowledged,
// so it can coalesce them. Combined with the receiver's delayed ACK -- which
// waits up to 40ms hoping to piggyback the acknowledgement on a reply -- a
// small-message ping-pong deadlocks into that timer. The measurement then
// reports tens of milliseconds of "network latency" on a loopback interface
// that can do single-digit microseconds. The number is stable, reproducible and
// completely wrong.
//
// So TCP_NODELAY is set on every socket here, and the probe reports that it did.
// Turning it off is a legitimate thing to measure -- it is what an application
// that does not set it experiences -- but it must be a deliberate choice, not a
// default that silently rewrites the result.
//
// LOOPBACK IS NOT A NIC. Traffic over 127.0.0.1 never reaches a wire: it is a
// memcpy through the kernel's network stack, with the protocol processing but
// none of the physical transit. That is a real and useful level to
// characterize -- it bounds what any local IPC over TCP can achieve -- but it
// is not a measurement of the network hardware, and a report that does not say
// which one it made is unusable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "ws2_32")
#  endif
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace ppe {

#if defined(_WIN32)
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

/// Winsock needs process-wide initialization; POSIX does not. Scoped so the
/// teardown cannot be forgotten.
struct net_context {
    bool ok = false;

    net_context() {
#if defined(_WIN32)
        WSADATA wsa{};
        ok = (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
        ok = true;
#endif
    }
    ~net_context() {
#if defined(_WIN32)
        if (ok) ::WSACleanup();
#endif
    }
    net_context(const net_context&) = delete;
    net_context& operator=(const net_context&) = delete;
};

inline void close_socket(socket_t s) {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

/// Disable Nagle. See the header comment: without this a small-message
/// ping-pong measures a 40ms delayed-ACK timer rather than the network.
inline bool set_nodelay(socket_t s) {
    int one = 1;
#if defined(_WIN32)
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&one), sizeof(one)) == 0;
#else
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
#endif
}

inline bool set_reuseaddr(socket_t s) {
    int one = 1;
#if defined(_WIN32)
    return ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                        reinterpret_cast<const char*>(&one), sizeof(one)) == 0;
#else
    return ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0;
#endif
}

/// Listen on `port`, or on an ephemeral port when `port` is 0. The chosen port
/// is written back through `bound_port`.
inline socket_t listen_on(unsigned short port, const char* bind_addr,
                          unsigned short* bound_port) {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return kInvalidSocket;
    set_reuseaddr(s);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind_addr == nullptr || std::strlen(bind_addr) == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        close_socket(s);
        return kInvalidSocket;
    }

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(s, 64) != 0) {
        close_socket(s);
        return kInvalidSocket;
    }

    if (bound_port != nullptr) {
        sockaddr_in actual{};
#if defined(_WIN32)
        int len = sizeof(actual);
#else
        socklen_t len = sizeof(actual);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&actual), &len) == 0) {
            *bound_port = ntohs(actual.sin_port);
        }
    }
    return s;
}

inline socket_t accept_one(socket_t listener) {
    socket_t s = ::accept(listener, nullptr, nullptr);
    if (s != kInvalidSocket) set_nodelay(s);
    return s;
}

inline socket_t connect_to(const char* host, unsigned short port) {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return kInvalidSocket;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close_socket(s);
        return kInvalidSocket;
    }
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(s);
        return kInvalidSocket;
    }
    set_nodelay(s);
    return s;
}

/// Send exactly `bytes`, looping over short writes.
///
/// A stream socket may accept fewer bytes than offered whenever the send buffer
/// is full, which is the normal case under load. Treating one send() as the
/// whole message is the classic way to build a probe that works on loopback and
/// corrupts its own protocol on a real link.
inline bool send_all(socket_t s, const void* buf, std::size_t bytes) {
    const char* p = static_cast<const char*>(buf);
    std::size_t left = bytes;
    while (left > 0) {
#if defined(_WIN32)
        const int n = ::send(s, p, static_cast<int>(left > 0x7FFFFFFF ? 0x7FFFFFFF : left), 0);
#else
        const auto n = ::send(s, p, left, 0);
#endif
        if (n <= 0) return false;
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

/// Receive exactly `bytes`, looping over short reads.
inline bool recv_all(socket_t s, void* buf, std::size_t bytes) {
    char* p = static_cast<char*>(buf);
    std::size_t left = bytes;
    while (left > 0) {
#if defined(_WIN32)
        const int n = ::recv(s, p, static_cast<int>(left > 0x7FFFFFFF ? 0x7FFFFFFF : left), 0);
#else
        const auto n = ::recv(s, p, left, 0);
#endif
        if (n <= 0) return false;  // 0 = orderly shutdown, which is short here
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Wire protocol
// ---------------------------------------------------------------------------
// A 16-byte header, explicitly little-endian. Byte order is spelled out rather
// than memcpy'd from the struct because the interesting case for a network probe
// is precisely the one where the two ends differ -- an x86 client against an
// AArch64 server is a normal thing to measure, and a host-order header would
// swap the length fields and hang.

enum class net_mode : std::uint8_t { ping_pong = 1, stream = 2 };

inline constexpr std::size_t kHeaderBytes = 16;

inline void put_u32le(unsigned char* p, std::uint32_t v) {
    p[0] = static_cast<unsigned char>(v & 0xff);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xff);
    p[2] = static_cast<unsigned char>((v >> 16) & 0xff);
    p[3] = static_cast<unsigned char>((v >> 24) & 0xff);
}

inline void put_u64le(unsigned char* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xff);
}

inline std::uint32_t get_u32le(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t get_u64le(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

struct net_request {
    net_mode      mode = net_mode::ping_pong;
    std::uint32_t msg_bytes = 0;
    std::uint64_t total_bytes = 0;   ///< stream mode: how much will follow
    std::uint32_t iterations = 0;    ///< ping-pong: how many round trips
};

inline void encode(const net_request& r, unsigned char* out) {
    std::memset(out, 0, kHeaderBytes);
    out[0] = static_cast<std::uint8_t>(r.mode);
    put_u32le(out + 4, r.msg_bytes);
    // total_bytes and iterations share the tail: stream uses the 64-bit count,
    // ping-pong uses the low 32 bits as an iteration count.
    if (r.mode == net_mode::stream) {
        put_u64le(out + 8, r.total_bytes);
    } else {
        put_u64le(out + 8, r.iterations);
    }
}

inline net_request decode(const unsigned char* in) {
    net_request r;
    r.mode = static_cast<net_mode>(in[0]);
    r.msg_bytes = get_u32le(in + 4);
    const std::uint64_t tail = get_u64le(in + 8);
    if (r.mode == net_mode::stream) {
        r.total_bytes = tail;
    } else {
        r.iterations = static_cast<std::uint32_t>(tail);
    }
    return r;
}

/// Serve one connection: echo in ping-pong mode, drain and acknowledge in
/// stream mode. Returns false on a protocol or transport failure.
inline bool serve_connection(socket_t s, std::vector<char>& scratch) {
    unsigned char header[kHeaderBytes];
    if (!recv_all(s, header, kHeaderBytes)) return false;
    const net_request r = decode(header);

    if (r.msg_bytes == 0 || r.msg_bytes > (64u << 20)) return false;
    if (scratch.size() < r.msg_bytes) scratch.resize(r.msg_bytes);

    if (r.mode == net_mode::ping_pong) {
        for (std::uint32_t i = 0; i < r.iterations; ++i) {
            if (!recv_all(s, scratch.data(), r.msg_bytes)) return false;
            if (!send_all(s, scratch.data(), r.msg_bytes)) return false;
        }
        return true;
    }

    std::uint64_t left = r.total_bytes;
    while (left > 0) {
        const std::size_t n =
            static_cast<std::size_t>(left < r.msg_bytes ? left : r.msg_bytes);
        if (!recv_all(s, scratch.data(), n)) return false;
        left -= n;
    }
    // Acknowledge completion so the client times the whole transfer rather than
    // the moment its last write landed in a socket buffer.
    unsigned char ack[8];
    put_u64le(ack, r.total_bytes);
    return send_all(s, ack, sizeof(ack));
}

}  // namespace ppe
