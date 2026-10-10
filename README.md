A real-time 3D Gaussian splatting renderer that runs natively or on the web.
Supports `.ply` and `.sog` files.

Gaussian uploads use chunks of at most 64 MiB, preserving full float precision
and global depth sorting without requiring a single multi-GB source buffer.
PLY decoding also reads bounded batches instead of retaining the entire file.

Before upload, the renderer checks storage-buffer and compute limits and logs
the estimated scene-buffer memory requirement. With `GL_NVX_gpu_memory_info`
(typically NVIDIA), the automatic budget is 80% of currently available VRAM;
otherwise it is 1024 MiB. Set `gpu_budget_mb=4096`, for example, to specify a
different cap. An explicit cap never exceeds the 80% available-VRAM limit when
that query is supported. The budget covers scene buffers, not window-sized
render targets or other assets; leave headroom for those and the desktop.

Scenes exceeding these limits are rejected before GPU upload, and OpenGL buffer
allocation failures cause a clean exit rather than an assertion/core dump.
Chunking removes the per-buffer size limitation, not the total memory requirement:
10 million Gaussians still need roughly 3.6 GiB of scene buffers plus the GPS
work buffer, render targets, and CPU decoding storage. A compressed `.sog` reduces
disk size, but its decoded data currently uses the same full-precision layout.
