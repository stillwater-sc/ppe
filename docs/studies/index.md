# Studies

A study is an executable that emits a table. It lives in
`applications/<study>/`, alongside whatever data, plotting scripts and write-up
it accumulates.

> **Status: scaffolding.** `app_blocking_study` is a placeholder — a hardcoded
> block-size sweep over a naive ijk GEMM. It shows the shape of a study without
> being one.

## Blocking studies

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

A **benchmark** (`benchmarks/`) measures one thing on one axis — `bench_triad`
measures memory bandwidth. A **study** (`applications/`) asks a question with an
answer, sweeping parameters and producing a table or a plot that supports a
conclusion. Benchmarks are instruments; studies are experiments that use them.
