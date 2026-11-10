#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .amp/in
if [ ! -f .amp/in/desktop-demo.ply ]; then
    python3 - <<'PY'
import struct

names = 'x y z scale_0 scale_1 scale_2 f_dc_0 f_dc_1 f_dc_2'.split()
header = ('ply\nformat binary_little_endian 1.0\nelement vertex 400\n' +
          ''.join('property float ' + name + '\n' for name in names) + 'end_header\n')
with open('.amp/in/desktop-demo.ply', 'wb') as scene:
    scene.write(header.encode())
    for i in range(400):
        scene.write(struct.pack('<9f', (i % 20 - 9.5) * .15, (i // 20 - 9.5) * .15,
                                3., -2.7, -2.7, -2.7, (i % 20 / 19 - .5) * 3,
                                (i // 20 / 19 - .5) * 3, .5))
PY
fi
exec env LIBGL_ALWAYS_SOFTWARE=1 ./gsplat ply=.amp/in/desktop-demo.ply render_mode=alpha gps_budget_m=1
