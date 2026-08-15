# Getting started

PPE is a C++20 CMake project. Everything builds with the standard preset flow —
no dependencies beyond a compiler and CMake 3.22+.

> **Status: scaffolding.** The applications, benchmarks and tools currently in
> the tree are placeholders. They compile, run, and exercise the build and CI,
> but they do not yet measure anything you should act on. Each one says so in
> its own output.

## Build and run

```bash
cmake --preset ci          # Release, portable baseline
cmake --build --preset ci
ctest --preset ci          # smoke tests: every executable answers --help
```

Then run any of the placeholders:

```bash
./build-ci/tools/tool_topology              # what is this machine?
./build-ci/tools/tool_topology --json       # ... as JSON, for other stages
./build-ci/benchmarks/bench_triad           # memory bandwidth
./build-ci/applications/app_blocking_study  # GEMM blocking sweep
```

For measurements you intend to keep, use the `release` preset and pin the
process:

```bash
cmake --preset release && cmake --build --preset release
taskset -c 4 ./build-release/benchmarks/bench_triad --mib 512
```

`release` turns on `PPE_NATIVE_ARCH` (`-march=native`). That makes the binary
non-portable, which is the point: you are measuring *this* machine, and a
baseline-ISA build measures a machine nobody runs. Pinning matters for the same
reason — on a hybrid CPU an unpinned run can land on an E-core and report a
number that describes different silicon.

## Layout

| Directory | Contents |
|---|---|
| `include/ppe/` | The shared substrate: platform attributes, version, CLI helpers |
| `applications/` | Performance studies — one subdirectory per study |
| `benchmarks/` | Microbenchmarks — one translation unit per measured thing |
| `tools/` | Shippable command-line tools — one subdirectory per tool |
| `cmake/` | Build machinery (banners, target helpers, configuration summary) |
| `docs/` | All documentation source |
| `docs-site/` | Astro/Starlight site generator — consumes `docs/`, authors nothing |

### Adding a target

The three build directories glob their sources, so a new file is picked up on
the next configure — no CMake edit needed:

- `applications/<study>/<name>.cpp` becomes target `app_<name>`
- `benchmarks/<name>.cpp` becomes target `bench_<name>`
- `tools/<tool>/<name>.cpp` becomes target `tool_<name>`

Every executable must answer `--help` and exit 0. That is the contract the CI
smoke tests assert, and it is the only thing CI can meaningfully check about a
program whose real job is a timed measurement sweep.

## What CI does and does not check

CI builds every target on Linux, macOS and Windows across GCC, Clang, Apple
Clang, MSVC and Clang-CL, and runs the `--help` smoke tests.

It never runs a timed measurement. Shared runners cannot provide pinned cores or
a quiet machine, so a performance gate built on them measures the neighbouring
tenants and fails at random. Real numbers are produced on dedicated hardware and
published under `docs/`.
