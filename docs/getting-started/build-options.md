# Build options

## Presets

| Preset | Build type | Purpose |
|---|---|---|
| `dev` | Debug | Day-to-day development; `compile_commands.json` for tooling |
| `release` | Release | Measurement builds — host-tuned (`PPE_NATIVE_ARCH=ON`) |
| `ci` | Release | Portable baseline, what CI builds |
| `msvc` | multi-config | Visual Studio 2022, x64 |

Each has a matching build and test preset:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The Unix presets use the Ninja generator. `msvc` uses the Visual Studio
generator, so its build and test presets specify `Release` explicitly — a
multi-config generator does not take the configuration from the configure step.

## Options

| Option | Default | Effect |
|---|---|---|
| `PPE_BUILD_APPLICATIONS` | `ON` | Build the studies under `applications/` |
| `PPE_BUILD_BENCHMARKS` | `ON` | Build the microbenchmarks under `benchmarks/` |
| `PPE_BUILD_TOOLS` | `ON` | Build the command-line tools under `tools/` |
| `PPE_BUILD_TESTS` | `ON` | Register the `--help` smoke tests with CTest |
| `PPE_NATIVE_ARCH` | `OFF` | Tune for the host CPU (`-march=native`) |
| `PPE_SANITIZE` | *(empty)* | Comma-separated sanitizer list, e.g. `address,undefined` |
| `PPE_CMAKE_TRACE` | `OFF` | Log target creation during configure |

### `PPE_NATIVE_ARCH`

Off by default, on in the `release` preset.

Off keeps binaries portable and keeps CI results reproducible: `-march=native`
would pin a build to whichever microarchitecture the runner happened to be, so a
red result could not be reproduced. On is what you want for an actual
measurement — without it the compiler targets a baseline ISA, cannot use FMA or
the wider vector registers, and you end up measuring a code generation strategy
nobody ships.

The flag is applied through `BUILD_INTERFACE` only, so it never leaks into
installed or exported flags.

### `PPE_SANITIZE`

```bash
cmake --preset dev -DPPE_SANITIZE=address,undefined
```

Applies to in-tree targets via `BUILD_INTERFACE`. Not wired for MSVC — use its
own `/fsanitize` separately. Sanitized builds are for correctness work; their
timings are meaningless.

## Version

The project version comes from the git tag (`vX.Y.Z`) when one is reachable, and
falls back to `PPE_FALLBACK_VERSION` in `CMakeLists.txt` otherwise. The release
workflow cross-checks the two and warns on a mismatch rather than failing — the
git tag is authoritative for the build either way.
