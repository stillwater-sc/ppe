# Visualization

> **Status: not started.** Nothing in the tree implements this yet. This page
> records the intent so the trace format is designed with its consumer in mind
> rather than retrofitted to it.

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

Visualization consumes trace event streams; it does not instrument anything
itself. The sibling `kpu` repository already describes an architecture for this
— simulators emit events to a separate collection and interpretation facility,
with a clean separation between functional simulation and
tracing/statistics/interpretation. That separation is worth preserving here:
the thing being measured should not know how it is being displayed.

Every trace carries its platform attributes. An occupancy plot with unlabeled
axes, or a schedule animation whose fabric geometry is implicit, is a picture
rather than a measurement.

## Delivery

Web-based, per the repository's documentation and visualization stack. Animations
that a reader can scrub through, published alongside the study that produced
them rather than in a separate artifact — a schedule animation divorced from its
question is decoration.
