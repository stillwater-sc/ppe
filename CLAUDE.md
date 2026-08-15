# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Purpose

**ppe** — Platform Performance Engineering tools and studies (Stillwater Supercomputing, Inc., MIT licensed).

The repository covers four related workstreams:

1. **Architecture attribute detection** — discover and report the performance-relevant properties of a machine across CPU, KPU, and GPU: core/thread topology, cache hierarchy sizes and sharing, NUMA domains, SIMD/ISA extensions, memory bandwidth and latency tiers, accelerator compute units, on-chip memory capacities, and interconnect.
2. **Linear algebra blocking studies** — measure how tiling/blocking parameters interact with the detected memory hierarchy; the detection layer supplies the machine model that the blocking studies parameterize against.
3. **Schedule and occupancy visualization** — animations of execution schedules and resource occupation over time (PE fabric utilization, memory-hierarchy residency, DMA/streamer activity).
4. **Traces and profilers** — capture the event streams that (2) and (3) consume.

These fit together as one pipeline: **detect the platform → model it → measure against the model → trace execution → visualize the result.** Changes to the attribute schema ripple through the study, trace, and visualization layers, so treat that schema as the repository's central contract.

## Status: pre-code

The repository currently contains only `README.md`, `LICENSE`, `.gitignore`, and this file (initial commit `f14d559`). There is no source, build system, or test suite yet — so there are no build/lint/test commands to document. Everything under "Planned toolchain" below is intent, not established fact; verify against actual build files before relying on it.

**Re-run `/init` once the first real code lands** to replace the planned sections with observed ones.

## Planned toolchain

- **C++** for low-level system programming (attribute detection, profilers, trace capture). The `.gitignore` is a CMake/vcpkg/CTest template, and the workspace convention (see below) is **C++20 + CMake**, so follow that unless the code says otherwise.
- **Rust** for tooling where memory safety and ergonomics matter more than ABI reach. `.gitignore` covers `target/`; `Cargo.lock` is left commented there — uncomment it only if this repo ends up publishing library crates rather than binaries.
- **Web** for documentation and visualizations (animations of schedules and occupancy, interactive study results).

Attribute detection is inherently platform-specific — CPUID/`sysfs`/`hwloc` on Linux, vendor runtime queries for GPUs, and the KPU simulator/hardware interfaces for KPU. Expect per-platform backends behind a common interface rather than portable code, and expect graceful degradation when a device or privilege level is absent: a missing accelerator is a normal outcome, not an error.

## KPU and the surrounding workspace

The **KPU** (Knowledge Processing Unit) is Stillwater's domain-flow processor architecture for AI/DSP workloads. Relevant concepts when modeling it as a platform target:

- **Domain flow**: computation expressed as recurrence equations over lattice index spaces, with data tokens flowing through a PE fabric.
- **Processing Element (PE)**: compute unit that matches operands by signature via an instruction store (CAM).
- **Systolic array**: pipelined PE grid; dataflows are output-, weight-, or input-stationary — the choice drives occupancy patterns worth visualizing.
- **Memory hierarchy**: external memory → L3 tile → L2 banks → L1 buffers → scratchpad. This is the structure blocking studies tile against.

This clone sits in the Stillwater multi-project workspace at `/home/stillwater/dev/stillwater/clones/`, which has its own workspace-level guidance. Repos most likely to matter here:

| Repo | Why it's relevant |
|------|-------------------|
| `kpu-sim` | C++20 system simulator (memory hierarchy, DMA, systolic arrays) — the KPU-side source of attributes and traces |
| `kpu` | Go functional simulator; its README describes an event-emission/tracing architecture (StatsD-style instrumentation → time-series store → visualization) directly relevant to the trace workstream |
| `kpu-hw`, `systars` | RTL IP and systolic-array generator — ground truth for hardware attributes |
| `universal` | Header-only custom arithmetic types (posit, cfloat, fixpnt, lns) for mixed-precision studies |
| `mtl4` / `mtl5` | Matrix Template Library — dense linear algebra, the natural subject of blocking studies |

Prefer consuming these as dependencies or data sources over reimplementing their models here.

## Workspace conventions

- C++ projects use **C++20** and **CMake**; `main` is the default branch everywhere.
- Git remotes: `git@github.sw:stillwater-sc/` (private) or `git@github.com:stillwater-sc/` (public).
