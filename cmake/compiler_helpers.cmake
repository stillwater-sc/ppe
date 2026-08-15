################################################################################################
# compiler_helpers.cmake
#
# Macros to auto-discover .cpp files and create application/benchmark/tool targets.
# Ported from MTL5's compile_all() pattern (itself from Universal), adapted for PPE.

####
# macro to read all cpp files in a directory and create a target for each cpp file
#
# Parameters:
#   testing    - "true" to register a CTest that runs the binary, any other value to skip
#   prefix     - target name prefix (e.g. "app", "bench", "tool")
#   folder     - IDE folder for MSVC/Xcode (e.g. "Applications", "Tools")
#   link_libs  - semicolon-separated list of libraries to link (e.g. "PPE::ppe")
#   ARGN       - list of source files
#
# Each source file "foo.cpp" produces target "${prefix}_foo"
macro(compile_all testing prefix folder link_libs)
    foreach(source ${ARGN})
        get_filename_component(test ${source} NAME_WE)
        string(REPLACE " " ";" new_source ${source})
        set(test_name ${prefix}_${test})
        add_executable(${test_name} ${new_source})
        target_link_libraries(${test_name} PRIVATE ${link_libs})
        set_target_properties(${test_name} PROPERTIES FOLDER ${folder})
        # Provenance is regenerated per build, so the recorded commit follows
        # the working tree rather than the last configure. Depended on per
        # target rather than carried on the INTERFACE, which would invalidate
        # every consumer's objects on each commit.
        if(TARGET ppe_build_info)
            add_dependencies(${test_name} ppe_build_info)
        endif()
        if(${testing} STREQUAL "true")
            if(PPE_CMAKE_TRACE)
                message(STATUS "testing: ${test_name}")
            endif()
            add_test(NAME ${test_name} COMMAND ${test_name})
        endif()
    endforeach()
endmacro()

####
# Register a smoke test that runs an existing target with --help.
#
# Applications, benchmarks and tools are executables, not test binaries: running
# one for real means a timed measurement sweep, which is not something CI can do
# meaningfully on a shared runner. The smoke test asserts the far weaker but
# still useful property that the binary links, starts, parses arguments and
# exits 0 -- which is what catches a broken target in CI.
#
# Every PPE executable is therefore expected to support --help.
macro(ppe_add_smoke_test target)
    if(PPE_BUILD_TESTS)
        add_test(NAME smoke_${target} COMMAND ${target} --help)
        set_tests_properties(smoke_${target} PROPERTIES LABELS "smoke")
    endif()
endmacro()
