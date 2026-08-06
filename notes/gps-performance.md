# GPS performance notes

This document records performance work on the experimental Gaussian Point
Splatting (GPS) renderer. Keep rejected experiments here as well as successful
ones so that they are not repeated without a materially different approach.

## Goals and invariants

- Preserve the GPS point-sampling distribution and sample IDs.
- Retain enough parallel work to hide sampling, storage-buffer, and framebuffer
  atomic latency.
- Do not return to one invocation per Gaussian with an unbounded sample loop.
  Large Gaussians made that approach severely load-imbalanced.
- Keep the shader portable through both `glsl430` and `wgsl` generation.
- Avoid synchronous CPU readbacks in the frame loop.
- Compare experiments with diagnostics disabled, the same deterministic camera
  motion, and otherwise identical renderer settings.

## Current pipeline

The relevant implementation is in `shaders/gps.glsl` and
`renderer_draw_gps_sample()` in `src/renderer.cpp`.

1. **Cull/project** produces projected Gaussian data and visible IDs.
2. **Clear** resets the depth/color output buffers and compact work count.
3. **Count** calculates the number of GPS work records for each Gaussian.
4. **Expand** atomically reserves ranges and writes compact
   `(gaussian_id, sample_id)` records.
5. **Splat** dispatches up to the configured work-list capacity. Each valid
   invocation consumes one compact record, performs the configured
   supersampling loop, and atomically updates the nearest depth.
6. **Resolve** converts the atomic depth/color buffers into render targets.

The splat shader reads the GPU-generated work count directly and rejects
invocations outside the actual count. This avoids a CPU readback stall, but
dispatches enough invocations for the configured capacity.

## Profiling workflow

Use `profile.sh` for repeatable captures. A representative GPS invocation is:

```sh
./profile.sh --render-mode gps --gps-ss 2 \
  --gps-accumulation off --gps-budget-m 8 \
  --seconds 10 --warmup-frames 120 --label gps-8m
```

After the first profiler-enabled build, use `--no-build` for parameter sweeps.
The default `camera_motion=look` supplies deterministic camera movement.

Record at least:

- date, GPU/driver, scene, viewport, and command;
- median and p90 GPU zone times;
- representative-frame measured GPU total;
- whether output appearance or sampling changed;
- a final decision: **keep**, **revert**, or **inconclusive**.

Raw Tracy captures and CSV exports belong under `build/profiles/` and should
remain untracked. The durable findings belong here.

## Baseline observations

Hardware for these measurements: NVIDIA GeForce GTX 1060 6 GB. Configuration:
2x supersampling, accumulation off, deterministic look motion. The work budget
is the configured capacity; it is not a readback of the actual generated work.

| Work budget | Median `gps splat` | Measured GPU total |
| ---: | ---: | ---: |
| 8M | 7.51 ms | 15.39 ms |
| 32M | 8.07 ms | 15.95 ms |
| 250M | 10.04 ms | 17.93 ms |

`gps expand` was approximately 2.6--2.7 ms in this sweep. The increasing splat
time indicates that capacity-sized dispatch has a measurable cost, roughly
2.5 ms between 8M and 250M, but useful splat work still dominates.

## Retained changes

### Do not force GPS diagnostics

**Decision: keep.** Diagnostics now run only when explicitly enabled. Forced
counter collection perturbed profiling and could introduce unnecessary work.
No visible FPS improvement was expected when diagnostics were already cheap or
amortized; the main benefit is that normal measurements no longer include
diagnostic behavior.

### Remove the CPU work-count readback

**Decision: keep.** The splat pass consumes the GPU-generated count directly.
This removes the synchronous CPU/GPU dependency while preserving one compact
record per GPS work item. The current capacity-sized dispatch is the cost of
that choice on Sokol's direct-dispatch API.

### Add per-pass Tracy zones and automated capture

**Decision: keep.** CPU and GPU zones cover GPS clear, count, expand, splat,
and resolve. `profile.sh` automates startup, capture, deterministic camera
movement, shutdown, CSV export, warm-up filtering, and summary statistics. It
also works over SSH with the monitor off on the test machine.

## Rejected experiments

### One invocation per Gaussian

**Decision: reverted before the measurements below.** Each invocation looped
over all work belonging to one Gaussian. Large Gaussians kept a small number of
invocations busy for much longer than the rest, causing severe load imbalance
and a large FPS regression.

Do not retry this exact mapping. Any future per-Gaussian design needs bounded
chunks which can be distributed independently.

### Persistent compact-list consumers

**Date:** 2026-08-05
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

The goal was to dispatch a fixed pool rather than the full capacity while
retaining the compact list and exact sample IDs.

| Splat consumer | Median `gps splat` |
| --- | ---: |
| Existing capacity dispatch | about 7.5 ms |
| Atomic queue, 32 records claimed per invocation | 65.29 ms |
| Grid-stride compact list, 256 workgroups | 25.18 ms |
| Grid-stride compact list, 8192 workgroups | 21.44 ms |

The per-invocation queue batch serialized 32 records and made accesses across
neighboring lanes poorly coalesced. Grid-stride traversal restored coalesced
access within each iteration and removed queue atomics, but remained much
slower even with 8192 workgroups. Persistent invocations appear to reduce the
GPU's ability to hide the long sampling and framebuffer-atomic latency.

A workgroup-tiled queue was also attempted conceptually: lane zero claims a
256-record tile, shared memory broadcasts its base, and all lanes consume one
record. The required dynamic loop and barriers failed WGSL validation because
the compiler could not prove that the shared-count-dependent control flow was
uniform. A GL-only version would not satisfy the project's shader portability
requirement.

Do not retry a persistent compact-list loop solely with another pool size. A
future attempt needs a different synchronization scheme, evidence that it
compiles for WGSL, and a reason it will preserve enough independent warps to
hide atomic latency.

### Hard-coded 2x supersampling loop

**Date:** 2026-08-06
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

The splat shader temporarily replaced its runtime-derived loop count and point
scale with compile-time constants of 4 and 2.0. An immediate control capture
restored the runtime expressions with otherwise identical settings.

| Splat shader | Median `gps splat` | p90 `gps splat` |
| --- | ---: | ---: |
| Hard-coded 2x constants | 15.964 ms | 17.398 ms |
| Runtime supersampling control | 16.057 ms | 17.209 ms |

The 0.6% median difference is within run-to-run variation, while the
specialized p90 was slightly worse. The GLSL compiler appears to optimize the
uniform loop sufficiently; permanent supersampling pipeline variants are not
justified by this result.

## Ranked next experiments

### 1. Reduce compact-list expansion cost

`gps expand` is a substantial, stable pass cost. Investigate bounded chunk
descriptors such as `(gaussian_id, first_sample, sample_count)` rather than one
8-byte record per sample. Chunks must remain small enough to avoid recreating
the large-Gaussian imbalance. The consumer should derive the same sample IDs
from `first_sample + local_index`.

**Potential benefit:** less list memory, fewer expansion writes, and less
bandwidth before splatting.
**Risk:** medium; chunk size affects load balancing, and a looping consumer may
repeat the persistent-thread regression.
**Small experiment:** add a fixed small chunk size and benchmark expansion and
splat separately at 8M and 250M.

### 2. Isolate low-risk splat shader costs

Use one-variable-at-a-time experiments around RNG generation, the `inv_dilog`
polynomial, transcendental operations (`log`, `sqrt`, `sin`, and `cos`), and
the supersampling loop. Preserve random inputs and output distribution unless
an approximation is being evaluated explicitly.

**Potential benefit:** high because this code runs for every generated point.
**Risk:** low for algebraic/hoisting changes, high for numerical
approximations.
**Small experiment:** use shader variants or temporary Tracy comparisons to
measure groups of operations before changing their implementation.

### 3. Revisit dispatch only with API support or new evidence

Indirect dispatch would use the GPU-generated count without CPU synchronization
or capacity over-dispatch, but the Sokol interface used by this project exposes
direct `sg_dispatch` rather than a usable indirect compute dispatch path.
Prefix sums can produce deterministic compact offsets but do not by themselves
solve dispatch sizing.

**Potential benefit:** removes the observed capacity-dependent overhead.
**Risk:** high if it requires backend-specific Sokol changes.
**Revisit when:** Sokol gains suitable indirect dispatch support, or a portable
multi-dispatch/chunk scheme demonstrates a win in an isolated prototype.

## Reference implementation lessons

`pointsplatting/src/core/rendering/passes/gaussian_point_splatting.cu` uses:

- one preprocessing thread per Gaussian;
- an exclusive scan to produce offsets;
- a compact repeated-Gaussian-ID work list;
- one splat thread per compact work item;
- a packed depth/color atomic update;
- a separate per-pixel resolve.

The current GPS path follows the important work-distribution property: splat
parallelism is based on compact work items, not Gaussians. CUDA can launch the
exact compact count more naturally than the current portable Sokol path. The
scan and chunking ideas may still reduce expansion contention or bandwidth,
but should not sacrifice one-record-level load balancing without benchmark
evidence.
