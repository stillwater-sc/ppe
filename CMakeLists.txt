cmake_minimum_required(VERSION 3.22)

# -- Version from git tag ------------------------------------------------
# If building from a git tag like v0.2.0, extract the version automatically.
# Falls back to the hardcoded version below when git is unavailable or the
# working directory is not a tagged commit. .github/workflows/release.yml
# greps PPE_FALLBACK_VERSION to cross-check the tag being released.
set(PPE_FALLBACK_VERSION "0.1.0")

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --match "v[0-9]*" --abbrev=0
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE PPE_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_DESCRIBE_RESULT
    )
    if(GIT_DESCRIBE_RESULT EQUAL 0 AND PPE_GIT_TAG MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(PPE_DETECTED_VERSION "${CMAKE_MATCH_1}")
        message(STATUS "PPE version from git tag: ${PPE_DETECTED_VERSION}")
    else()
        set(PPE_DETECTED_VERSION "${PPE_FALLBACK_VERSION}")
        message(STATUS "PPE version (fallback): ${PPE_DETECTED_VERSION}")
    endif()
else()
    set(PPE_DETECTED_VERSION "${PPE_FALLBACK_VERSION}")
    message(STATUS "PPE version (no git): ${PPE_DETECTED_VERSION}")
endif()

project(PPE
    VERSION ${PPE_DETECTED_VERSION}
    LANGUAGES CXX
    DESCRIPTION "Platform Performance Engineering -- tools and studies"
)

# -- CMake infrastructure ------------------------------------------------
# NOTE: PPE keeps its CMake modules in cmake/, not tools/cmake/ as MTL5 does.
# Here tools/ holds shippable PPE command-line tools, so build machinery would
# be a confusing tenant of that directory.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(banners)
include(compiler_helpers)
include(summary)
print_header()

# -- Options -------------------------------------------------------------
option(PPE_BUILD_APPLICATIONS "Build the applications (studies and experiments)" ON)
option(PPE_BUILD_BENCHMARKS   "Build the benchmark suite"                        ON)
option(PPE_BUILD_TOOLS        "Build the command-line tools"                     ON)
option(PPE_BUILD_TESTS        "Register smoke tests for the built executables"   ON)
option(PPE_NATIVE_ARCH        "Tune for the host CPU (-march=native; non-portable binaries)" OFF)
option(PPE_CMAKE_TRACE        "Trace CMake target creation"                      OFF)

# -- Global settings -----------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -- Configure version header --------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ppe/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/ppe/version.hpp"
    @ONLY
)

# -- Build provenance ----------------------------------------------------
# Regenerated at BUILD time, not configure time: a configure-time commit goes
# stale as soon as you commit and rebuild, and a result naming the wrong commit
# is worse than one naming none. The script rewrites the header only when its
# content changes, so nothing rebuilds spuriously. Not part of the ppe
# INTERFACE -- putting a per-commit define on every consumer would invalidate
# their objects on each commit; targets depend on this explicitly instead
# (see compile_all in cmake/compiler_helpers.cmake).
#
# CMAKE_CXX_FLAGS alone is usually EMPTY: the optimisation level lives in
# CMAKE_CXX_FLAGS_<CONFIG> and PPE_NATIVE_ARCH adds -march=native through
# target_compile_options. A record reading `cxx_flags=` on a -O3 -march=native
# build records nothing while looking like it recorded something, so the
# per-config flags are folded in here. The authoritative answer to "which ISA"
# is ppe::build_isa(), taken from the compiler's own predefined macros -- flags
# are the intent, that is the effect.
set(PPE_PROVENANCE_FLAGS "${CMAKE_CXX_FLAGS} $<$<CONFIG:Debug>:${CMAKE_CXX_FLAGS_DEBUG}>$<$<CONFIG:Release>:${CMAKE_CXX_FLAGS_RELEASE}>$<$<CONFIG:RelWithDebInfo>:${CMAKE_CXX_FLAGS_RELWITHDEBINFO}>$<$<CONFIG:MinSizeRel>:${CMAKE_CXX_FLAGS_MINSIZEREL}>")
if(PPE_NATIVE_ARCH)
    set(PPE_PROVENANCE_FLAGS "${PPE_PROVENANCE_FLAGS} [PPE_NATIVE_ARCH=ON]")
endif()

add_custom_target(ppe_build_info ALL
    COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/include/ppe/build_info.hpp.in
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/include/ppe/build_info.hpp
            "-DCXX_FLAGS=${PPE_PROVENANCE_FLAGS}"
            -DCMAKE_TYPE=$<CONFIG>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitInfo.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/include/ppe/build_info.hpp
    COMMENT "Recording build provenance (git commit, cxx flags)"
    VERBATIM)

# Generate it once at configure time too, so a consumer that includes the header
# without depending on the target still compiles. Genex do not expand here, so
# the per-config flags are resolved directly; single-config generators know the
# build type at this point and multi-config ones get the plain flags until the
# build-time regeneration above corrects them.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _prov_cfg)
execute_process(COMMAND ${CMAKE_COMMAND}
    -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/include/ppe/build_info.hpp.in
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/include/ppe/build_info.hpp
    "-DCXX_FLAGS=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_prov_cfg}}"
    -DCMAKE_TYPE=${CMAKE_BUILD_TYPE}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitInfo.cmake)

# -- Header-only INTERFACE library ---------------------------------------
# The shared substrate every application, benchmark and tool links against.
# Platform attribute detection lands here; the studies and visualizations
# consume it rather than each re-deriving the machine model.
add_library(ppe INTERFACE)
add_library(PPE::ppe ALIAS ppe)

target_include_directories(ppe INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(ppe INTERFACE cxx_std_20)

# Host-CPU tuning. Off by default so binaries stay portable and CI results stay
# reproducible across runner microarchitectures; turn it on for real
# measurements on a known machine, where -march=native is usually the point.
# BUILD_INTERFACE only, so it never leaks into installed/exported flags.
if(PPE_NATIVE_ARCH AND NOT MSVC)
    target_compile_options(ppe INTERFACE $<BUILD_INTERFACE:-march=native>)
endif()

# Sanitizers for in-tree builds: pass a comma-separated list, e.g.
# -DPPE_SANITIZE=address,undefined. Not for MSVC (use its own /fsanitize).
set(PPE_SANITIZE "" CACHE STRING "Sanitizers for in-tree builds (e.g. address,undefined)")
if(PPE_SANITIZE AND NOT MSVC)
    target_compile_options(ppe INTERFACE
        $<BUILD_INTERFACE:-fsanitize=${PPE_SANITIZE};-fno-omit-frame-pointer;-g>)
    target_link_options(ppe INTERFACE
        $<BUILD_INTERFACE:-fsanitize=${PPE_SANITIZE}>)
endif()

# -- Subdirectories ------------------------------------------------------
# enable_testing() must be called from the top level for `ctest --test-dir` to
# see tests registered in the subdirectories below.
if(PPE_BUILD_TESTS)
    enable_testing()
endif()

if(PPE_BUILD_APPLICATIONS)
    add_subdirectory(applications)
endif()

if(PPE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(PPE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# -- Install rules -------------------------------------------------------
# Headers only for now. There is no exported CMake package config yet: nothing
# consumes PPE as a library, and an exported target set that no one imports is
# maintenance without a user. Add MTL5-style export rules when that changes.
include(GNUInstallDirs)

install(DIRECTORY include/ppe
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/ppe/version.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ppe
)

# -- Configuration summary -----------------------------------------------
ppe_print_configuration_summary()
