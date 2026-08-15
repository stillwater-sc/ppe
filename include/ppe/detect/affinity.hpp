// affinity.hpp -- pin the calling thread to one logical processor.
//
// Needed to measure a CLUSTER rather than a machine. On a heterogeneous part an
// unpinned probe reports whichever cluster the scheduler happened to pick, and
// may migrate mid-measurement and report a blend of two -- which is the failure
// mode documented at length in applications/memory_hierarchy.
//
// MACOS CANNOT DO THIS, and that is not a gap in this file. Darwin exposes
// thread_policy_set(THREAD_AFFINITY_POLICY), which is documented as a hint
// about cache affinity between threads rather than a binding to a processor,
// and on Apple silicon it is not implemented at all: the kernel alone decides
// which cluster a thread runs on. So `pin_current_thread` reports failure there
// with a reason, and callers must degrade to "measured, cluster unknown" rather
// than silently attributing a number to a cluster it may not describe.
#pragma once

#include <string>

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#elif defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace ppe {

struct pin_result {
    bool ok = false;
    std::string note;   ///< why not, when !ok
};

/// Pin the calling thread to logical processor `cpu`.
inline pin_result pin_current_thread(int cpu) {
    pin_result r;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    if (::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0) {
        r.ok = true;
    } else {
        r.note = "pthread_setaffinity_np failed (cgroup or container policy?)";
    }
#elif defined(_WIN32)
    // Single processor group only. A machine with more than 64 logical
    // processors splits into groups and needs SetThreadGroupAffinity; saying so
    // is better than pinning to the wrong processor in another group.
    if (cpu >= 64) {
        r.note = "cpu index beyond processor group 0; needs SetThreadGroupAffinity";
    } else if (::SetThreadAffinityMask(::GetCurrentThread(),
                                       static_cast<DWORD_PTR>(1ull) << cpu) != 0) {
        r.ok = true;
    } else {
        r.note = "SetThreadAffinityMask failed";
    }
#elif defined(__APPLE__)
    (void)cpu;
    r.note = "macOS does not support pinning a thread to a processor; "
             "THREAD_AFFINITY_POLICY is a hint and is unimplemented on Apple silicon";
#else
    (void)cpu;
    r.note = "no affinity interface on this platform";
#endif
    return r;
}

/// True when this platform can pin at all -- lets a caller decide whether a
/// per-cluster measurement is meaningful before spending time on it.
inline bool affinity_supported() {
#if defined(__linux__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

}  // namespace ppe
