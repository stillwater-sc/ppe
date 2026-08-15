# Plan: the first PPE application

How the `mtl5/ppe` GEMM experiment relates to this repository, and what the
first real application here should be.

## The question

`mtl5/ppe` is a six-step GEMM optimization progression with a timing harness, a
peak model, and six write-ups. It overlaps this repository's subject matter.
Should it move here, be copied here, or stay where it is?

And separately: what is a good *first* application for a repository whose scope
is platform attributes across compute, storage and networking — where linear
algebra is one workload among many?

## Assessment

### `mtl5/ppe` is three different things

| Component | Nature | Belongs |
|---|---|---|
| `kernels.hpp`, `docs/00-05` | Cumulative pedagogy: each step adds exactly one attributable idea | **mtl5, permanently** |
| `peak.hpp` | A machine model — ISA ops/cycle and clock | **ppe**, as detected rather than hardcoded |
| `harness.hpp` timing/statistics/verification | Workload-general measurement discipline | both; ~50 lines |
| `harness.hpp` `gemm_ops`, `measurement` | GEMM-shaped | mtl5 |

### The seam is the machine model, not the study

`peak.hpp` opens with:

```
// Target: Alder Lake P-core, AVX2 + FMA + AVX-VNNI, no AVX-512.
```

and notes that the sustained clock "is a property of the machine, supplied by
the caller rather than guessed here" — it arrives as `--ghz 5.0` on the command
line. `ops_per_cycle<T>()` returns hand-written constants for one
microarchitecture.

That is a hand-maintained machine model with *detect this* written between the
lines, and it is exactly what this repository exists to supply. The seam between
the two repositories is the machine model. It is not the GEMM study.

### `peak.hpp` also records a warning worth respecting

It documents two discarded approaches. The second is the important one: a probe
that *measured* achievable peak reported 3.8 million GOP/s when the compiler
folded the loop away, and after the operands were made opaque reported 17.7
GOP/s for fp64 — below the 37 GOP/s the GEMM kernels actually achieved.

> A probe that is beaten by the thing it is meant to bound is not a ceiling either.

"Just measure the peak" is its own project. Any plan here has to respect that,
which is one reason the first application below measures latency and bandwidth
(honestly measurable) rather than compute peak.

## Decision: invert the relationship, couple through data

**`mtl5/ppe` stays where it is, unchanged.** Its standalone-by-design property —
nothing there includes `<mtl/...>` — is a pedagogical feature. A teaching
progression that acquires an external dependency stops being readable
top-to-bottom, and its expansion to more microarchitectures is mtl5's mission,
not this repository's.

**Nothing of `kernels.hpp` or the write-ups is ported.**

**This repository owns the machine model.** The eventual coupling is at the
*data* level, not the code level: PPE tooling emits a machine profile, and
`mtl5/ppe` optionally consumes it (`--machine profile.json`) with its hardcoded
Alder Lake model as the fallback. No code dependency, so no drift risk in the
pedagogy — and those constants stop needing a hand edit per microarchitecture.

**One duplication is accepted deliberately:** `time_median`, `reps_for`,
`matches`, `fill` — about 50 lines of stable utility code, copied with
attribution. Building shared infrastructure to avoid 50 lines of stable code is
the worse trade. What must *not* be duplicated is the machine model, because
that is the part that goes stale as microarchitectures land.

## The unifying idea

Linear algebra is one workload; storage and networking are out of its reach. The
abstraction that spans all three:

> **A platform is a hierarchy of levels, each characterized by latency,
> bandwidth, and capacity as a function of working-set size and concurrency.**

Registers → L1 → L2 → L3 → DRAM → NVMe → network. The measurement *shape* is the
same at every level: sweep the working set, sweep the concurrency, plot latency
and achieved bandwidth. A cache-size knee and an NVMe queue-depth saturation
point are the same curve at different scales.

That single axis is what makes this a *platform* performance engineering
repository rather than a CPU one, and it gives storage and networking somewhere
to land without a redesign.

## First application: `memory_hierarchy`

Not a linear algebra study. The measured counterpart to attribute detection.

Detection *asks* the OS what the hierarchy is. This *measures* what it behaves
like: pointer-chase latency against working-set size, achieved bandwidth against
working-set size, and both against concurrency.

Why this one first:

1. **It makes the attribute schema real.** Every field in `platform.hpp`
   currently reports "not detected". This forces the schema to carry cache
   sizes, line size, and latency/bandwidth tiers — and to cross-check *claimed*
   against *measured*. "Does this machine tell the truth about itself?" is a
   genuinely useful tool; on VMs and hybrid CPUs the answer is often no.
2. **It is workload-agnostic.** Pointer chasing and streaming. No GEMM.
3. **Everything downstream needs it.** Blocking studies need real cache sizes;
   a roofline needs measured bandwidth. Building it first means later studies
   are parameterized rather than hardcoded.
4. **It establishes the curve** that storage and networking reuse.
5. **It avoids the `peak.hpp` trap.** Latency and bandwidth are honestly
   measurable; compute peak is where naive probes fail.

## Phases

### Phase 0 — measurement substrate

Port the workload-general harness into `include/ppe/`, with attribution. Add a
provenance record — commit, dirty flag, compiler, flags, ISA, build type —
regenerated at *build* time rather than configure time, and emitted with every
result. Retrofit the existing placeholders to prove it works end to end.

A committed result is evidence, and evidence that cannot be traced to the code
and configuration that produced it is worth much less than it looks.

### Phase 1 — `applications/memory_hierarchy/`

Latency and bandwidth against working-set size and concurrency. Emits CSV and
JSON keyed to the attribute schema, with the phase 0 provenance record attached.
Subsumes and replaces the `bench_triad` placeholder.

### Phase 2 — real CPU detection backend

Populate `platform.hpp` from CPUID, `/sys/devices/system/cpu`, `sysctl`, and
`GetLogicalProcessorInformationEx`. The application then reports **claimed vs.
measured** side by side.

Substantial prior art exists in mtl5: `include/mtl/util/cpuid.hpp`,
`cache_info.hpp`, and `system_info.hpp` already do CPUID-based identification
and cache discovery with AArch64 fallbacks. Port and generalize rather than
starting over.

### Phase 3 — peak model and roofline

Port `peak.hpp`'s *role*, not its constants: ops/cycle derived from the detected
ISA, clock from a sustained-clock measurement rather than a command-line flag.
The roofline falls out of phases 1 and 3 — modelled compute peak against
measured bandwidth gives the ridge point.

At this point `mtl5/ppe` can consume a PPE machine profile, and the GEMM
blocking study returns here as a *consumer* of the detected hierarchy — which is
what makes it a platform study rather than a duplicate of mtl5's teaching
material.

### Phase 4 — storage and network levels

The same curve at new levels: storage (queue depth, block size, read/write mix)
and network (message size, in-flight requests). Same schema, same provenance,
same output format.

## What this plan deliberately does not do

- It does not move or copy the GEMM progression.
- It does not build a shared library between the two repositories.
- It does not attempt a measured compute-peak probe before phase 3, and treats
  that as a known-hard problem when it does.
- It does not add a timed measurement to CI at any phase.
