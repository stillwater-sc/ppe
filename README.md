# ppe

Platform Performance Engineering tools and studies.

Detect what a machine is, model it, measure against the model, trace what ran,
and show the result — across CPU, KPU, and GPU platforms.

> **Status: early.** `memory_hierarchy` is a real measurement — it resolves the
> cache hierarchy on the machine it runs on. The remaining executables are
> placeholders and say so in their own output.

## Quick start

```bash
cmake --preset ci
cmake --build --preset ci
ctest --preset ci

./build-ci/tools/tool_topology                 # what does the OS claim?
./build-ci/applications/app_memory_hierarchy   # what does it actually do?
./build-ci/applications/app_blocking_study     # GEMM blocking sweep
```

For measurements you intend to keep, use the host-tuned `release` preset and pin
the process:

```bash
cmake --preset release && cmake --build --preset release
taskset -c 4 ./build-release/applications/app_memory_hierarchy \
    --max-mib 128 --csv results.csv
```

On an i7-12700K pinned to a P-core, the latency sweep is flat at ~1.0 ns to
48 KiB, steps 3x, is flat at ~3.06 ns to ~1 MiB, steps again, and plateaus near
82 ns — matching `lscpu`'s 48 KiB L1d, 1.25 MiB L2, and 25 MiB L3.

See [docs/getting-started/](docs/getting-started/index.md) for the full build
options, and [docs/architecture/](docs/architecture/index.md) for how the pieces
fit together.

## Layout

| Directory | Contents |
|---|---|
| `include/ppe/` | Shared substrate — platform attributes, version, CLI helpers |
| `applications/` | Performance studies, one subdirectory per study |
| `benchmarks/` | Microbenchmarks, one translation unit per measured thing |
| `tools/` | Shippable command-line tools, one subdirectory per tool |
| `docs/` | All documentation source |
| `docs-site/` | Astro/Starlight site generator — consumes `docs/`, authors nothing |

## License

MIT — see [LICENSE](LICENSE).
