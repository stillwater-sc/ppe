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
