#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .amp/in
if [ ! -f .amp/in/desktop-million.ply ]; then
    python3 - <<'PY'
import struct

names = 'x y z scale_0 scale_1 scale_2 f_dc_0 f_dc_1 f_dc_2'.split()
header = ('ply\nformat binary_little_endian 1.0\nelement vertex 1000000\n' +
          ''.join('property float ' + name + '\n' for name in names) + 'end_header\n')
batch = b''.join(struct.pack('<9f', (i % 100 - 49.5) * .03, (i // 100 - 49.5) * .03,
                             3., -4., -4., -4., .8, .2, -.5) for i in range(10000))
with open('.amp/in/desktop-million.ply', 'wb') as scene:
    scene.write(header.encode())
    for _ in range(100):
        scene.write(batch)
PY
fi
exec env LIBGL_ALWAYS_SOFTWARE=1 ./gsplat ply=.amp/in/desktop-million.ply render_mode=alpha gps_budget_m=1
