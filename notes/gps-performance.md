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
3. **Expand** calculates each Gaussian's work count and reusable splat
   parameters, atomically reserves a range, and writes compact
   `(gaussian_id, sample_id)` records.
4. **Splat** dispatches up to the configured work-list capacity. Each valid
   invocation consumes one compact record, performs the configured
   supersampling loop, and atomically updates the nearest depth.
5. **Resolve** converts the atomic depth/color buffers into render targets.

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

**Decision: keep.** CPU and GPU zones cover GPS clear, expand, splat, and
resolve. `profile.sh` automates startup, capture, deterministic camera
movement, shutdown, CSV export, warm-up filtering, and summary statistics. It
also works over SSH with the monitor off on the test machine.

### Store compact work records as `uvec2`

**Date:** 2026-08-11
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** keep.

The compact `(gaussian_id, sample_id)` record remains 8 bytes but is represented
as one `uvec2`. Expansion writes both IDs with one vector assignment and
splatting loads them with one vector read. An A/B/A sequence compared two
vector captures around an immediate scalar-struct control.

| Record representation | Median `gps expand` | Median `gps splat` | Combined |
| --- | ---: | ---: | ---: |
| `uvec2`, first capture | 2.933 ms | 16.084 ms | 19.017 ms |
| Two scalar fields, control | 3.900 ms | 16.010 ms | 19.910 ms |
| `uvec2`, confirmation | 2.723 ms | 16.234 ms | 18.957 ms |

The average combined time of the vector captures was 18.987 ms, a 4.6%
reduction from the scalar control. Expansion improved substantially and
repeatably; the small splat variation did not offset that gain. Record size,
sample IDs, RNG inputs, work distribution, and sampling behavior are unchanged.

### Clamp the expansion range before the write loop

**Date:** 2026-08-12
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** keep.

Expansion now returns when an atomically reserved range starts beyond capacity,
then calculates `min(count, capacity - base)` once. Previously every compact
record evaluated `base + i >= capacity` and branched out of the loop. The
written range and compact records are unchanged. An A/B/A sequence compared
the clamped loop around an immediate per-item-check control.

| Expansion bound | Median `gps expand` | p90 `gps expand` |
| --- | ---: | ---: |
| Clamped before loop, first capture | 1.426 ms | 2.252 ms |
| Per-item check, control | 2.777 ms | 6.303 ms |
| Clamped before loop, confirmation | 1.430 ms | 2.191 ms |

The clamped form reduced median expansion time by about 49% and also greatly
reduced its p90. Splat timings varied with the camera workload across captures,
but this change does not modify the generated records or splat pass.

### Fuse point counting into expansion

**Date:** 2026-08-20
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** keep.

Expansion now calculates each Gaussian's work count before reserving and writing
its compact range. The separate count dispatch, pipeline, and 4-byte-per-Gaussian
intermediate buffer were removed. The formulas, randomized fractional rounding,
compact records, and sample IDs are unchanged.

| Work generation | Median generation | Median `gps splat` | Measured GPU total |
| --- | ---: | ---: | ---: |
| Separate count + expand | 1.828 ms | 19.084 ms | 26.665 ms |
| Fused, first capture | 1.440 ms | 19.262 ms | 26.424 ms |
| Fused, confirmation | 1.471 ms | 19.251 ms | 26.486 ms |

The fused captures averaged 1.456 ms for work generation, a 20.4% reduction.
Normal splat variation reduced the end-to-end gain, but measured GPU total
still improved by an average 0.210 ms (0.8%). Fusion also removes one dispatch
and the intermediate count buffer.

### Prepare reusable Gaussian parameters during expansion

**Date:** 2026-08-21
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** keep.

Expansion now writes a 48-byte prepared record per Gaussian that receives work.
It contains the raster mean, Cholesky basis, clamped opacity, sampling
dilogarithm, packed color, depth key, and splat ID. Splatting loads this record
instead of repeatedly reading the 64-byte projected record and recomputing
those values for every compact work item. RNG, inverse-dilog evaluation, sample
IDs, and point placement remain unchanged.

| Prepared parameters | Median `gps expand` | Median `gps splat` | Combined | GPU total |
| --- | ---: | ---: | ---: | ---: |
| No, baseline | 0.960 ms | 7.977 ms | 8.937 ms | 13.585 ms |
| Yes, first capture | 1.130 ms | 7.046 ms | 8.176 ms | 12.886 ms |
| Yes, confirmation | 1.140 ms | 7.062 ms | 8.202 ms | 12.875 ms |

The prepared captures average 8.189 ms for expansion plus splatting, an 8.4%
reduction. Measured GPU total improves by about 0.70 ms (5.2%). The tradeoff is
an additional 48 bytes of GPU storage per Gaussian and approximately 0.18 ms
more expansion work.

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

### Estrin evaluation for `inv_dilog`

**Date:** 2026-08-24
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

The degree-10 inverse-dilog polynomial temporarily used Estrin grouping instead
of Horner evaluation. This shortened the serial dependency chain but required
additional power and combination operations. An immediate control restored the
original Horner form.

| Polynomial evaluation | Median `gps splat` | p90 `gps splat` |
| --- | ---: | ---: |
| Estrin | 17.796 ms | 19.054 ms |
| Horner control | 17.007 ms | 18.093 ms |

Estrin regressed median splat time by 4.6% and also worsened p90. A float32
comparison over one million inputs in [0, 1.645] found a maximum absolute
difference of 1.14e-4 and mean absolute difference of 3.18e-6, so it also lost
bitwise sample equivalence without providing a performance benefit. Keep the
Horner polynomial.

### Tighten the resolve supersampling loops

**Date:** 2026-08-16
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

The resolve shader temporarily looped directly to the uniform supersampling
factor and removed a subpixel bounds check guaranteed by the render-target
dimensions. The control restored the fixed 4x4 loops with runtime breaks and
the bounds check.

| Resolve traversal | Median `gps resolve` | p90 `gps resolve` |
| --- | ---: | ---: |
| Tight loops, first capture | 1.066 ms | 1.078 ms |
| Original loops, control | 1.056 ms | 1.069 ms |
| Tight loops, confirmation | 1.067 ms | 1.077 ms |

The tightened form was consistently about 1% slower. The driver likely handles
the constant-bounded loops better, and the removed checks were not a measurable
cost. Keep the original resolve traversal.

### Reject occluded samples before the depth atomic

**Date:** 2026-08-18
**Decision:** not benchmarked; reverted because it failed shader generation.

The splat shader temporarily read the current depth key and called `atomicMin`
only when the candidate could still win. This is logically safe because depth
keys only decrease during the pass and could avoid contended atomics under
overdraw.

The shader did not compile for the required WGSL target. WGSL represents a
storage value used by `atomicMin` as an atomic type and prohibits an ordinary
load from that value. Replacing the read with an atomic no-op such as
`atomicAdd(value, 0)` would retain the atomic cost and defeat the experiment.
Do not retry this approach without a portable atomic-load facility or a
separate depth representation that does not add greater synchronization cost.

### Store projected splat IDs in the work list

**Date:** 2026-08-07
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

The compact record temporarily stored the projected `splat_id` instead of the
source `gaussian_id`. Expansion read the Gaussian-to-splat mapping once per
Gaussian with work, allowing splatting to remove one indirect mapping read per
work item. Record size and all sample IDs remained unchanged. An immediate
control restored the original mapping.

| Variant | Median `gps expand` | Median `gps splat` | Combined |
| --- | ---: | ---: | ---: |
| Store projected splat ID | 3.906 ms | 16.820 ms | 20.726 ms |
| Store Gaussian ID control | 3.690 ms | 16.892 ms | 20.582 ms |

The splat improvement was only 0.4%, indicating that the mapping read is
probably cache-friendly. Its cost was shifted into expansion, which regressed
by 5.9%; combined time increased by 0.7%. Keeping Gaussian IDs is both faster
overall and consistent with the existing pipeline.

### Precompute the sampling dilogarithm per Gaussian

**Date:** 2026-08-10
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

The point-count record temporarily grew from 4 to 8 bytes and stored the exact
sampling `dilog_alpha`. Counting computed it once per Gaussian and splatting
loaded it through the existing `gaussian_id`, replacing the polynomial and
`log()` evaluation performed per compact work item. An immediate control
restored the 4-byte count and per-work-item calculation.

| Variant | Median `gps count` | Median `gps splat` | Count + splat |
| --- | ---: | ---: | ---: |
| Precomputed per Gaussian | 0.400 ms | 15.991 ms | 16.391 ms |
| Per-work-item control | 0.362 ms | 16.034 ms | 16.396 ms |

Splatting improved by only 0.043 ms while counting regressed by 0.038 ms. The
combined difference was 0.005 ms, well within run-to-run variation. The extra
buffer storage/read offsets the saved arithmetic on this GPU, so the simpler
per-work-item calculation remains preferable.

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
