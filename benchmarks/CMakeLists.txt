# PPE benchmarks -- microbenchmarks that characterize a platform.
#
# Flat: a benchmark is a single translation unit measuring one thing. Every
# "*.cpp" here becomes target "bench_<filename_stem>".
#
# CURRENTLY EMPTY. bench_triad lived here until applications/memory_hierarchy
# subsumed it: a single bandwidth number at one working set is the least
# informative point on a curve that the sweep now produces in full. A benchmark
# earns a place here when it measures something a sweep does not -- an
# instrument, not one sample of an experiment. See docs/studies/.
#
# Timed runs happen on dedicated hardware, never in CI -- shared runners cannot
# give pinned cores or a quiet machine, and a perf gate built on them is a flake
# generator. CI compiles every benchmark and smoke-checks --help; the numbers
# are produced locally and published under docs/.

file(GLOB PPE_BENCHMARK_SRC CONFIGURE_DEPENDS "*.cpp")

compile_all("false" "bench" "Benchmarks" "PPE::ppe" ${PPE_BENCHMARK_SRC})

foreach(source ${PPE_BENCHMARK_SRC})
    get_filename_component(stem ${source} NAME_WE)
    ppe_add_smoke_test(bench_${stem})
endforeach()
