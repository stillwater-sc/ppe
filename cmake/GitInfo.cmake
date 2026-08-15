# Regenerate build_info.hpp at BUILD time.
#
# Ported from mtl5/cmake/GitInfo.cmake (Stillwater, MIT).
#
# Run in script mode from a custom target, so the recorded commit follows the
# working tree rather than whenever CMake last configured. configure_file leaves
# the file untouched when the rendered content is identical, so this does not
# force rebuilds of anything that includes it.
#
# Expects: SRC_DIR, TEMPLATE, OUTPUT, CXX_FLAGS, CMAKE_TYPE
find_package(Git QUIET)

# Values below are pasted between quotes in a C++ string literal, so anything
# the shell or CMake let through must be escaped or the generated header is
# invalid. A Windows flag alone breaks it twice: `/I C:\foo\bar /D S="q"` gives
# `\f` and `\b` as bogus escapes AND terminates the literal early. Backslashes
# first, or the escaping escapes itself.
function(ppe_escape_c_string out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    # Encode, do not discard. Dropping \r and folding \n to a space would keep
    # the header valid while silently changing the value it records -- wrong
    # provenance rather than missing provenance, which is the failure this whole
    # feature exists to prevent.
    string(REPLACE "\r" "\\r" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

set(PPE_BUILD_GIT_COMMIT "unknown")
set(PPE_BUILD_GIT_DIRTY  "unknown")

if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _sha_rc
        ERROR_QUIET)
    if(_sha_rc EQUAL 0 AND _sha)
        set(PPE_BUILD_GIT_COMMIT "${_sha}")
        # --porcelain is empty exactly when the tree is clean. UNTRACKED FILES
        # COUNT (--untracked-files=normal): applications/, benchmarks/ and
        # tools/ all glob their sources, so an untracked .cpp there is compiled
        # into the build while nothing tracked has changed -- git_dirty=0 would
        # then claim a reproducibility this build does not have. .gitignore
        # still applies, so build trees and editor droppings do not trip it.
        execute_process(
            COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=normal
            WORKING_DIRECTORY "${SRC_DIR}"
            OUTPUT_VARIABLE _status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _status_rc
            ERROR_QUIET)
        if(_status_rc EQUAL 0)
            if(_status STREQUAL "")
                set(PPE_BUILD_GIT_DIRTY "0")
            else()
                set(PPE_BUILD_GIT_DIRTY "1")
            endif()
        endif()
    endif()
endif()

if(NOT CMAKE_TYPE)
    # Multi-config generators (Visual Studio, Ninja Multi-Config) leave
    # CMAKE_BUILD_TYPE empty at configure time; the build-time invocation passes
    # $<CONFIG> instead. Say so rather than recording a blank.
    set(CMAKE_TYPE "unknown-at-configure-time")
endif()

ppe_escape_c_string(PPE_BUILD_CXX_FLAGS  "${CXX_FLAGS}")
ppe_escape_c_string(PPE_BUILD_CMAKE_TYPE "${CMAKE_TYPE}")
configure_file("${TEMPLATE}" "${OUTPUT}" @ONLY)
