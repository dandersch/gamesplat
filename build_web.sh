#!/bin/bash
set -euo pipefail

CXX="${CXX:-em++}"
BUILD_DIR="${BUILD_DIR:-build/web}"
OUT="$BUILD_DIR/index.html"
SHELL_FILE="${SHELL_FILE:-web/shell.html}"

# The demo asset pack is large. Keep the known-good initial heap as the
# default, but make it easy to tune down/up while testing browser behavior.
INITIAL_MEMORY="${INITIAL_MEMORY:-671088640}"
MAXIMUM_MEMORY="${MAXIMUM_MEMORY:-2147483648}"
USE_PRELOAD_CACHE="${USE_PRELOAD_CACHE:-1}"

mkdir -p "$BUILD_DIR"

echo "Generating sokol-shdc headers..."
for name in wireframe overlay darken mesh splat; do
    ./sokol-shdc --input "shaders/$name.glsl" --output "shaders/$name.glsl.h" --slang glsl430:glsl300es
done

COMMON_FLAGS=(
    -O2
    -std=c++17
    -Wall
    -Wextra
    -Wno-missing-field-initializers
    -Wno-unused-function
    -Ithird_party
    -Ithird_party/imgui
    -Ithird_party/imgui/backends
)

EM_FLAGS=(
    -sUSE_SDL=3
    -sFULL_ES3=1
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY="$INITIAL_MEMORY"
    -sMAXIMUM_MEMORY="$MAXIMUM_MEMORY"
    -sEXIT_RUNTIME=0
    -sENVIRONMENT=web
    --shell-file "$SHELL_FILE"
)

PRELOAD_FLAGS=(
    --preload-file res/transition.wav@res/transition.wav
    --preload-file test/export_n01.ply@test/export_n01.ply
    --preload-file test/colmap/images@test/colmap/images
    --preload-file test/colmap/sparse/0@test/colmap/sparse/0
    --preload-file test/cyberpunk_guy.glb@test/cyberpunk_guy.glb
    --preload-file test/priest.glb@test/priest.glb
)

if [ "$USE_PRELOAD_CACHE" = "1" ]; then
    PRELOAD_FLAGS+=(--use-preload-cache)
fi

SOURCES=(
    src/main.cpp
    third_party/third_party_impl.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_sdl3.cpp
)

echo "Building $OUT..."
"$CXX" "${COMMON_FLAGS[@]}" "${EM_FLAGS[@]}" \
    "${SOURCES[@]}" "${PRELOAD_FLAGS[@]}" \
    -o "$OUT"

echo "Done: $OUT"
