# PPE tools -- the shippable command-line utilities.
#
# One subdirectory per tool. Every "<tool>/*.cpp" becomes target
# "tool_<filename_stem>"; the installed/invoked name is the target name.
#
# Unlike applications and benchmarks, tools are meant to be run by users and by
# other programs (a trace collector feeding a visualization, for instance), so
# their output formats are interfaces -- change them deliberately.

file(GLOB PPE_TOOL_SRC CONFIGURE_DEPENDS "*/*.cpp")

compile_all("false" "tool" "Tools" "PPE::ppe" ${PPE_TOOL_SRC})

foreach(source ${PPE_TOOL_SRC})
    get_filename_component(stem ${source} NAME_WE)
    ppe_add_smoke_test(tool_${stem})
endforeach()
