# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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
