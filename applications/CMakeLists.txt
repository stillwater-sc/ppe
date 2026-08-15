# PPE applications -- performance studies and experiments.
#
# One subdirectory per study, since a study accumulates its own data, plotting
# scripts and write-up alongside the source. Every "<study>/*.cpp" becomes
# target "app_<filename_stem>".
#
# Studies are not run by CTest: a real study is a timed sweep that needs pinned
# cores and a quiet machine. CI asserts only that each one builds and answers
# --help (see cmake/compiler_helpers.cmake).

file(GLOB PPE_APPLICATION_SRC CONFIGURE_DEPENDS "*/*.cpp")

compile_all("false" "app" "Applications" "PPE::ppe" ${PPE_APPLICATION_SRC})

foreach(source ${PPE_APPLICATION_SRC})
    get_filename_component(stem ${source} NAME_WE)
    ppe_add_smoke_test(app_${stem})
endforeach()
