// fileio.hpp -- positioned reads with optional cache bypass.
//
// THE ENTIRE POINT IS BYPASSING THE PAGE CACHE. A storage probe that reads a
// file it just wrote, through the normal buffered path, measures memcpy from
// DRAM and reports it as disk bandwidth. The number is enormous, smooth,
// plausible, and describes nothing about the device -- the same failure mode as
// a compute-peak probe whose loop was folded away.
//
// So the read path here is explicit about which mode it got:
//
//   Linux    O_DIRECT. Requires the buffer address, the byte count and the file
//            offset to be aligned (512 or 4096 depending on the device). FAILS
//            with EINVAL on filesystems that do not support it -- tmpfs among
//            them, which is exactly where a temp file is likely to land -- so
//            the failure is caught and reported rather than fatal.
//   macOS    fcntl(F_NOCACHE). No alignment requirement, and it does not
//            guarantee the data misses the cache, only that this file's pages
//            are not retained.
//   Windows  FILE_FLAG_NO_BUFFERING, with the same alignment requirements as
//            O_DIRECT.
//
// Callers MUST check `direct_io` on the returned handle and report it. A
// buffered fallback is a legitimate configuration to measure in -- it is what
// most applications actually experience -- but a bandwidth figure from it is a
// statement about the page cache, and saying so is the difference between a
// measurement and a fiction.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace ppe {

/// Alignment that satisfies O_DIRECT / FILE_FLAG_NO_BUFFERING on every device
/// this is likely to meet. 4096 covers 512e and 4Kn alike.
inline constexpr std::size_t kDirectAlign = 4096;

struct file_handle {
#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    bool direct_io = false;   ///< true if the cache bypass actually took effect
    std::string mode;         ///< "O_DIRECT", "F_NOCACHE", "NO_BUFFERING", "buffered"
    std::string note;         ///< why the bypass was not used, when it was not

    bool valid() const {
#if defined(_WIN32)
        return h != INVALID_HANDLE_VALUE;
#else
        return fd >= 0;
#endif
    }
};

/// Aligned allocation, required by the direct paths.
inline void* aligned_alloc_bytes(std::size_t bytes) {
#if defined(_WIN32)
    return ::_aligned_malloc(bytes, kDirectAlign);
#else
    void* p = nullptr;
    if (::posix_memalign(&p, kDirectAlign, bytes) != 0) return nullptr;
    return p;
#endif
}

inline void aligned_free_bytes(void* p) {
#if defined(_WIN32)
    ::_aligned_free(p);
#else
    std::free(p);
#endif
}

/// Force a file's contents to the device and wait for it.
///
/// NOT OPTIONAL BEFORE MEASURING. A stream's flush() pushes userspace buffers
/// into the kernel and returns; the pages are still dirty and the writeback is
/// still queued. A read sweep started at that moment competes with the writeback
/// of its own test file for the same device, and reports the contention as the
/// device's read speed. Measured here: 40 MB/s without this call against
/// 205 MB/s with it, on the same file and the same hardware -- a 5x error, in
/// the direction that makes a device look bad.
inline bool sync_file(const std::string& path) {
#if defined(_WIN32)
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = ::FlushFileBuffers(h) != 0;
    ::CloseHandle(h);
    return ok;
#else
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const bool ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
#endif
}

/// Open for reading, trying to bypass the cache. Never fails merely because the
/// bypass is unavailable: it falls back to buffered and records why.
inline file_handle open_for_read(const std::string& path, bool want_direct) {
    file_handle f;

#if defined(_WIN32)
    if (want_direct) {
        f.h = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING,
                            FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f.h != INVALID_HANDLE_VALUE) {
            f.direct_io = true;
            f.mode = "NO_BUFFERING";
            return f;
        }
        f.note = "FILE_FLAG_NO_BUFFERING rejected; using buffered reads";
    }
    f.h = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    f.mode = "buffered";
    return f;

#elif defined(__APPLE__)
    f.fd = ::open(path.c_str(), O_RDONLY);
    if (f.fd >= 0 && want_direct) {
        if (::fcntl(f.fd, F_NOCACHE, 1) == 0) {
            f.direct_io = true;
            f.mode = "F_NOCACHE";
            return f;
        }
        f.note = "F_NOCACHE rejected; using buffered reads";
    }
    f.mode = "buffered";
    return f;

#else  // Linux and other POSIX
    if (want_direct) {
#  if defined(O_DIRECT)
        f.fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
        if (f.fd >= 0) {
            f.direct_io = true;
            f.mode = "O_DIRECT";
            return f;
        }
        // EINVAL here is the common and interesting case: the filesystem does
        // not support O_DIRECT at all. tmpfs is the usual culprit, and a temp
        // file is exactly where this probe is likely to have put its data.
        f.note = "O_DIRECT rejected (filesystem may not support it, e.g. tmpfs);"
                 " using buffered reads";
#  else
        f.note = "O_DIRECT not available on this platform; using buffered reads";
#  endif
    }
    f.fd = ::open(path.c_str(), O_RDONLY);
    f.mode = "buffered";
    return f;
#endif
}

/// Positioned read. Does not use or disturb a shared file offset, so several
/// threads may read one handle concurrently -- which is what a queue-depth
/// sweep needs.
inline std::ptrdiff_t read_at(const file_handle& f, void* buf, std::size_t bytes,
                              std::uint64_t offset) {
#if defined(_WIN32)
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    if (!::ReadFile(f.h, buf, static_cast<DWORD>(bytes), &got, &ov)) {
        // A synchronous handle reports EOF as a failure with ERROR_HANDLE_EOF.
        if (::GetLastError() == ERROR_HANDLE_EOF) return 0;
        return -1;
    }
    return static_cast<std::ptrdiff_t>(got);
#else
    return ::pread(f.fd, buf, bytes, static_cast<off_t>(offset));
#endif
}

inline void close_file(file_handle& f) {
#if defined(_WIN32)
    if (f.h != INVALID_HANDLE_VALUE) { ::CloseHandle(f.h); f.h = INVALID_HANDLE_VALUE; }
#else
    if (f.fd >= 0) { ::close(f.fd); f.fd = -1; }
#endif
}

}  // namespace ppe
