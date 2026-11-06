#!/bin/bash
set -euo pipefail
# Run ./build.sh first to generate shaders and vendor libraries.
out=build/gsanim_render
if [[ ! -x "$out" ]] || find src shaders tests/gsanim_render.cpp -type f -newer "$out" -print -quit | grep -q .; then
    "${CXX:-g++}" -O2 -std=c++17 -DSOKOL_GLCORE -I. -Ivendor -Ivendor/imgui -Ivendor/tracy \
        tests/gsanim_render.cpp -o "$out" build/libimgui.a build/libthirdparty.a \
        -Wl,--wrap=XrmGetResource -lSDL3 -lsqlite3 -lm -lGL -lX11 -lXi -lXcursor -ldl -lpthread -lwebp
fi
# Tiling WMs may ignore the requested size; change only this temporary window.
if command -v i3-msg >/dev/null && i3-msg -t get_version >/dev/null 2>&1; then
    width=1920 height=1080
    for arg in "$@"; do
        case "$arg" in width=*) width="${arg#width=}";; height=*) height="${arg#height=}";; esac
    done
    "$out" "$@" &
    pid=$!
    # Wait for i3 to manage the window, not just for X to create it. Querying
    # the tree also avoids xdotool/Xlib crashes during rapid capture restarts.
    window=$(python - <<'PY'
import json, subprocess, time
deadline = time.monotonic() + 10
while time.monotonic() < deadline:
    nodes = [json.loads(subprocess.check_output(['i3-msg', '-t', 'get_tree']))]
    while nodes:
        node = nodes.pop()
        nodes.extend(node.get('nodes', []) + node.get('floating_nodes', []))
        if node.get('name') == 'GSAnim validation' and node.get('window'):
            print(node['window'])
            raise SystemExit(0)
    time.sleep(.05)
raise SystemExit('Validation window did not appear')
PY
)
    i3-msg "[id=$window] floating enable, border pixel 0, resize set $width px $height px" >/dev/null
    wait "$pid"
else
    exec "$out" "$@"
fi
