# Studies

A study is an executable that emits a table. It lives in
`applications/<study>/`, alongside whatever data, plotting scripts and write-up
it accumulates.

## `memory_hierarchy` — the first real study

Detection asks the OS what the cache hierarchy is. This measures what it
behaves like: a dependent pointer chase over a random cycle (one outstanding
miss at a time, so the result is a load-to-use latency), a streaming read
(independent accesses, so the machine runs as many misses in parallel as it
can), and a thread-scaling sweep at a fixed working set.

It resolves a real hierarchy. On an i7-12700K, pinned to a P-core, the latency
curve is flat at ~1.0 ns to 48 KiB, steps 3x, is flat at ~3.06 ns to ~1 MiB,
steps again, and plateaus around 82 ns — against `lscpu`'s 48 KiB L1d per
P-core, 1.25 MiB L2 per P-core, and 25 MiB shared L3.

### Claimed vs measured

Since phase 2 the sweep ends by comparing what the OS claims against what it
measured. A claimed size is consistent with a knee when it falls in
`[below, above)`: the working set at `below` still fit the level, the one at
`above` did not.

Pinned to a P-core on the i7-12700K, all three levels agree:

```
level         claimed   measured
L1d            48 KiB   consistent: step at 48 KiB -> 64 KiB
L2           1.25 MiB   consistent: step at 1 MiB -> 1.5 MiB
L3             25 MiB   consistent: step at 24 MiB -> 32 MiB
```

Run *unpinned* on the same hybrid machine, the tool disagrees with itself — and
says so:

```
L1d            32 KiB   NO step bracketing this size
L2              2 MiB   NO step bracketing this size
L3             25 MiB   consistent: step at 24 MiB -> 32 MiB
```

Both halves are behaving correctly. Detection is affinity-aware and, unpinned,
selects the smallest per-core budget on the machine — the E-cluster's 32 KiB L1d
and 2 MiB L2-shared-by-four. The sweep meanwhile ran mostly on P-cores. L3 is
shared by every core, so it agrees either way. This is the tool answering "does
this machine tell the truth about itself?" with a useful *no*, and naming the
reason.

### Two lessons it taught

**A dependent chase must walk a single cycle, not any permutation.** A random
permutation decomposes into several disjoint cycles, and a chase starting in one
only ever visits that cycle's slots — touching a fraction of the working set
while appearing to sweep all of it, and reporting the latency of a smaller set.
Sattolo's algorithm produces one cycle by construction.

**Per-thread timing intervals cannot measure aggregate bandwidth.** The first
implementation timed each thread's own `t0`→`t1` and multiplied by the thread
count. When threads run *sequentially* — exactly what happens when they are
pinned to fewer cores than there are threads — each records a short interval
overlapping none of the others, and N serialized runs are reported as N
concurrent ones. That version claimed 7.18x scaling for 8 threads on a single
core. The aggregate is now measured on one wall clock spanning all threads,
which cannot fail that way.

The second bug is the more instructive one: it did not look like a bug. It
produced a smooth, plausible, monotonically improving scaling curve. It was
caught by pinning the process to one core and asking whether the answer was
physically possible.

## Blocking studies

> **Status: scaffolding.** `app_blocking_study` is a placeholder — a hardcoded
> block-size sweep over a naive ijk GEMM. It shows the shape of a study without
> being one.

The question: how do tiling parameters interact with the memory hierarchy that
is actually present, rather than the one the algorithm assumed?

This is where the detection layer earns its place. A blocking sweep that
hardcodes 16/32/64/128 is measuring arbitrary numbers; a sweep whose block sizes
are derived from the detected L1d, L2 and L3 sizes is measuring the hypothesis.
Wiring `applications/blocking_study` to `ppe::detect_cpu()` is the first real
step in this workstream.

## What a believable measurement needs

The placeholders deliberately omit all of this, and say so in their output. The
list is the gap between scaffolding and a result:

- **Pinning.** `taskset` or equivalent. On a hybrid CPU an unpinned run can land
  on an E-core and report a number describing different silicon.
- **A host-tuned build.** `-march=native` via the `release` preset. A
  baseline-ISA build cannot use FMA or the wide registers and measures a code
  generation strategy nobody ships.
- **Warm-up.** The first touch of an array faults pages in. Timing that measures
  the allocator.
- **Repetition and statistics.** Best-of-N, or a distribution. A single trial on
  a shared machine measures whatever else was running.
- **A working set chosen against the cache hierarchy.** A bandwidth number is
  meaningless unless you know the working set sits outside the cache that would
  otherwise have served it.
- **Provenance.** Commit, compiler, flags, ISA baseline, machine. A number
  without these cannot be compared to a later number, which makes it worthless
  for exactly the purpose it was collected for.

## Benchmarks vs studies

Both live in this repository and neither is run by CI.

A **benchmark** (`benchmarks/`) measures one thing on one axis. A **study**
(`applications/`) asks a question with an answer, sweeping parameters and
producing a table or a plot that supports a conclusion. Benchmarks are
instruments; studies are experiments that use them.

`benchmarks/` is currently empty: `bench_triad` lived there until
`memory_hierarchy` subsumed it, since a single bandwidth number at one working
set is the least informative point on a curve the sweep now produces in full.
