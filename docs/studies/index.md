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

### The cache line is the slot spacing, and it must be right

The chase spaces its slots one cache line apart so every hop lands on a line the
previous hop did not fetch. That spacing was hardcoded to 64 bytes until phase 2
made the real value available; it is now taken from
`detect_cpu().cache_line_bytes`, with `--line-bytes` to override and a 64-byte
fallback that announces itself.

This is not a cosmetic fix. Forcing the wrong value on the development machine,
whose lines are 64 bytes, at an 8 MiB working set:

| assumed line | reported latency |
|---|---|
| 64 (correct) | 15.02 ns |
| 32 | 13.12 ns |
| 16 | 12.70 ns |

An undersized line makes consecutive slots share a line, so a fraction of the
hops hit in the line just fetched and the reported latency is a blend of a miss
and a hit — **biased low by 13% at half the true size**, and the knee it places a
level boundary at moves with it. The curve stays smooth and plausible throughout.

Apple silicon uses 128-byte lines, so the old hardcoded 64 was exactly this
error on every M-series machine.

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

## `storage_hierarchy` — the same curve, one level down

Block size and queue depth against bandwidth and latency, on a file. The knees
mark the device's minimum useful transfer and its internal parallelism rather
than cache capacity, but the shape of the investigation is identical.

On the development machine's rotating volume, 256 MiB file, `O_DIRECT`:

```
block              seq GB/s  rand latency us
4 KiB                 0.053          6273.16
16 KiB                0.170          6205.05
64 KiB                0.205          6392.82
1 MiB                 0.205         11002.68
```

Sequential bandwidth climbs to a 205 MB/s plateau from 64 KiB — below that,
per-request overhead dominates, which is the storage analogue of a cache-line
effect. Random latency sits near 6.2 ms and queue depth scales only 2.4x to
depth 16. Rotating storage, correctly characterized: an NVMe device would show
sub-100 us latency and keep scaling into the tens.

### Two ways this probe can lie

**The page cache.** Reading a just-written file through the buffered path
measures memcpy from DRAM and reports it as device bandwidth. The probe requests
a cache bypass, reports which mode it got, and refuses to present a buffered
result as a device characterization. `--no-direct` measures the buffered path
deliberately — a valid thing to measure, and labelled as such.

**Its own writeback.** `flush()` on a stream pushes userspace buffers into the
kernel and returns; the pages are still dirty and the writeback still queued. A
read sweep started at that moment competes with the writeback of its own test
file for the same device. The first version did exactly this and reported
**40 MB/s where `dd` reported 213** — a 5x error that looked like a slow disk
rather than a bug. It was caught by checking the probe against an independent
tool instead of trusting a plausible number, and the fix is an `fsync` before
measuring.

## `network_hierarchy` — the same curve, one level further out

Message size and concurrent connections against round-trip latency and
streaming bandwidth. Runs against an in-process loopback server by default, or
`--server` / `--connect HOST` across two machines.

Loopback on the development machine:

```
message            RTT (us)      stream GB/s
64 B                  14.37            0.023
4 KiB                  8.42            1.258
32 KiB                14.50            5.763
256 KiB               54.91            9.424

conns        aggregate GB/s      vs 1 conn
1                     6.240          1.00x
8                    15.608          2.50x
16                   10.774          1.73x
```

Bandwidth climbs steeply with message size because small messages are dominated
by per-message cost — syscall, headers, wakeup — exactly as small blocks are on
storage. Concurrency scales to 8 and then falls back at 16, which is right:
loopback is CPU-bound, and 16 client threads plus 16 server threads oversubscribe
20 logical processors.

### Three ways this probe can lie

**Nagle.** Nagle's algorithm holds a small write until the previous one is
acknowledged; the receiver's delayed ACK waits up to 40 ms hoping to piggyback
its acknowledgement. A small-message ping-pong deadlocks into that timer and
reports tens of milliseconds of "network latency" on an interface capable of
single-digit microseconds — stable, reproducible, wrong. Every socket sets
`TCP_NODELAY` and the report says so.

**Loopback is not a NIC.** Traffic over 127.0.0.1 never reaches a wire: it is
protocol processing plus a memcpy. That bounds what local IPC over TCP can
achieve, which is worth knowing, but it is not a measurement of network
hardware. The target is printed and recorded in the CSV, because a loopback
figure and a cross-host figure are different levels.

**A serialized server.** The first version served connections one at a time from
the accept loop, so the concurrency sweep measured the server's serialization
rather than the link — 8 connections came out at **0.67x** of one. A sweep whose
independent variable the program itself has pinned to 1 produces a smooth,
plausible curve about nothing. Thread-per-connection is a measurement
requirement here, not a scalability preference.

## Blocking studies

`blocking_study` sweeps GEMM tile sizes against the **detected** cache
hierarchy. A blocked GEMM with square tile `b` holds three tiles live -- an A
panel, a B panel, and the C tile -- so its working set is `3 * b^2 * 8` bytes,
and setting that equal to a cache's capacity gives the largest tile that level
can hold:

```
b = sqrt(S / 24)
```

Each detected level therefore yields a candidate block size, annotated in the
output, alongside a ladder of neighbours -- without neighbours a "winner" is
just the largest of three arbitrary points.

On the i7-12700K, pinned:

```
Detected hierarchy (sysfs):
  L1d  48 KiB     -> block   40  (3 tiles = 37.5 KiB)
  L2   1.25 MiB   -> block  232  (3 tiles = 1.23 MiB)
  L3   25 MiB     -> block 1040  (3 tiles = 24.8 MiB)  shared by 12 cores
```

### The result is a negative one, and it took work to state honestly

At `--size 1024` the answer is stable across runs: **no cache-derived block
reaches the top band.** Block 1024 -- unblocked, one tile -- and block 32 win,
neither of which the model predicts. That is explicable: at n=1024 three
matrices are 24 MiB against a 25 MiB L3, so the whole problem nearly fits the
last level and blocking for L1 or L2 buys little.

Getting to a stable statement required two corrections, both of which are the
same mistake in different clothes.

**A winner picked from noise.** The first version reported the single best
block. Consecutive runs named 256, then 512, then 256 -- the top candidates sat
within ~5% of each other, and the argmax moved with the noise while the
conclusion was restated at full confidence each time. `ppe::time_with_spread`
now returns the observed spread per sample, and every block within the noise
floor of the best is reported as indistinguishable.

**A conclusion drawn from a sweep that cannot discriminate.** With the band in
place, consecutive runs at `--size 512` still reported "consistent with the
model" and "the model does not hold" -- opposite conclusions, because band
membership moved with the noise floor. The fix is not a better tiebreak. When
half or more of the candidates fall inside the band, the study now says the
sweep does not discriminate and names what would fix it: a larger size, pinning,
a fixed clock, a quiet machine. **A sweep in which most candidates are
indistinguishable has not measured a preference, and saying so is the result.**

### Still simplified

One kernel, one thread, square tiles, no packing. A production GEMM uses
rectangular tiles chosen per level, packs panels into contiguous buffers, and
has a register-blocked micro-kernel -- and those choices interact with this one.
`mtl5/ppe` walks that progression in six documented steps.

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
