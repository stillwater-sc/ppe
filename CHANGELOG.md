# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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
