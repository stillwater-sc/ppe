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

`include/ppe/platform.hpp` holds the **schema** and stays free of OS headers, so
a consumer that only wants to read an attribute set someone else produced does
not drag a header tree behind it. `include/ppe/detect/cpu.hpp` holds the
**backends**, which pull in sysfs, sysctl, CPUID and `<windows.h>` as needed.

Zero means *not detected*, never *zero* — a study must be able to tell an absent
value from a measured one, and a machine model that silently reports a
zero-byte L3 produces confident nonsense downstream. `sharing_cores == 0` means
the topology was unreadable, which is **not** the same as "private" and is never
reported as 1.

### CPU backend (implemented)

| Platform | Source |
|---|---|
| Linux (all ISAs) | `/sys/devices/system/cpu`, scanned over the affinity mask |
| Linux fallback | CPUID, if sysfs is unavailable (a stripped container) |
| macOS | `sysctl`, `hw.perflevel0.*` on Apple silicon |
| Windows (all ISAs) | `GetLogicalProcessorInformationEx` |

**sysfs is preferred over CPUID on Linux, x86 included**, for three reasons:

- **Determinism.** CPUID describes whichever core the calling thread happens to
  be on, so on a hybrid part the same binary reports a P-core or an E-core
  hierarchy run to run.
- **Sharing.** sysfs publishes `shared_cpu_list` per cache, so a cluster L2
  shared by four cores can be discounted. CPUID's equivalent counts *logical*
  processors and needs threads-per-core to interpret.
- **Affinity.** The scan covers exactly the CPUs this process may run on, so
  under `taskset` detection describes where the work will actually run. This
  matters more here than in the library this was adapted from: PPE's own
  guidance is to pin the process, so detection and measurement must agree about
  which core they are discussing.

Where several caches are candidates, the one with the **smallest per-core
budget** wins, so a model's blocks fit whichever core the work lands on rather
than overflowing the smaller kind.

### Cluster topology (#6)

`ppe::device_attributes` is single-valued by design: `detect/cpu.hpp` keeps, per
level, the entry with the *smallest per-core budget* across the CPUs the process
may run on. That is right for a model that must not overflow whichever core the
work lands on, and wrong for describing a machine. On an i7-12700K it reports
32 KiB L1d / 2 MiB L2 (the E-cluster) unpinned, and 48 KiB / 1.25 MiB pinned to a
P-core. Both correct; neither is the machine.

`include/ppe/detect/topology.hpp` adds the structural view alongside it.
**A cluster is the set of cores sharing one L2 instance** — the definition that
keeps a 4+2 machine from collapsing into "6 cores", and that renders an Alder
Lake as 8 single-core P-clusters plus one 4-core E-cluster. Identical clusters
are collapsed at *render* time with a multiplier, never in the data.

Unlike `detect_cpu()`, topology describes the **whole machine** rather than the
affinity mask: a report that changed under `taskset` would be describing the
scheduler's permissions, not the hardware.

Core capability comes from the best available source, recorded so the role
labels can be traced: `cpu_capacity` (ARM/EAS), then ACPI CPPC `nominal_perf`,
then `cpufreq`. The canonical file is absent on x86 — including the hybrid parts
where the distinction matters most — where CPPC reads 45 on a P-core against 27
on an E-core.

Rendered by `include/ppe/report/topology_report.hpp` as an ASCII tree, a
self-contained HTML page, and JSON, so the page and any future visualization
render from one machine-readable form.

### Measured per cluster

`tool_topology --measure` pins to one CPU of each cluster and runs the shared
probes from `include/ppe/probe/memory.hpp`, so the structural view carries
latency and bandwidth alongside the capacities. On the i7-12700K:

| cluster | L1d | L2 | DRAM |
|---|---|---|---|
| performance | 1.02 ns / 157 GB/s | 3.67 ns / 116 GB/s | 74.9 ns / 29.1 GB/s |
| efficiency | 1.05 ns / 77.9 GB/s | 7.47 ns / 70.8 GB/s | 80.1 ns / 19.2 GB/s |

The E-cluster has *lower absolute* L1 latency than the P-cluster — a 3-cycle L1
at 3.8 GHz beats a 5-cycle L1 at 4.9 — while being roughly half as fast for L1
bandwidth, half as fast at L2, and reaching DRAM more slowly. None of that is
visible in a single-valued machine model, which is the argument for the whole
cluster schema in one table.

Three rules govern this:

**One representative per shape.** Measuring all eight identical P-clusters to
print one line costs eight times as long, and copying one cluster's numbers onto
its siblings would attribute a measurement to hardware that never ran it. The
renderer shows the first of each collapsed run, which is the one measured.

**Measurements live outside `core_cluster`.** That struct is what the machine
claims about itself; a measurement mixed into it would be indistinguishable from
a claim a week later.

**Pinning can fail, and then the report says so.** macOS exposes no
thread-to-processor binding — `THREAD_AFFINITY_POLICY` is a hint and is
unimplemented on Apple silicon — so the probes still run but the result is
marked *not pinned* rather than attributed to a cluster that may not have
produced it.

### The clock, measured

`include/ppe/probe/counters.hpp` counts `PERF_COUNT_HW_CPU_CYCLES` for the
calling thread and divides by wall time, giving a sustained clock rather than a
specification. `ppe::best_clock()` labels the result `measured` or falls back to
the OS claim and says why.

Measured on the i7-12700K, pinned:

| core | measured sustained | claimed max |
|---|---|---|
| P-core (cpu4) | 4.861 GHz | 5.000 GHz |
| E-core (cpu16) | 3.794 GHz | 3.800 GHz |

The P-core does not sustain its advertised ceiling under load; the E-core does.
Every peak model multiplies by this number, so a 2.8% error propagates into
every GOP/s figure — and it is invisible without counting cycles.

Access is gated by `perf_event_paranoid`: 2 or lower suffices, since the counter
sets `exclude_kernel`. A machine that denies counters is a normal machine, and
the fallback says which kind it is.

**Two hybrid-CPU bugs this surfaced**, both of which reported a plausible number:

**The generic PMU counts zero on E-cores.** Alder Lake exposes `cpu_core` and
`cpu_atom`; `PERF_TYPE_HARDWARE` binds to the P-core PMU, so on an E-core the
counter *opens successfully and counts nothing*. No error to check. The correct
encoding puts the PMU in the upper 32 bits of `config`, which was established by
testing all four plausible forms on both core types — the obvious guess
(`type = pmu`) counts zero on both.

**cpufreq was read from `cpu0` regardless of the running core.** Pinned to an
E-core it reported a P-core's ceiling. On this part cpu0 says 4.9 GHz, cpu4 says
5.0, and cpu16 says 3.8 — so it was wrong even between P-cores. It now reads the
current CPU.

### The FMA issue width, measured

`peak.hpp` carried the FMA unit count as its one undetectable factor, defaulting
to 2. `include/ppe/probe/fma.hpp` now measures it.

**Why this is measurable when compute peak was not.** `peak.hpp` records a probe
that tried to measure achievable GOP/s and failed twice — 3.8 million GOP/s with
the loop folded away, then 17.7 GOP/s once operands were opaque, below what real
kernels achieved. That probe measured a *rate*, in operations per second, and
inherited every problem of wall-clock timing. This measures a *ratio* — FMA
instructions per **cycle** — from hardware counters. Frequency cancels out, so no
sustained clock is needed and thermal state is irrelevant.

Measured on the i7-12700K:

| core | FMA/cycle | units | fp64 peak |
|---|---|---|---|
| P-core (Golden Cove) | 1.9997 | 2 | 78.0 GOP/s |
| E-core (Gracemont) | 0.9998 | 1 | 29.5 GOP/s |

Both match the documented microarchitecture. **The default of 2 was wrong for
E-cores**: combined with the `cpu0` clock bug, an E-core roofline reported
78.4 GOP/s where the truth is 29.5 — 2.66x too high.

**The premise had to be checked, and failed first time.** The probe divides a
*known* instruction count by measured cycles, so it is only valid if the compiler
emits the loop that was written. Initialising twelve accumulators to the same
value made all twelve chains provably equal, so the compiler collapsed them into
one — the disassembly held two `vfmadd231pd`, not twelve. The probe then reported
**2.9992 FMA/cycle on a part that issues 2**, having actually measured FMA
latency (0.25/cycle) scaled by a 12x-too-large numerator.

Two changes followed: distinct starting values so the chains cannot be merged,
and a second counter for retired instructions so the probe **detects its own
invalidation** — if retired instructions do not cover the FMAs requested, it
refuses to report rather than dividing by a premise that no longer holds.

### Counters are bound to a PMU, threads are not

A counter is bound to a PMU when it is opened, but an unpinned thread can
migrate. On a hybrid part it can land on a core the *other* PMU owns, where the
counter reads zero — so a measurement would describe a core it never ran on.
`measure_clock_ghz` records the CPU on both sides and refuses when the migration
**crossed PMU domains**; a move between two identical cores leaves the counter
valid and is accepted, since rejecting those would refuse to measure on any
unpinned process.

This surfaced as a test that failed once under load and would not reproduce in 24
attempts. Running unpinned across all 20 CPUs reproduced it every time.

### GPU and KPU

`include/ppe/detect/accelerator.hpp`. Same framing as the CPU: a device is a
hierarchy of levels with capacities, and what differs is what sits at each level
and how many engines share it.

**No SDK at build time.** PPE builds on four platforms with a compiler and CMake
and nothing else, and detecting a GPU must not change that:

- **PCI enumeration** (`/sys/class/drm` on Linux) identifies a GPU with no vendor
  software installed at all — the answer to *is there one, and whose*.
- **The vendor runtime**, if present, is opened at **run** time with `dlopen` /
  `LoadLibrary` and called through hand-declared prototypes. No headers, no link
  dependency. A machine without drivers reports "not detected" rather than
  failing to build. The CUDA driver API is wired this way
  (`cuDeviceGetAttribute` for SM count, clocks, L2, bus width, compute
  capability).

**The KPU is not probed, it is configured.** A KPU exists today as the `kpu-sim`
simulator and as RTL, not as a device on a bus, so its attributes come from a
kpu-sim **system configuration** — which already describes exactly the hierarchy
this repository models: memory banks with bandwidth and latency, L3 tiles, L2
banks, scratchpads, and a systolic compute fabric. Reading that file is the
data-level coupling the plan describes for mtl5: no shared library, no build
dependency, just a format. Supply it with `--kpu-config` or `PPE_KPU_CONFIG`.

Reading `kpu-sim/configs/systems/datacenter_hbm.json`:

```
+-- [KPU] Datacenter AI Cluster Node / kpu_0  (via kpu-config)
|      4 x systolic 32x32 fp32
|      HBM3 bank      32 GiB     across 4   819.0 GB/s  10 ns
|      L3 tile        4 MiB      across 8
|      L2 bank        4 MiB      across 16
|      scratchpad     1 MiB      across 4
```

**Every accelerator figure is claimed**, by a driver or by a configuration file
— none is measured. The report says so, because the CPU rows beside it are the
only ones produced by running code on the hardware.

**Vendor sysfs, no runtime needed.** Intel clocks come from the i915 (`gt_*_freq_mhz`)
or xe (`device/tile0/gt0/freq0/*`) trees — the same silicon reports through either
depending on which driver the kernel bound. AMD reads VRAM from amdgpu sysfs and,
when `amdkfd` is loaded, compute-unit geometry from the KFD topology, matched to
the card by `location_id` rather than by enumeration order — order would attach
the wrong node on a machine with two AMD GPUs, which is exactly the machine where
it matters.

The KFD parser is unit-tested (`tests/kfd.cpp`) because no machine involved in
this project has an AMD GPU. Its two conversions are the kind that are wrong by a
constant factor and look plausible: `simd_count` counts SIMDs rather than compute
units (a CU holds 2 on RDNA, 4 on GCN), and `max_engine_clk_fcompute` is kHz.

Requires a JSON reader; `include/ppe/json.hpp` is a minimal one written for this
rather than a dependency taken on, with `tests/json.cpp` asserting the shapes
kpu-sim configs actually contain and the failure modes.

### Still to build

| Target | Sources |
|---|---|
| Intel execution-unit count | Level Zero — clocks are read from i915/xe sysfs, but no driver publishes the EU count |
| AMD on a machine without amdkfd | VRAM only; compute geometry needs the KFD topology |
| Apple GPU | Metal / IORegistry — needs Objective-C++ |
| KPU hardware | `kpu-hw` interfaces, once a device exists to probe |

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
