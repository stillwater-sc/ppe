# Architecture

PPE is a pipeline with five stages and one contract.

```
detect  ->  model  ->  measure  ->  trace  ->  visualize
   |          |           |           |           |
platform    machine    studies &   event      schedules &
attributes   model    benchmarks   streams    occupancy
```

The contract is the **attribute schema** — the description of what a machine is.
Every later stage depends on it: a study parameterizes its sweep against the
detected cache hierarchy, a trace is annotated with the device it ran on, and a
visualization labels its axes from the fabric geometry. Change the schema and
every downstream stage moves with it, which is why it is the part of this
repository worth designing rather than accreting.

## Attribute detection

`include/ppe/platform.hpp` holds the interface. Today it reports only what is
portably available from the standard library and the compiler's predefined
macros: hardware concurrency, the compiler, and the ISA baseline the translation
unit was compiled for. Everything else reports "not detected".

That distinction is deliberate. Zero means *not detected*, never *zero* — a
study must be able to tell an absent value from a measured one, and a machine
model that silently reports a zero-byte L3 produces confident nonsense
downstream.

### Per-platform backends

Real detection is not portable code. It is a common interface with a backend per
platform:

| Target | Sources |
|---|---|
| CPU | CPUID; `/sys/devices/system/cpu` on Linux; `sysctl` on macOS; `GetLogicalProcessorInformationEx` on Windows; hwloc for topology and NUMA |
| GPU | CUDA / HIP / Level Zero / Metal runtime queries — SM or CU counts, on-chip memory, clock domains |
| KPU | The `kpu-sim` and `kpu-hw` interfaces — PE fabric geometry, scratchpad and L1/L2/L3 tile capacities |

Two rules keep this tractable:

**A missing device is a normal outcome.** No GPU, no permission to read an MSR,
no KPU simulator on the machine — none of these are errors. Detection reports
what it found and leaves the rest unset. A tool that refuses to run on a laptop
because it cannot find an accelerator is a tool nobody runs on a laptop.

**Flags are intent; predefined macros are effect.** `-march=native` is what you
asked for; `__AVX2__` is what you got. Record the effect. A measurement whose
ISA baseline is unknown is not comparable to any other measurement.

## The ISA baseline and why it is recorded everywhere

Every placeholder prints its compiler and ISA baseline before its numbers. This
is not decoration. The single most common way a performance comparison goes
wrong is comparing two builds that differ in code generation rather than in the
thing under study — and the difference is invisible in the output unless
something puts it there.

## Relationship to the KPU ecosystem

PPE consumes the sibling projects rather than reimplementing them:

- **`kpu-sim`** (C++20 system simulator) — the KPU-side source of attributes and
  traces: memory hierarchy, DMA, systolic arrays.
- **`kpu`** (Go functional simulator) — its event-emission architecture
  (instrumentation → time-series store → visualization) is prior art for the
  trace workstream.
- **`kpu-hw` / `systars`** — RTL IP and the systolic array generator; ground
  truth for hardware attributes.
- **`mtl5` / `mtl4`** — dense linear algebra, the natural subject of the
  blocking studies.
- **`universal`** — custom arithmetic types, for mixed-precision studies.
