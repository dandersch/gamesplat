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

### Prepare the frame-dependent RNG seed per Gaussian

**Date:** 2026-08-30
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

The two-component RNG base seed temporarily replaced the splat ID in the
existing prepared record. Expansion calculated the exact frame-dependent seed
once per Gaussian, removing three integer multiplies and two additions per
compact work item without changing the record size or generated samples. An
immediate control restored per-work-item seed construction.

| Seed construction | Median `gps expand` | Median `gps splat` | Combined |
| --- | ---: | ---: | ---: |
| Prepared per Gaussian | 1.661 ms | 17.172 ms | 18.833 ms |
| Per-work-item control | 1.592 ms | 17.263 ms | 18.855 ms |

The 0.091 ms splat reduction was almost entirely shifted into expansion, and
the 0.022 ms combined difference is within noise. Summing all median GPU zones
gave 24.212 ms prepared versus 24.213 ms for the control. Keep the simpler
per-work-item seed construction.

### Prepare reciprocal opacity per Gaussian

**Date:** 2026-08-31
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

The unused fourth component of the prepared sampling record temporarily stored
`1 / alpha`. Splatting multiplied the inverse-dilog result by this value instead
of dividing by alpha in the supersampling loop. This added no storage but moved
the reciprocal calculation to expansion. An immediate control restored the
division.

| Opacity normalization | Median `gps expand` | Median `gps splat` | Combined |
| --- | ---: | ---: | ---: |
| Prepared reciprocal | 1.753 ms | 17.454 ms | 19.207 ms |
| Division control | 1.635 ms | 17.330 ms | 18.965 ms |

The prepared reciprocal regressed expansion by 0.118 ms and splatting by
0.124 ms, increasing combined time by 0.242 ms (1.3%). The shader compiler or
driver already handles the loop-invariant division efficiently; keep the
original expression.

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

### Pre-scale prepared raster transforms for supersampling

**Date:** 2026-09-01
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

Expansion temporarily multiplied each Gaussian's prepared raster mean and
Cholesky basis by the supersampling factor. This replaced one final `vec2`
multiply per generated point with three scalar multiplies per Gaussian. An
immediate control restored the original unscaled prepared values.

| Transform scaling | Median `gps expand` | Median `gps splat` | Combined |
| --- | ---: | ---: | ---: |
| Pre-scaled during expansion | 5.578 ms | 22.725 ms | 28.303 ms |
| Per-point control | 4.776 ms | 21.940 ms | 26.716 ms |

Both captures experienced an unusually heavy but comparable environment:
median cull/project was 21.162 ms for the variant and 20.954 ms for the
control, while clear and resolve were also similar. Pre-scaling regressed
combined expand and splat time by 5.9%, so the original per-point scaling is
retained. It also preserves the original floating-point operation ordering.

### Collapse pixel bounds to an unsigned vector comparison

**Date:** 2026-09-02
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

The splat shader temporarily converted the candidate pixel and extent to
`uvec2` and used one vector `lessThan` check. Converting a negative pixel to
unsigned still rejects it, so this preserved the original bounds behavior
while expressing four scalar comparisons more compactly.

| Bounds check | Median `gps splat` | p90 `gps splat` |
| --- | ---: | ---: |
| Unsigned vector comparison | 1.386 ms | 1.558 ms |
| Four scalar comparisons, control | 1.388 ms | 1.559 ms |

The 0.002 ms median difference is negligible across more than 1,250 measured
frames, with identical p90 timing and stable surrounding passes. The compiler
already handles the scalar expression well, so the clearer original check is
retained.

### Pack two work items into each compact record

**Date:** 2026-09-06
**Configuration:** 8M budget, 2x supersampling, accumulation off,
deterministic look motion.
**Decision:** reverted.

Expansion temporarily emitted one compact record for each pair of adjacent
Gaussian work items. Each splat invocation processed at most two work items,
preserving their sample indices and limiting the serial batch much more tightly
than the earlier persistent-consumer experiment. The record buffer and
capacity dispatch were halved.

| Record batch size | Median `gps expand` | Median `gps splat` | Combined |
| ---: | ---: | ---: | ---: |
| Two work items | 1.002 ms | 19.709 ms | 20.711 ms |
| One work item, control | 1.575 ms | 16.836 ms | 18.411 ms |

Chunking reduced expansion by 36.4%, but the extra serial sampling per
invocation regressed splatting by 17.1% and combined time by 12.5%. It can also
waste half a record at the configured limit for Gaussians with odd work counts.
Even this small batch sacrifices too much parallelism on this GPU, so the
one-record-per-work-item path remains preferable.

### Hoist the first RNG linear step out of the point loop

**Date:** 2026-09-08
**Hardware:** NVIDIA GeForce GTX 1060 6 GB, driver 580.173.02, Linux/OpenGL.
**Scene/viewport:** `res/export_n01.sog`, default 1280x720 window, 2x GPS
supersampling (2560x1440 buffers), accumulation and diagnostics off.
**Decision:** reverted; no measurable benefit.

The splat shader temporarily applied the sample-index offset and initial
`1664525u * seed + 1013904223u` before the supersampling loop. Each iteration
copied that seed and advanced its x component by `374761393u * 1664525u`.
Distributivity modulo 2^32 preserves the original RNG inputs exactly while
replacing repeated offset/LCG arithmetic with one integer addition per point.
Unlike the earlier per-Gaussian seed experiment, this changed no prepared data
or expansion work.

Ran baseline/variant/restored-control captures using:

```sh
./profile.sh --render-mode gps --gps-ss 2 --gps-accumulation off \
  --gps-budget-m 8 --seconds 15 --warmup-frames 120 --label gps-rng-baseline
# Repeat with labels gps-rng-hoisted and gps-rng-control after each edit.
```

Camera motion was the default deterministic `look`. Times below are
median / p90 in ms; GPU totals are profile.sh's representative-frame totals.

| GPU zone | Baseline | Hoisted | Restored control |
| --- | ---: | ---: | ---: |
| Cull/project | 3.558 / 4.158 | 3.568 / 4.244 | 3.556 / 4.198 |
| Clear | 0.386 / 0.389 | 0.386 / 0.390 | 0.386 / 0.389 |
| Expand | 1.448 / 2.258 | 1.454 / 2.218 | 1.449 / 2.355 |
| Splat | 16.736 / 18.846 | 16.785 / 19.267 | 16.850 / 19.002 |
| Resolve | 1.036 / 1.048 | 1.036 / 1.052 | 1.042 / 1.055 |
| Representative GPU total | 23.719 | 23.695 | 23.769 |

The variant's median splat time lies between the controls (only 0.05% below
their average), while its p90 is worse than both. The GPU-total difference is
also negligible (0.2% versus the control average). Desktop GPU clients remained
running; no clocks or external processes were changed. These captures do not
justify retaining the optimization.

Both shader targets (`glsl430` and `wgsl`) generated successfully and the native
profiler build ran successfully for all three captures. A deterministic CPU
uint32 reference comparison checked 300,000 final RNG pairs over supersampling
factors 1–4, randomized IDs/seeds and boundary sample indices including uint32
wraparound: all matched exactly. Sampling arithmetic, sample IDs, counts and
dispatch mapping were unchanged. No appearance change was intended; no visual
comparison was performed. The original shader and binary were restored and
rebuilt for the final control.

Raw captures and CSVs are under `build/profiles/` in
`20260908-164715-gps-rng-baseline`, `20260908-164802-gps-rng-hoisted`, and
`20260908-164902-gps-rng-control` (untracked).

### Reduce splat workgroups from 256 to 64 threads

**Date:** 2026-09-09
**Hardware:** NVIDIA GeForce GTX 1060 6 GB, driver 580.173.02, Linux/OpenGL.
**Scene/viewport:** `res/export_n01.sog`, default 1280x720 window, 2x GPS
supersampling (2560x1440 buffers), accumulation and diagnostics off.
**Decision:** reverted; smaller groups were slower.

The experiment changed only the splat shader's local size and the matching CPU
dispatch group count and row stride. Smaller groups might improve occupancy
for the register-heavy sampling shader. Unlike persistent consumers or paired
records, this retained one invocation per compact work item and the original
supersampling loop. Expansion, compact records, sample IDs, RNG and all
floating-point sampling operations were untouched.

Ran baseline/variant/restored-control captures with default deterministic
`look` camera motion:

```sh
./profile.sh --render-mode gps --gps-ss 2 --gps-accumulation off \
  --gps-budget-m 8 --seconds 15 --warmup-frames 120 --label gps-wg256-baseline
# Repeat with labels gps-wg64 and gps-wg256-control after each edit.
```

Times are median / p90 in ms; totals are profile.sh's representative-frame
measured GPU totals, not sums of zone medians.

| GPU zone | 256 baseline | 64 threads | 256 restored control |
| --- | ---: | ---: | ---: |
| Cull/project | 3.543 / 4.009 | 3.555 / 4.008 | 3.529 / 4.025 |
| Clear | 0.387 / 0.389 | 0.387 / 0.388 | 0.386 / 0.388 |
| Expand | 1.448 / 2.266 | 1.446 / 2.301 | 1.479 / 2.276 |
| Splat | 16.885 / 19.178 | 17.223 / 19.545 | 16.992 / 19.145 |
| Resolve | 1.065 / 1.077 | 1.054 / 1.066 | 1.054 / 1.065 |
| Representative GPU total | 23.896 | 24.149 | 24.014 |

The 64-thread variant regressed median splatting by 1.7% against the average
of the controls, and p90 by 2.0%. Its representative GPU total was 0.194 ms
(0.8%) worse. Surrounding passes were similar. This does not establish the
hardware cause: smaller groups also increase dispatch overhead and, at 8Mi
capacity, require three dispatch rows with additional rejected invocations.
Keep 256-thread groups for this workload.

All three profiler builds succeeded, including `glsl430` and `wgsl` shader
generation. CPU dispatch checks verified contiguous, unique work-index coverage
for both group sizes at partial-group and 65535-group row boundaries, plus 8Mi
and 250Mi capacities. The count guard rejects all extra invocations, preserving
the sampled work set. Scheduling and framebuffer races can still differ; no
bitwise framebuffer or visual comparison was performed. The shader and CPU
dispatch were restored and rebuilt before the final control capture.

Raw traces and CSVs remain untracked under `build/profiles/`:
`20260909-164354-gps-wg256-baseline`, `20260909-164503-gps-wg64`, and
`20260909-164635-gps-wg256-control`.

### Skip clearing the GPS color buffer

**Date:** 2026-09-10
**Hardware:** NVIDIA GeForce GTX 1060 6 GB, driver 580.173.02, Linux/OpenGL.
**Scene/viewport:** `res/export_n01.sog`, default 1280x720 window, 2x GPS
supersampling (2560x1440 buffers), accumulation and diagnostics off.
**Decision:** reverted; clear-pass gain, inconclusive end-to-end benefit.

Removed the clear shader's color-buffer store and binding, together with the
matching CPU binding. This saves 14.0625 MiB of writes per frame at this size.
Resolve reads color only when depth differs from the empty sentinel, and every
successful depth update schedules a fresh color write before the splat pass
finishes. Thus untouched pixels can retain stale colors without exposing them.
Sample generation, depth clearing, work records, RNG and point placement were
unchanged. This does not fix the prototype's existing depth/color write race.

Ran baseline/variant/restored-control captures with default deterministic
`look` camera motion:

```sh
./profile.sh --render-mode gps --gps-ss 2 --gps-accumulation off \
  --gps-budget-m 8 --seconds 15 --warmup-frames 120 --label gps-color-clear-baseline
# Repeat with labels gps-depth-only-clear and gps-color-clear-control.
```

Times are median / p90 in ms; totals are profile.sh's representative-frame
measured GPU totals.

| GPU zone | Baseline | Depth-only clear | Restored control |
| --- | ---: | ---: | ---: |
| Cull/project | 3.611 / 4.192 | 3.594 / 4.214 | 3.591 / 4.242 |
| Clear | 0.387 / 0.389 | 0.240 / 0.244 | 0.387 / 0.389 |
| Expand | 1.450 / 2.319 | 1.500 / 2.541 | 1.494 / 2.443 |
| Splat | 17.062 / 19.356 | 17.286 / 19.609 | 17.452 / 19.622 |
| Resolve | 1.046 / 1.057 | 1.055 / 1.066 | 1.052 / 1.063 |
| Representative GPU total | 24.028 | 24.227 | 24.392 |

Median clear time improved by 0.147 ms (38.0%), but the variant's GPU total
was 0.017 ms above the average of the controls (24.210 ms). Splat times drifted
by 0.390 ms between controls, more than the clear saving. These captures show
a real reduction in clear work but neither a reliable overall win nor evidence
that skipping the clear causes the surrounding-pass slowdown. Under the
keep-only-if-beneficial rule, retain the original implementation. Revisit with
longer, more stable captures rather than assuming a 38% clear gain means a
frame-time gain.

All three native profiler builds and captures succeeded, including shader
generation for `glsl430` and `wgsl`. Inspected all GPS color-buffer accesses:
only splat writes and depth-guarded resolve reads remain after removing clear.
No framebuffer or visual comparison was performed. Both source files were
restored and the original renderer rebuilt for the final control capture.

Raw traces and CSVs remain untracked under `build/profiles/`:
`20260910-135625-gps-color-clear-baseline`,
`20260910-135724-gps-depth-only-clear`, and
`20260910-135817-gps-color-clear-control`.

### Skip repeated pixel atomics within a work item

**Date:** 2026-09-12
**Hardware:** NVIDIA GeForce GTX 1060 6 GB, driver 580.173.02, Linux/OpenGL.
**Scene/viewport:** `res/export_n01.sog`, default 1280x720 window, 2x GPS
supersampling (2560x1440 buffers), accumulation and diagnostics off.
**Decision:** reverted; no reliable benefit across timing statistics.

The splat shader tracked the last in-bounds pixel index in one uint, initially
`0xFFFFFFFFu`. A sample hitting that same pixel skipped `atomicMin`; offscreen
samples left the tracked index unchanged. Since a work item's depth key is
constant and framebuffer depths only decrease, repeating that key cannot win
after its first submission, even if another invocation updates depth between
samples. This needs no framebuffer read, unlike the rejected early-occlusion
experiment. Sampling, RNG, sample IDs, point placement and dispatch stayed
unchanged; the cost is a live register and a branch per in-bounds sample.

Ran baseline/variant/restored-control/variant-confirmation captures:

```sh
./profile.sh --render-mode gps --gps-ss 2 --gps-accumulation off \
  --gps-budget-m 8 --seconds 15 --warmup-frames 120 --label gps-repeat-pixel-baseline
# Repeat with labels gps-repeat-pixel-skip, gps-repeat-pixel-control,
# and gps-repeat-pixel-confirm after each edit. Camera motion defaults to look.
```

Times are ms. GPU totals are profile.sh's representative-frame measurements.

| Measurement | Baseline | Skip repeats | Restored control | Skip confirmation |
| --- | ---: | ---: | ---: | ---: |
| Median splat | 16.923 | 16.876 | 17.267 | 16.868 |
| Mean splat | 16.741 | 16.795 | 16.899 | 16.833 |
| p90 splat | 19.192 | 19.114 | 19.173 | 19.190 |
| Median expand | 1.446 | 1.471 | 1.502 | 1.454 |
| p90 expand | 2.462 | 2.337 | 2.469 | 2.379 |
| Representative GPU total | 23.883 | 23.868 | 24.141 | 23.843 |

Variant medians repeated well, averaging 1.3% below the controls, but that
comparison is dominated by the slower restored control. Against the original
baseline, the variants improved median splat by only 0.3% and GPU total by
0.015–0.040 ms. Average mean splat was effectively unchanged: 16.814 ms for
the variants versus 16.820 ms for controls; p90 was also essentially unchanged.
This is suggestive rather than a demonstrated performance win. Keep the
original shader instead of adding state and branching on this evidence.

All four profiler builds/captures succeeded with `glsl430` and `wgsl` shader
generation. An exhaustive CPU model tested 331,776 schedules over two pixels,
four samples, offscreen samples, initially empty/near/equal/far depths, and
competing depth updates between samples. Final depths and successful color-write
events matched the unoptimized model in every case. This validates the skipped
atomic's redundancy, not GPU framebuffer race behavior; no visual or bitwise
framebuffer comparison was performed. The original shader was restored and
rebuilt after the confirmation capture.

Raw traces and CSVs remain untracked under `build/profiles/`:
`20260912-143935-gps-repeat-pixel-baseline`,
`20260912-144021-gps-repeat-pixel-skip`,
`20260912-144126-gps-repeat-pixel-control`, and
`20260912-144215-gps-repeat-pixel-confirm`.

## Ranked next experiments

### 1. Isolate low-risk splat shader costs

Use one-variable-at-a-time experiments around RNG generation, the `inv_dilog`
polynomial, transcendental operations (`log`, `sqrt`, `sin`, and `cos`), and
the supersampling loop. Preserve random inputs and output distribution unless
an approximation is being evaluated explicitly.

**Potential benefit:** high because this code runs for every generated point.
**Risk:** low for algebraic/hoisting changes, high for numerical
approximations.
**Small experiment:** use shader variants or temporary Tracy comparisons to
measure groups of operations before changing their implementation.

### 2. Revisit dispatch only with API support or new evidence

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
