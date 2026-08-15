// cli.hpp -- minimal argument helpers shared by PPE executables.
//
// Every PPE executable is expected to answer --help and exit 0: that contract
// is what the CI smoke tests assert (see cmake/compiler_helpers.cmake), since a
// shared runner cannot run a real timed measurement meaningfully.
//
// Deliberately tiny. If PPE grows real option parsing, replace this with a
// proper parser rather than extending it into one.
#pragma once

#include <string_view>

namespace ppe {

// True if `flag` appears anywhere in argv[1..argc).
inline bool has_flag(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i < argc; ++i) {
        if (flag == argv[i]) return true;
    }
    return false;
}

/// Value following `flag`, or nullptr when absent. Deliberately not a parser:
/// see the note above.
inline const char* flag_value(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (flag == argv[i]) return argv[i + 1];
    }
    return nullptr;
}

inline bool wants_help(int argc, char** argv) {
    return has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h");
}

}  // namespace ppe
