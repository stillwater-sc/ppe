#!/usr/bin/env python3
"""Check that platform APIs are backed by the header that declares them.

WHY THIS EXISTS. Three separate commits have shipped a header that used a
platform API without including its header, and each broke only on the platform
this repository is not developed on:

  * topology.hpp used std::set / std::pair with <set> inside an #if __linux__
    branch -- both Windows jobs failed to build
  * accelerator.hpp used HMODULE / LoadLibraryA / GetProcAddress with no
    <windows.h> at all -- both Windows jobs failed to build

A platform-conditional include is only exercised on that platform, so "it builds
here" is structurally incapable of catching this, and reading carefully has now
failed twice. This is the check that does not depend on remembering.

It is deliberately dumb: if a file mentions a symbol, it must include the header
that declares it. That over-triggers on a symbol appearing in a comment, which
is the right direction to be wrong in -- a spurious failure costs a comment
rewrite, a missed one costs a red CI run on a platform nobody can reproduce
locally.
"""

import re
import sys
from pathlib import Path

# symbol -> header that must appear in an #include somewhere in the file
REQUIREMENTS = {
    # Win32
    "HMODULE": "windows.h",
    "LoadLibraryA": "windows.h",
    "GetProcAddress": "windows.h",
    "FreeLibrary": "windows.h",
    "HKEY": "windows.h",
    "RegOpenKeyExA": "windows.h",
    "RegQueryValueExA": "windows.h",
    "KAFFINITY": "windows.h",
    "SetThreadAffinityMask": "windows.h",
    "GetCurrentThread": "windows.h",
    "GetLogicalProcessorInformationEx": "windows.h",
    "CreateFileA": "windows.h",
    "ReadFile": "windows.h",
    "FlushFileBuffers": "windows.h",
    "CloseHandle": "windows.h",
    "INVALID_HANDLE_VALUE": "windows.h",
    "WSAStartup": "winsock2.h",
    "closesocket": "winsock2.h",
    # POSIX
    "dlopen": "dlfcn.h",
    "dlsym": "dlfcn.h",
    "dlclose": "dlfcn.h",
    "pthread_setaffinity_np": "pthread.h",
    "sched_getaffinity": "sched.h",
    "sysctlbyname": "sys/sysctl.h",
    "getauxval": "sys/auxv.h",
    "posix_memalign": "cstdlib",
    "pread": "unistd.h",
    "fcntl": "fcntl.h",
    "inet_pton": "arpa/inet.h",
    "setsockopt": "sys/socket.h",
    # std
    "std::set<": "set",
    "std::pair<": "utility",
    "std::thread": "thread",
    "std::atomic": "atomic",
    "std::mutex": "mutex",
    "std::ifstream": "fstream",
    "std::ofstream": "fstream",
    "std::ostringstream": "sstream",
    "std::filesystem": "filesystem",
    "std::mt19937_64": "random",
    "std::accumulate": "numeric",
    "std::iota": "numeric",
    "std::memcpy": "cstring",
    "std::strtoull": "cstdlib",
}


def includes_of(text):
    return set(re.findall(r'#\s*include\s*[<"]([^>"]+)[>"]', text))


def strip_comments(text):
    """Remove // and /* */ comments.

    Headers here carry long design comments that name platform APIs while
    explaining why they are or are not used -- platform.hpp discusses
    GetLogicalProcessorInformationEx without calling it. Scanning those would
    demand rewording prose to satisfy a linter, which is the wrong trade.
    """
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    return text


def mentions(text, symbol):
    """Whole-symbol match.

    Substring matching reported 'pread' inside 'relative_spread' -- a linter
    that cries wolf gets ignored, which costs more than the bug it was added
    for.
    """
    if symbol.endswith("<"):
        return symbol in text
    return re.search(r'(?<![A-Za-z0-9_])' + re.escape(symbol) + r'(?![A-Za-z0-9_])',
                     text) is not None


PLATFORM_GUARD = re.compile(
    r'#\s*(if|elif)\s+.*(defined\s*\(?\s*(__linux__|_WIN32|__APPLE__|__aarch64__|'
    r'__x86_64__|_M_X64|__GNUC__|__clang__|_MSC_VER)|'
    r'__linux__|_WIN32|__APPLE__|PPE_HAS_X86_CPUID|PPE_FMA_PROBE_X86)')

# Headers that do not exist on every platform. Including one outside a platform
# guard breaks the build on the platforms that lack it -- which, on a project
# developed on Linux, means Windows and macOS find out in CI.
PLATFORM_HEADERS = {
    "sched.h", "unistd.h", "dlfcn.h", "pthread.h", "fcntl.h", "elf.h",
    "windows.h", "winsock2.h", "ws2tcpip.h", "intrin.h", "immintrin.h",
    "cpuid.h", "sys/sysctl.h", "sys/auxv.h", "sys/mman.h", "sys/ioctl.h",
    "sys/socket.h", "sys/syscall.h", "sys/types.h", "netinet/in.h",
    "netinet/tcp.h", "arpa/inet.h", "linux/perf_event.h", "asm/unistd.h",
}


def unguarded_platform_includes(text):
    """Platform-specific headers included outside any platform guard.

    The inverse of the symbol rule: that one catches a symbol used without its
    header, this catches a header included where it does not exist. Both broke
    Windows, and neither is reachable from a Linux build.
    """
    inside = []
    bad = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith('#if'):
            inside.append(bool(PLATFORM_GUARD.match(stripped)))
            continue
        if stripped.startswith('#elif'):
            if inside:
                inside[-1] = inside[-1] or bool(PLATFORM_GUARD.match(stripped))
            continue
        if stripped.startswith('#endif'):
            if inside:
                inside.pop()
            continue
        m = re.match(r'#\s*include\s*[<"]([^>"]+)[>"]', stripped)
        if m and m.group(1) in PLATFORM_HEADERS and not any(inside):
            bad.append(m.group(1))
    return bad


def guarded_symbols(text):
    """Names defined only inside a platform-conditional region.

    Tracks #if/#endif nesting and records `inline ... name(` and `struct name`
    declared while inside a platform guard, minus anything also declared
    outside one. A test that uses such a name compiles on one platform and
    breaks the others -- which is how tests/kfd.cpp, written to verify a parser
    for hardware nobody here owns, broke both Windows and both macOS jobs.
    """
    inside = []          # stack of bools: is this level a platform guard
    guarded, unguarded = set(), set()
    decl = re.compile(r'^\s*(?:inline\s+[\w:<>,\s*&]+?\b(\w+)\s*\(|struct\s+(\w+))')
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith('#if'):
            inside.append(bool(PLATFORM_GUARD.match(stripped)))
        elif stripped.startswith('#elif'):
            if inside:
                inside[-1] = inside[-1] or bool(PLATFORM_GUARD.match(stripped))
        elif stripped.startswith('#endif'):
            if inside:
                inside.pop()
        else:
            m = decl.match(line)
            if m:
                name = m.group(1) or m.group(2)
                (guarded if any(inside) else unguarded).add(name)
    return guarded - unguarded


def check_test_portability(files):
    """Tests must only use symbols available on every platform."""
    guarded = {}
    for path in files:
        if path.suffix != ".hpp":
            continue
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for name in guarded_symbols(text):
            guarded[name] = path

    failures = []
    for path in files:
        if "tests" not in path.parts:
            continue
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for name, origin in sorted(guarded.items()):
            if re.search(r'(?<![A-Za-z0-9_])' + re.escape(name) + r'\s*[\(<]', text):
                failures.append(
                    f"{path}: uses '{name}', which {origin} defines only inside a "
                    f"platform guard")
    return failures


def main():
    roots = [Path("include"), Path("applications"), Path("tools"), Path("tests"),
             Path("benchmarks")]
    files = [p for r in roots if r.is_dir()
             for p in r.rglob("*") if p.suffix in (".hpp", ".cpp", ".h")]

    failures = []
    for path in sorted(files):
        raw = path.read_text(encoding="utf-8", errors="replace")
        have = includes_of(raw)
        text = strip_comments(raw)
        for symbol, header in REQUIREMENTS.items():
            if not mentions(text, symbol):
                continue
            if header in have:
                continue
            # <windows.h> also comes in via winsock2.h, which includes it.
            if header == "windows.h" and "winsock2.h" in have:
                continue
            failures.append(f"{path}: uses '{symbol}' but does not include <{header}>")

    for path in sorted(files):
        raw = path.read_text(encoding="utf-8", errors="replace")
        for header in unguarded_platform_includes(strip_comments(raw)):
            failures.append(
                f"{path}: includes <{header}> outside a platform guard; it does not "
                f"exist everywhere")

    failures += check_test_portability(files)

    if failures:
        print("Platform include check FAILED:")
        for f in failures:
            print("  " + f)
        print("\nA platform-conditional include is only exercised on that platform.")
        print("Include the header unconditionally, or inside a branch that covers")
        print("every platform using the symbol.")
        return 1

    print(f"Platform include check OK ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
