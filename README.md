A real-time 3D Gaussian splatting renderer that runs natively or on the web.
Supports `.ply`, `.sog`, and experimental `.gsanim` 0.1 files.

Startup defaults to **GPS Prototype** with a **16M work budget** (128 MiB).
Override with `render_mode=alpha`, `render_mode=stochastic`, or `gps_budget_m=N`.

Gaussian uploads use chunks of at most 64 MiB, preserving full float precision
and global depth sorting without requiring a single multi-GB source buffer.
PLY decoding also reads bounded batches instead of retaining the entire file.

Before upload, the renderer checks storage-buffer and compute limits and logs
the estimated scene-buffer memory requirement. With `GL_NVX_gpu_memory_info`
(typically NVIDIA), the automatic budget is 80% of currently available VRAM;
otherwise it is 1024 MiB. Set `gpu_budget_mb=4096`, for example, to specify a
different total scene-buffer cap. New allocations must also fit within 80% of
currently free VRAM when that query is supported. The cap covers scene buffers,
not window-sized render targets or other assets; leave headroom for those and
the desktop.

Scenes exceeding these limits are rejected before GPU upload, and OpenGL buffer
allocation failures cause a clean exit rather than an assertion/core dump.
Chunking removes the per-buffer size limitation, not the total memory requirement:
10 million Gaussians still need roughly 3.6 GiB of scene buffers plus the GPS
work buffer, render targets, and CPU decoding storage. A compressed `.sog` reduces
disk size, but its decoded data currently uses the same full-precision layout.

Changing **GPS Work Budget** replaces only the GPS work buffer, preserving the
rest of the loaded scene. The old buffer stays resident until its replacement
is valid, so the resize needs temporary space for both work buffers. A failed
resize keeps the previous budget and accumulated image, and displays a warning.
Each 1M work items costs 8 MiB; the maximum 250M setting costs 2000 MiB.

The native OpenGL GPS UI reports requested work and warns when the global cap
drops Gaussian work (sampled at most twice per second). This diagnoses budget
exhaustion; it does not remove the prototype's dropping behavior.

Run the small software-OpenGL regression test after a native build:
`bash tests/test_gps_work_budget.sh`. It requires Mesa/llvmpipe and a display,
simulates low free VRAM, and never opens the large scene assets.

## Baked appearance animation proof of concept

```sh
./build.sh
./gsplat ply=animation_poc/tv_loop.gsanim
```

GSAnim defaults to the existing sorted alpha renderer. The Info panel provides
play/pause, time scrubbing, and state selection (`-1` follows the timeline).
WASD and mouse look stay on. The supplied scene is a **synthetic
0.4-second, 10 Hz step loop**, not the original Blender animation. Illumination
and reflections are baked; geometry and opacity never animate. No relighting,
training, or changes to the supplied asset are performed.

The loader implements `animation_poc/SPEC.md`, including signed, reference-relative
int8 deltas and step/linear looping. Geometry and packed deltas upload once;
the cull/SH shader reads appearance by original Gaussian ID before view-dependent
sorting. Only timeline uniforms change per frame. Temporal history is invalidated
when appearance changes. Linear manifests crossfade decoded SH coefficients,
including the last-to-first interval; this is a transport test, not physically
correct intermediate TV noise.

The optional `reference_views.json` beside the asset supplies Blender cameras
for setup. The fly camera starts on the first of those views (Z up, in asset
space) and can be moved. Check **Reference camera** in the Info panel to lock
the view for the slider and the A-B transition. Use `baked_views=PATH` to
specify the file explicitly, including for a reference-decoded static PLY.
Reference cameras keep asset coordinates, use a black background, and preserve
vertical FOV when expanding a square view to widescreen. The sorted
reference path renders Gaussians alone into RGBA16F before display clipping;
it applies no gamma, exposure, or tone map. Ordinary static-scene defaults stay
unchanged. GPS/stochastic modes remain available as experimental alternatives,
but sorted alpha is the reference-compatible comparison mode.

Reproduce a paused state with `anim_time=0.2` or `anim_state=2`. State selection
pauses playback; pressing Play returns to the timeline. A missing optional camera
file leaves the Z-up fly camera at the origin. POC limits are 1M Gaussians, 256
states, 65536 keys, 1 MiB metadata/header, 256 MiB deltas, and a 512 MiB archive;
GPU buffer and available-memory limits may impose lower caps. Unsupported
versions/encodings, invalid timelines, nonfinite values, bad schemas, forbidden
delta -128, CRC failures, and unknown/duplicate ZIP entries are rejected.

```sh
python -m unittest discover -s tests -p test_gsanim.py
python -m unittest discover -s animation_poc -p test_codec.py
python tests/test_gsanim_render.py .amp/in/artifacts/conformance
bash tests/render_gsanim.sh ply=animation_poc/tv_loop.gsanim \
  warmup=3 interval=10 csv=.amp/in/artifacts/play.csv
# Same measurement paused, or with a camera transition:
bash tests/render_gsanim.sh ply=animation_poc/tv_loop.gsanim \
  anim_time=0.1 transition=1 warmup=3 interval=10 csv=.amp/in/artifacts/paused.csv
```

The native GL measurement harness runs the actual app at 1920×1080 with VSync
off, rejects an incorrect framebuffer size, and reports wall-frame, CPU-submit,
and asynchronous GPU elapsed-query median/p95 times. CPU submission is not GPU
completion time. `frames=8 width=768 height=768 capture=PATH.ppm` captures without
benchmarking; `ui=1` includes the controls. NVIDIA free-memory deltas can include
other desktop processes; use per-process `nvidia-smi` memory alongside the logged
logical scene-buffer allocation. Shader compilation/startup and scene loading
are logged separately. The rendered tests compare every state and two asymmetric
linear samples against `codec.py` at three cameras after camera/state changes.
