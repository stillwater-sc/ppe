# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Phase 3** (#4): peak model and roofline. `include/ppe/peak.hpp` derives
  ops/cycle from runtime-detected vector width instead of hardcoding one
  microarchitecture, `include/ppe/detect/isa.hpp` detects SIMD capability
  (including the XGETBV check for OS register-state enablement), and
  `applications/roofline` reports modelled compute peak against measured
  bandwidth to give the ridge point. The FMA-unit count is an input, not a
  detection: no instruction reports it.
- `tests/` — assertions about the model layer, with `test_peak_model`
  verifying the derived model reproduces the hand-written constants it
  generalizes.
- **Phase 2** (#4): real CPU detection backends (`include/ppe/detect/`). Linux
  sysfs scanned over the affinity mask, macOS `sysctl`, and a Windows
  `GetLogicalProcessorInformationEx` backend — the last covering a
  configuration the adapted-from library could not reach at all (Windows
  ARM64). Detects cache sizes, associativity, per-level sharing in physical
  cores, physical core count, NUMA domains, vendor and brand.
  `memory_hierarchy` now reports claimed vs. measured side by side.
- **Phase 1** (#4): `applications/memory_hierarchy` — the first real study.
  Dependent pointer-chase latency and streaming-read bandwidth against
  working-set size, plus a thread-scaling sweep. Emits CSV with provenance
  carried in the file. Resolves the real cache hierarchy: verified against
  `lscpu` on an i7-12700K (48 KiB L1d, 1.25 MiB L2, 25 MiB L3).
- **Phase 0** (#4): measurement substrate (`include/ppe/harness.hpp`) ported
  from `mtl5/ppe` with attribution, and build provenance
  (`cmake/GitInfo.cmake`, `include/ppe/{build_info.hpp.in,provenance.hpp}`)
  regenerated at build time so a recorded commit follows the working tree.

### Removed

- `benchmarks/triad.cpp` — subsumed by `memory_hierarchy`, whose bandwidth
  sweep produces the full curve rather than one sample of it.

### Earlier scaffolding

- Repository scaffolding ported from the sibling `mtl5` project: CMake build
  with presets, GitHub Actions CI, and the Astro/Starlight documentation
  publishing workflow.
- `applications/`, `benchmarks/` and `tools/` directory structure, each globbing
  its sources so new targets need no CMake edit.
- Placeholder targets exercising the build and CI end to end: `tool_topology`
  (report platform attributes), `bench_triad` (memory bandwidth),
  `app_blocking_study` (GEMM blocking sweep). All three are scaffolding and say
  so in their own output.
- `include/ppe/platform.hpp` — the attribute schema the rest of the pipeline
  depends on, with a portable placeholder backend.
