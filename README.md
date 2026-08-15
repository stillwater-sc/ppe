# ppe

Platform Performance Engineering tools and studies.

Detect what a machine is, model it, measure against the model, trace what ran,
and show the result — across CPU, KPU, and GPU platforms.

> **Status: scaffolding.** The build, CI, and documentation pipeline are
> working. The executables in the tree are placeholders that exercise the
> toolchain end to end; they do not yet measure anything you should act on, and
> each says so in its own output.

## Quick start

```bash
cmake --preset ci
cmake --build --preset ci
ctest --preset ci

./build-ci/tools/tool_topology              # what is this machine?
./build-ci/benchmarks/bench_triad           # memory bandwidth
./build-ci/applications/app_blocking_study  # GEMM blocking sweep
```

For measurements you intend to keep, use the host-tuned `release` preset and pin
the process:

```bash
cmake --preset release && cmake --build --preset release
taskset -c 4 ./build-release/benchmarks/bench_triad --mib 512
```

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
