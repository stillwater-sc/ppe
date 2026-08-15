# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Purpose

**ppe** — Platform Performance Engineering tools and studies (Stillwater Supercomputing, Inc., MIT licensed).

Four workstreams that form one pipeline:

**detect the platform → model it → measure against the model → trace execution → visualize the result**

1. **Architecture attribute detection** — performance-relevant properties of a machine across CPU, KPU, and GPU: core/thread topology, cache hierarchy sizes and sharing, NUMA domains, SIMD/ISA extensions, memory bandwidth and latency tiers, accelerator compute units, on-chip memory, interconnect.
2. **Linear algebra blocking studies** — how tiling parameters interact with the detected memory hierarchy.
3. **Schedule and occupancy visualization** — animations of execution schedules and resource occupation over time.
4. **Traces and profilers** — the event streams (2) and (3) consume.

The **attribute schema** (`include/ppe/platform.hpp`) is the contract joining these. Changes there ripple through every downstream stage — treat it as the central design surface, not a utility header.

## Status: scaffolding

The build, CI, presets, and docs pipeline are real and working. The three executables are **placeholders** — they compile, run, and exercise the toolchain end to end, but measure nothing you should act on. Each says so in its own output. Replacing them is the work.

## Build

```bash
cmake --preset ci && cmake --build --preset ci && ctest --preset ci
```

| Preset | Use |
|---|---|
| `dev` | Debug + `compile_commands.json` |
| `release` | **Measurement builds** — `PPE_NATIVE_ARCH=ON` (`-march=native`) |
| `ci` | Release, portable baseline — what CI builds |
| `msvc` | Visual Studio 2022 x64 (hidden by `--list-presets` on non-Windows) |

Each has matching build and test presets. Options: `PPE_BUILD_{APPLICATIONS,BENCHMARKS,TOOLS,TESTS}`, `PPE_NATIVE_ARCH`, `PPE_SANITIZE=address,undefined`, `PPE_CMAKE_TRACE`.

Run a single test: `ctest --preset ci -R smoke_tool_topology`. All tests carry the `smoke` label (`ctest -L smoke`).

Docs site: `cd docs-site && npm ci && npm run build:full` (`build:full` = Doxygen + Astro; plain `build` skips the API reference).

## Layout

| Directory | Contents | Target naming |
|---|---|---|
| `include/ppe/` | Shared substrate: `platform.hpp`, `cli.hpp`, generated `version.hpp` | header-only `PPE::ppe` |
| `applications/` | Studies, one subdirectory each | `<study>/<name>.cpp` → `app_<name>` |
| `benchmarks/` | Microbenchmarks, flat | `<name>.cpp` → `bench_<name>` |
| `tools/` | Shippable CLI tools, one subdirectory each | `<tool>/<name>.cpp` → `tool_<name>` |
| `cmake/` | Build machinery | — |
| `docs/` | All documentation source | — |
| `docs-site/` | Astro/Starlight generator | — |

All three build directories **glob** their sources (`CONFIGURE_DEPENDS`), so a new `.cpp` needs no CMake edit.

**Every executable must answer `--help` and exit 0.** That contract is what the CI smoke tests assert — the only thing CI can meaningfully check about a program whose real job is a timed sweep. Use `ppe::wants_help()` from `ppe/cli.hpp`.

Note the deviation from mtl5: PPE keeps CMake modules in `cmake/`, not `tools/cmake/`, because here `tools/` holds shippable tools.

## Conventions that CI enforces

- **ASCII-only C++ sources.** The `ascii-check` job scans `include/ applications/ benchmarks/ tools/` for multibyte characters and fails. Markdown and CMake files are exempt. Watch for em dashes and typographic quotes in comments.
- **Cross-platform.** Linux x64/ARM64, macOS ARM64, Windows (MSVC + Clang-CL), GCC 13 / Clang 18. ARM64 is not a box-tick: the detection backends diverge hardest between x86 and AArch64.
- **Sanitizers.** An ASan+UBSan lane builds and runs everything — this is low-level systems code reading raw structures from sysfs, CPUID, and vendor runtimes.

## Measurement discipline

These are the rules the placeholders deliberately violate (and announce), and the gap between scaffolding and a result:

- **CI never runs a timed measurement.** Shared runners cannot give pinned cores or a quiet machine; a perf gate built on them measures the neighbours and fails at random. Numbers come from dedicated hardware and are published under `docs/`.
- **Zero means "not detected", never "zero".** A machine model that silently reports a zero-byte L3 produces confident nonsense downstream.
- **A missing device is a normal outcome**, not an error. No GPU, no MSR permission, no KPU simulator — report what was found, leave the rest unset.
- **Flags are intent; predefined macros are effect.** `-march=native` is what you asked for, `__AVX2__` is what you got. Record the effect — `ppe::build_isa()` does this, and every placeholder prints it before its numbers.
- **Host-tuned builds are the point** for real measurements (`release` preset). A baseline-ISA build measures a code generation strategy nobody ships. CI stays portable so results are reproducible across runner microarchitectures.

## Documentation

`docs-site/src/content/docs/` is **100% generated** by `docs-site/sync-content.mjs` and gitignored. **Never author there — write in `docs/`.** New pages must be registered in `FILE_MAP` in `sync-content.mjs` *and* in the sidebar in `astro.config.mjs`; a page in `docs/` that is in neither is invisible to the site.

Published to GitHub Pages at base path `/ppe` on push to `main`.

## KPU and the surrounding workspace

The **KPU** (Knowledge Processing Unit) is Stillwater's domain-flow processor for AI/DSP workloads:

- **Domain flow**: computation as recurrence equations over lattice index spaces; data tokens flow through a PE fabric.
- **PE**: compute unit matching operands by signature via an instruction store (CAM).
- **Systolic array**: pipelined PE grid; output-, weight-, or input-stationary dataflow — the choice drives the occupancy patterns worth visualizing.
- **Memory hierarchy**: external → L3 tile → L2 banks → L1 buffers → scratchpad. What blocking studies tile against.

Sibling repos in `/home/stillwater/dev/stillwater/clones/` (which has workspace-level guidance of its own) — consume these rather than reimplementing them:

| Repo | Relevance |
|---|---|
| `mtl5` | Where this repo's CI, presets, and docs pipeline were ported from; also has a `ppe/` GEMM study that is direct prior art |
| `kpu-sim` | C++20 system simulator — KPU-side attributes and traces |
| `kpu` | Go functional simulator; its event-emission tracing architecture is prior art for the trace workstream |
| `kpu-hw`, `systars` | RTL IP and systolic generator — ground truth for hardware attributes |
| `universal` | Custom arithmetic types (posit, cfloat, fixpnt, lns) for mixed-precision studies |
| `mtl4` / `mtl5` | Dense linear algebra — the subject of blocking studies |

## Workspace conventions

- C++20 + CMake; `main` is the default branch everywhere.
- Git remotes: `git@github.sw:stillwater-sc/` (private) or `git@github.com:stillwater-sc/` (public).
- Rust is planned for parts of the tooling; no crates exist yet (`.gitignore` already covers `target/`).
