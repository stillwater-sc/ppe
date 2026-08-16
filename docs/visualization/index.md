# Visualization

> **Status: first view shipped.** `memory_hierarchy --schedule PATH` renders a
> schedule and an occupancy curve as a self-contained HTML page
> (`include/ppe/report/schedule_report.hpp`). Animation over a domain-flow fabric
> is still ahead; what exists draws real recorded spans.

## What gets visualized

**Schedules.** Which operation ran where, and when. For a domain-flow fabric
that means data tokens moving through a PE array; for a CPU it means the
blocking loop nest's traversal of the tile space.

**Resource occupation over time.** The complement of a schedule: at each instant,
what fraction of the fabric, the memory hierarchy level, or the DMA channel is
in use. Idle is the interesting part — a schedule animation that shows full
occupancy throughout is either wrong or describes a problem already solved.

The KPU memory hierarchy (external → L3 tile → L2 banks → L1 buffers →
scratchpad) is the structure most of these views are drawn against, and the
systolic dataflow choice — output-, weight-, or input-stationary — is what makes
one occupancy pattern differ from another.

## Traces feed it

The trace layer now exists: see [Tracing](../tracing/index.md). Spans are
captured with provenance attached and exported as Chrome Trace Event JSON, which
Perfetto already renders as a per-thread timeline. That is not yet a schedule or
occupancy view -- it is a flat span timeline -- but it is the event stream those
views will be drawn from, and it means the visualization work starts from real
recorded data rather than a synthetic example.


Visualization consumes trace event streams; it does not instrument anything
itself. The sibling `kpu` repository already describes an architecture for this
— simulators emit events to a separate collection and interpretation facility,
with a clean separation between functional simulation and
tracing/statistics/interpretation. That separation is worth preserving here:
the thing being measured should not know how it is being displayed.

Every trace carries its platform attributes. An occupancy plot with unlabeled
axes, or a schedule animation whose fabric geometry is implicit, is a picture
rather than a measurement.

## What exists now

Two views of the same spans, because they answer different questions.

**Schedule** — one row per thread, one bar per span, colour by span name. This is
what a trace viewer shows, and it answers *what happened*.

**Occupancy** — the fraction of instrumented lanes inside a span at each instant.
Perfetto has no notion of this, and it answers *what did the machine have left*.
**The idle is the point**: a dense-looking schedule can still leave most of the
machine unused.

On a threaded bandwidth sweep, peak occupancy reaches 56% — nine of sixteen lanes
busy during the eight-thread phase — against a mean of 8%, because most of the
timeline is the single-threaded sweep.

### The denominator is the whole argument

The first run of this reported **6% peak occupancy**. Arithmetically correct and
entirely misleading: fifteen worker threads had registered a thread name and
never opened a span, so they counted as idle lanes forever. Two changes followed
— lanes that recorded nothing are excluded from the denominator and reported
separately as *uninstrumented*, and the bandwidth workers were given a span so
the parallel phase is visible at all.

That is the failure mode this whole view has to guard against: **a picture invites
more trust than a table.** Time in uninstrumented code reads as idle and looks
identical to a machine with nothing to do. The page states its lane count, its
uninstrumented count, and any dropped events, so the denominator is never
implicit.

## Delivery

Web-based, per the repository's documentation and visualization stack. Animations
that a reader can scrub through, published alongside the study that produced
them rather than in a separate artifact — a schedule animation divorced from its
question is decoration.
