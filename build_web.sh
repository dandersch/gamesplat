#!/bin/bash
set -euo pipefail

CC="${CC:-emcc}"
CXX="${CXX:-em++}"
BUILD_ROOT="${BUILD_DIR:-build/web}"
SHELL_FILE="${SHELL_FILE:-web/shell.html}"
VENDOR_DIR="vendor"
WEBP_DIR="$VENDOR_DIR/libwebp"
WEBP_LIB="$BUILD_ROOT/libwebpdecoder.a"
EM_CACHE_DIR="${EM_CACHE_DIR:-$BUILD_ROOT/emscripten_cache}"

# The demo asset pack is large. Keep the known-good initial heap as the
# default, but make it easy to tune down/up while testing browser behavior.
INITIAL_MEMORY="${INITIAL_MEMORY:-671088640}"
MAXIMUM_MEMORY="${MAXIMUM_MEMORY:-2147483648}"
USE_PRELOAD_CACHE="${USE_PRELOAD_CACHE:-1}"

mkdir -p "$BUILD_ROOT"

echo "Generating sokol-shdc headers..."
for name in wireframe overlay darken mesh splat accum; do
    ./bin/sokol-shdc --input "shaders/$name.glsl" --output "shaders/$name.glsl.h" --slang glsl430:glsl300es:wgsl
done

COMMON_FLAGS_BASE=(
    -O2
    -std=c++17
    -Wall
    -Wextra
    -Wno-missing-field-initializers
    -Wno-unused-function
    -I.
    -I"$VENDOR_DIR"
    -I"$VENDOR_DIR/imgui"
    -I"$VENDOR_DIR/imgui/backends"
    -I"$WEBP_DIR"
    -I"$WEBP_DIR/src"
)

WEBP_CFLAGS=(
    -O2
    -Wall
    -Wextra
    -Wno-unused-function
    -I"$WEBP_DIR"
    -I"$WEBP_DIR/src"
)

WEBP_DECODER_SOURCES=(
    "$WEBP_DIR/src/dec/alpha_dec.c"
    "$WEBP_DIR/src/dec/buffer_dec.c"
    "$WEBP_DIR/src/dec/frame_dec.c"
    "$WEBP_DIR/src/dec/idec_dec.c"
    "$WEBP_DIR/src/dec/io_dec.c"
    "$WEBP_DIR/src/dec/quant_dec.c"
    "$WEBP_DIR/src/dec/tree_dec.c"
    "$WEBP_DIR/src/dec/vp8_dec.c"
    "$WEBP_DIR/src/dec/vp8l_dec.c"
    "$WEBP_DIR/src/dec/webp_dec.c"

    "$WEBP_DIR/src/dsp/alpha_processing.c"
    "$WEBP_DIR/src/dsp/cpu.c"
    "$WEBP_DIR/src/dsp/dec.c"
    "$WEBP_DIR/src/dsp/dec_clip_tables.c"
    "$WEBP_DIR/src/dsp/filters.c"
    "$WEBP_DIR/src/dsp/lossless.c"
    "$WEBP_DIR/src/dsp/rescaler.c"
    "$WEBP_DIR/src/dsp/upsampling.c"
    "$WEBP_DIR/src/dsp/yuv.c"

    "$WEBP_DIR/src/utils/bit_reader_utils.c"
    "$WEBP_DIR/src/utils/color_cache_utils.c"
    "$WEBP_DIR/src/utils/filters_utils.c"
    "$WEBP_DIR/src/utils/huffman_utils.c"
    "$WEBP_DIR/src/utils/palette.c"
    "$WEBP_DIR/src/utils/quant_levels_dec_utils.c"
    "$WEBP_DIR/src/utils/random_utils.c"
    "$WEBP_DIR/src/utils/rescaler_utils.c"
    "$WEBP_DIR/src/utils/thread_utils.c"
    "$WEBP_DIR/src/utils/utils.c"
)

EM_FLAGS_BASE=(
    -sUSE_SDL=3
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY="$INITIAL_MEMORY"
    -sMAXIMUM_MEMORY="$MAXIMUM_MEMORY"
    -sEXIT_RUNTIME=0
    -sENVIRONMENT=web
    --shell-file "$SHELL_FILE"
)

PRELOAD_FLAGS=(
    --preload-file res/export_n01.sog@res/export_n01.sog
)

if [ "$USE_PRELOAD_CACHE" = "1" ]; then
    PRELOAD_FLAGS+=(--use-preload-cache)
fi

SOURCES=(
    src/main.cpp
    "$VENDOR_DIR/miniz.c"
    "$VENDOR_DIR/third_party_impl.cpp"
    "$VENDOR_DIR/imgui/imgui.cpp"
    "$VENDOR_DIR/imgui/imgui_draw.cpp"
    "$VENDOR_DIR/imgui/imgui_tables.cpp"
    "$VENDOR_DIR/imgui/imgui_widgets.cpp"
)

echo "Building libwebp decoder..."
WEBP_OBJS=()
for src in "${WEBP_DECODER_SOURCES[@]}"; do
    rel="${src#$WEBP_DIR/src/}"
    obj="$BUILD_ROOT/webp/${rel%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "$CC" "${WEBP_CFLAGS[@]}" -c "$src" -o "$obj"
    WEBP_OBJS+=("$obj")
done
emar rcs "$WEBP_LIB" "${WEBP_OBJS[@]}"

build_app() {
    local backend="$1"
    local out_dir="$2"
    shift 2
    local backend_flags=("$@")
    local out="$out_dir/index.html"

    mkdir -p "$out_dir"
    echo "Building $backend: $out..."
    "$CXX" --cache "$EM_CACHE_DIR" "${COMMON_FLAGS_BASE[@]}" "${backend_flags[@]}" "${EM_FLAGS_BASE[@]}" \
        "${SOURCES[@]}" "${PRELOAD_FLAGS[@]}" \
        "$WEBP_LIB" \
        -o "$out"
    echo "Done: $out"
}

build_app "WebGL2" "$BUILD_ROOT/webgl" \
    -DSOKOL_GLES3 \
    -sUSE_WEBGL2=1 \
    -sFULL_ES3=1 \
    -sMIN_WEBGL_VERSION=2 \
    -sMAX_WEBGL_VERSION=2

build_app "WebGPU" "$BUILD_ROOT/webgpu" \
    -DSOKOL_WGPU \
    --use-port=emdawnwebgpu \
    -sASYNCIFY=1

echo "Done: $BUILD_ROOT/webgl/index.html and $BUILD_ROOT/webgpu/index.html"
