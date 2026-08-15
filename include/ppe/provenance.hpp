// provenance.hpp -- the record that makes a measurement evidence.
//
// A number without the build and machine that produced it cannot be compared to
// a later number, which makes it worthless for exactly the purpose it was
// collected for. Every PPE executable emits this record alongside its results,
// in whichever format the consumer wants.
//
// The fields divide into three groups, and the distinction matters:
//
//   build   -- commit, dirty flag, flags, build type. What code this is.
//   effect  -- compiler and ISA baseline, from the compiler's own predefined
//              macros. What the compiler actually did, which is not always what
//              the flags asked for.
//   machine -- what ppe::detect_cpu() found. Mostly "not detected" until the
//              phase 2 backends land; the fields are here so the schema is
//              stable before it is full.
#pragma once

#include <ppe/build_info.hpp>
#include <ppe/detect/cpu.hpp>
#include <ppe/platform.hpp>
#include <ppe/version.hpp>

#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>

namespace ppe {

struct provenance {
    // Build
    std::string ppe_version;
    std::string git_commit;
    std::string git_dirty;
    std::string cxx_flags;
    std::string cmake_type;

    // Effect (compiler predefined macros, not flags)
    std::string compiler;
    std::string isa;

    // Machine
    device_attributes cpu;

    // Run
    std::string utc_timestamp;
};

namespace detail {

/// ISO-8601 UTC timestamp. Portable across MSVC (gmtime_s) and POSIX
/// (gmtime_r); the thread-unsafe std::gmtime is avoided rather than papered
/// over, since a harness may well be collecting from several threads.
inline std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_MSC_VER)
    if (gmtime_s(&tm, &t) != 0) return "unknown";
#else
    if (gmtime_r(&t, &tm) == nullptr) return "unknown";
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        return "unknown";
    }
    return std::string(buf);
}

/// Escape for a JSON string literal. build_cxx_flags in particular can carry
/// quotes and backslashes straight from a Windows command line -- it was
/// escaped once to survive the C++ literal, and needs escaping again to survive
/// the JSON one.
inline std::string json_escape(const std::string& s) {
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

/// Gather the provenance record for this binary on this machine, now.
inline provenance collect_provenance() {
    provenance p;
    p.ppe_version   = version_string;
    p.git_commit    = build_git_commit;
    p.git_dirty     = build_git_dirty;
    p.cxx_flags     = build_cxx_flags;
    p.cmake_type    = build_cmake_type;
    p.compiler      = build_compiler();
    p.isa           = build_isa();
    p.cpu           = detect_cpu();
    p.utc_timestamp = detail::utc_now();
    return p;
}

/// Human-readable block, for the head of a console run.
inline std::string to_text(const provenance& p) {
    std::ostringstream o;
    o << "PPE " << p.ppe_version << "  commit " << p.git_commit
      << (p.git_dirty == "1" ? " (DIRTY)" : p.git_dirty == "unknown" ? " (dirty unknown)" : "")
      << "  " << p.utc_timestamp << "\n"
      << "build   : " << p.compiler << ", " << p.cmake_type
      << ", ISA baseline " << p.isa << "\n"
      << "flags   : " << (p.cxx_flags.empty() ? "(none recorded)" : p.cxx_flags) << "\n"
      << "device  : " << p.cpu.name << ", "
      << p.cpu.logical_processors << " logical processors\n";
    return o.str();
}

/// Comment block for the head of a CSV, so a committed result file carries its
/// own provenance rather than relying on a sidecar that can be separated from it.
inline std::string to_csv_comment(const provenance& p) {
    std::ostringstream o;
    o << "# ppe_version=" << p.ppe_version << "\n"
      << "# git_commit=" << p.git_commit << "\n"
      << "# git_dirty=" << p.git_dirty << "\n"
      << "# utc=" << p.utc_timestamp << "\n"
      << "# compiler=" << p.compiler << "\n"
      << "# build_isa=" << p.isa << "\n"
      << "# cmake_type=" << p.cmake_type << "\n"
      << "# cxx_flags=" << p.cxx_flags << "\n"
      << "# device=" << p.cpu.name << "\n"
      << "# detect_source=" << p.cpu.source << "\n"
      << "# logical_processors=" << p.cpu.logical_processors << "\n"
      << "# physical_cores=" << p.cpu.physical_cores << "\n"
      << "# numa_domains=" << p.cpu.numa_domains << "\n"
      << "# l1d_bytes=" << p.cpu.l1d_bytes << "\n"
      << "# l2_bytes=" << p.cpu.l2_bytes << "\n"
      << "# l3_bytes=" << p.cpu.l3_bytes << "\n"
      << "# cache_line_bytes=" << p.cpu.cache_line_bytes << "\n";
    return o.str();
}

/// JSON object, for the machine-readable result stream.
inline std::string to_json(const provenance& p) {
    using detail::json_escape;
    std::ostringstream o;
    o << "{\n"
      << "  \"ppe_version\": \"" << json_escape(p.ppe_version) << "\",\n"
      << "  \"utc\": \"" << json_escape(p.utc_timestamp) << "\",\n"
      << "  \"build\": {\n"
      << "    \"git_commit\": \"" << json_escape(p.git_commit) << "\",\n"
      << "    \"git_dirty\": \"" << json_escape(p.git_dirty) << "\",\n"
      << "    \"cmake_type\": \"" << json_escape(p.cmake_type) << "\",\n"
      << "    \"cxx_flags\": \"" << json_escape(p.cxx_flags) << "\",\n"
      << "    \"compiler\": \"" << json_escape(p.compiler) << "\",\n"
      << "    \"isa\": \"" << json_escape(p.isa) << "\"\n"
      << "  },\n"
      << "  \"device\": {\n"
      << "    \"kind\": \"" << to_string(p.cpu.kind) << "\",\n"
      << "    \"name\": \"" << json_escape(p.cpu.name) << "\",\n"
      << "    \"vendor\": \"" << json_escape(p.cpu.vendor) << "\",\n"
      << "    \"source\": \"" << json_escape(p.cpu.source) << "\",\n"
      << "    \"logical_processors\": " << p.cpu.logical_processors << ",\n"
      << "    \"physical_cores\": " << p.cpu.physical_cores << ",\n"
      << "    \"numa_domains\": " << p.cpu.numa_domains << ",\n"
      << "    \"l1d_bytes\": " << p.cpu.l1d_bytes << ",\n"
      << "    \"l1d_assoc\": " << p.cpu.l1d_assoc << ",\n"
      << "    \"l1d_sharing_cores\": " << p.cpu.l1d_sharing_cores << ",\n"
      << "    \"l2_bytes\": " << p.cpu.l2_bytes << ",\n"
      << "    \"l2_sharing_cores\": " << p.cpu.l2_sharing_cores << ",\n"
      << "    \"l3_bytes\": " << p.cpu.l3_bytes << ",\n"
      << "    \"l3_sharing_cores\": " << p.cpu.l3_sharing_cores << ",\n"
      << "    \"cache_line_bytes\": " << p.cpu.cache_line_bytes << "\n"
      << "  }\n"
      << "}\n";
    return o.str();
}

}  // namespace ppe
