# Tracing

The fourth stage of the pipeline: **detect → model → measure → trace →
visualize.** Traces are the event stream that studies annotate themselves with
and that schedule and occupancy views are drawn from.

`include/ppe/trace.hpp` is a header-only span recorder. Any PPE executable can
emit a trace; `memory_hierarchy --trace PATH` is wired up today.

```bash
taskset -c 4 ./build-release/applications/app_memory_hierarchy \
    --max-mib 64 --threads 8 --trace sweep.json
```

The output is [Chrome Trace Event Format](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU),
which Perfetto UI, `chrome://tracing` and speedscope all read. PPE ships no
viewer because it does not need to: open `sweep.json` at
[ui.perfetto.dev](https://ui.perfetto.dev) and the sweep's spans appear on one
lane per thread.

## A tracer that perturbs what it measures is worse than none

The perturbation is invisible in the output, so four properties are structural
rather than nice-to-have. Each costs something.

**No allocation on the hot path.** Buffers are reserved when a thread first
records. An allocation inside a span puts the allocator inside the measurement,
and a page fault inside a microsecond-scale span dominates it entirely.

**No locks on the hot path.** Each thread owns its buffer. The registry mutex is
taken when a thread first registers and again at export, never per event. A
shared buffer would serialize exactly the threads whose concurrency is under
study — the same error as a serialized server in a connection sweep.

**No string copying.** Names are `const char*` with static storage. Copying per
event would allocate; interning would hash. The one way to misuse the API is
passing a pointer to a temporary, so it is spelled out: **names must outlive the
recorder.**

**Bounded, with drops counted and reported.** Capacity is fixed. When a buffer
fills, events are dropped, the drop is counted, and both the export and the
trace file itself say so. A trace with silent gaps produces a schedule animation
with invisible holes — a picture that lies, and the viewer cannot tell.

## Measured overhead

On an i7-12700K P-core, 2M spans, `-O2`:

| | ns per span |
|---|---|
| disabled | 0.2 |
| enabled | 32.3 |

The enabled cost is dominated by two `steady_clock` reads, which is the floor
for any wall-clock span. **Spans should wrap work measured in microseconds, not
nanoseconds**: a span around a 100 ns operation is measuring the tracer.

The disabled figure is the more interesting one. An earlier version read the
clock unconditionally, on the reasoning that a branch was no cheaper than a
timestamp and identical code shapes were worth something. Measured, that cost
**29.6 ns per span with tracing off** — so merely instrumenting a function
perturbed it by ~30 ns whether or not anyone was tracing, which is most of the
way to the overhead the design exists to avoid. Latching the enabled flag at
construction took it to 0.2 ns, which is the compiler deleting the span
entirely.

These numbers live in documentation rather than in a test because they are
properties of the runner; asserting them on shared CI hardware would be a flake
generator. `tests/trace.cpp` asserts the bookkeeping instead — retention, drop
accounting, per-thread lanes, nesting, and that the drop count reaches the file —
which has right answers everywhere.

## Provenance travels inside the trace

The `otherData` block carries commit, dirty flag, compiler, build ISA, device
and timestamp. A trace that cannot be attributed to a build and a machine is a
picture, not evidence — the same rule the result CSVs follow.

## A bug worth recording

The per-thread buffer lookup was first keyed on the recorder's `this` pointer. A
recorder is often a local, and a second one constructed after the first is
destroyed routinely lands on the same address — so the cache matched the dead
recorder and returned a pointer into its freed buffer. A use-after-free, which
presented as the new recorder silently recording nothing, since events landed in
memory nobody read.

`tests/trace.cpp` constructs several recorders in sequence and caught it on the
first run. The fix keys the cache on a never-reused instance id.
