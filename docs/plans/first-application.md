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
ISA. The roofline falls out of phases 1 and 3 — modelled compute peak against
measured bandwidth gives the ridge point.

**Resolved: the clock is measured where the kernel permits it.**
`include/ppe/probe/counters.hpp` opens `PERF_COUNT_HW_CPU_CYCLES` for the
calling thread and divides counted cycles by wall time, giving a sustained
figure rather than a specification. `ppe::best_clock()` returns it labelled
`measured`, or the OS claim labelled as such with the reason measurement was
unavailable. Access is gated by `perf_event_paranoid`: 2 or lower suffices,
because the counter sets `exclude_kernel`; the development machine reports 4 and
therefore exercises the fallback. A machine that denies counters is a normal
machine, and the report says which kind it is rather than presenting either as
the other.

**Original amendment, retained for the reasoning:** This phase originally
called for "a sustained-clock measurement rather than a command-line flag".
Reaching that honestly needs a performance counter (privileged, and with no
portable equivalent), or the timestamp counter (invariant since Nehalem — it
ticks at a fixed reference rate regardless of the core clock, which is exactly
what makes it useless here), or a dependent instruction chain of assumed
latency, which merely relocates the guess. `ppe/detect/clock.hpp` therefore
reports what the OS claims, labelled as a claim, with `--ghz` to override. A
`perf_event` backend on Linux, degrading to the claim where permissions do not
allow it, would close this properly and is worth doing.

**What is derivable, and what is not.** The model's three factors have different
status, and conflating them is how a peak model starts lying:

| Factor | Status |
|---|---|
| lanes | derived from detected vector width |
| ops per FMA | derived: 2 with FMA, 1 without |
| FMA units per core | **not derivable** — no instruction reports it; an input, default 2 |
| clock | claimed by the OS, or supplied |

`tests/peak_model.cpp` asserts the derived model reproduces the hand-written
constants on the target they were written for. It exists because the first
version failed it.

At this point `mtl5/ppe` can consume a PPE machine profile, and the GEMM
blocking study returns here as a *consumer* of the detected hierarchy — which is
what makes it a platform study rather than a duplicate of mtl5's teaching
material.

### Phase 4 — storage and network levels

The same curve at new levels. Same schema, same provenance, same output format.

**Storage: done.** `applications/storage_hierarchy` sweeps block size and queue
depth against bandwidth and latency. The knees mean something different from the
DRAM ones — the device's minimum useful transfer, below which per-request
overhead dominates, and its internal parallelism, above which more in-flight
requests stop helping — but they are the same kind of finding.

Two hazards it has to defeat, both of which produce large, smooth, plausible
numbers describing nothing:

- **The page cache.** Reading a just-written file through the buffered path
  measures memcpy from DRAM. The probe requests `O_DIRECT` / `F_NOCACHE` /
  `FILE_FLAG_NO_BUFFERING`, reports which mode it actually got, and refuses to
  present a buffered result as a device characterization.
- **Its own writeback.** A stream's `flush()` pushes userspace buffers into the
  kernel and returns; the pages are still dirty and the writeback still queued.
  A read sweep started then competes with the writeback of its own test file.
  Measured on the development machine: **40 MB/s without `fsync` against
  205 MB/s with it** — a 5x error, in the direction that makes a device look bad.

**Network: done.** `applications/network_hierarchy` sweeps message size and
concurrent connections against round-trip latency and streaming bandwidth,
against an in-process loopback server by default or `--server` / `--connect`
across two hosts.

Three hazards, all producing plausible numbers about the wrong thing:

- **Nagle.** Combined with the receiver's delayed ACK, a small-message ping-pong
  deadlocks into a 40 ms timer and reports it as network latency. `TCP_NODELAY`
  is set everywhere and the report says so.
- **Loopback is not a NIC.** It is protocol processing plus a memcpy, never a
  wire. A useful bound on local IPC, not a hardware measurement; the target is
  printed and recorded in the CSV.
- **A serialized server.** The first version served connections one at a time,
  so the concurrency sweep measured the server rather than the link: 8
  connections at 0.67x of one. Thread-per-connection is a measurement
  requirement, not a scalability preference.

## What this plan deliberately does not do

- It does not move or copy the GEMM progression.
- It does not build a shared library between the two repositories.
- It does not attempt a measured compute-peak probe before phase 3, and treats
  that as a known-hard problem when it does.
- It does not add a timed measurement to CI at any phase.
