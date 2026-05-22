#!/bin/bash
set -e

CXX="${CXX:-g++}"
CXXFLAGS="-O2 -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function"
LDFLAGS="-lSDL3 -lsqlite3 -lm"
OUT="gsplat"

IMGUI_DIR="third_party/imgui"
IMGUI_LIB="third_party/libimgui.a"
THIRDPARTY_LIB="third_party/libthirdparty.a"
THIRDPARTY_SRC="third_party/third_party_impl.cpp"

# Optional: stage the sokol_gfx + sokol_imgui implementations into
# libthirdparty.a (still inert at runtime; the renderer port wires them up in
# a later commit). Enable with: GSPLAT_USE_SOKOL=1 ./build.sh
if [ -n "$GSPLAT_USE_SOKOL" ]; then
    CXXFLAGS="$CXXFLAGS -DGSPLAT_USE_SOKOL"
    THIRDPARTY_LIB="third_party/libthirdparty_sokol.a"   # separate cache slot
    # GL loader + thread/dl deps that sokol_gfx's GLCORE backend needs.
    LDFLAGS="$LDFLAGS -lGL -ldl -lpthread"
fi

echo "Generating sokol-shdc headers..."
# Sokol-shdc transpiles annotated #version-450 GLSL into C headers containing
# embedded per-backend shader sources plus a code-generated sg_shader_desc.
# These headers are NOT consumed by the running binary yet (the SDL_GPU
# renderer still loads the .spv files compiled below). They are produced and
# included in libthirdparty.a under GSPLAT_USE_SOKOL so we can be sure they
# compile, ready for the renderer port.
for name in wireframe overlay darken mesh splat; do
    ./sokol-shdc --input "shaders/$name.glsl" --output "shaders/$name.glsl.h" --slang glsl430:glsl300es
done

echo "Compiling shaders..."
glslc -fshader-stage=vertex shaders/splat.vert.glsl -o shaders/splat.vert.spv
glslc -fshader-stage=fragment shaders/splat.frag.glsl -o shaders/splat.frag.spv
glslc -fshader-stage=vertex shaders/overlay.vert.glsl -o shaders/overlay.vert.spv
glslc -fshader-stage=fragment shaders/overlay.frag.glsl -o shaders/overlay.frag.spv
glslc -fshader-stage=fragment shaders/darken.frag.glsl -o shaders/darken.frag.spv
glslc -fshader-stage=vertex shaders/wireframe.vert.glsl -o shaders/wireframe.vert.spv
glslc -fshader-stage=fragment shaders/wireframe.frag.glsl -o shaders/wireframe.frag.spv
glslc -fshader-stage=vertex shaders/mesh.vert.glsl -o shaders/mesh.vert.spv
glslc -fshader-stage=fragment shaders/mesh.frag.glsl -o shaders/mesh.frag.spv

# Build imgui static lib if missing
if [ ! -f "$IMGUI_LIB" ]; then
    echo "Building imgui..."
    IMGUI_OBJS=()
    for src in "$IMGUI_DIR"/imgui.cpp "$IMGUI_DIR"/imgui_draw.cpp \
               "$IMGUI_DIR"/imgui_tables.cpp "$IMGUI_DIR"/imgui_widgets.cpp \
               "$IMGUI_DIR"/backends/imgui_impl_sdl3.cpp \
               "$IMGUI_DIR"/backends/imgui_impl_sdlgpu3.cpp; do
        obj="${src%.cpp}.o"
        $CXX $CXXFLAGS -I"$IMGUI_DIR" -I"$IMGUI_DIR/backends" -c "$src" -o "$obj"
        IMGUI_OBJS+=("$obj")
    done
    ar rcs "$IMGUI_LIB" "${IMGUI_OBJS[@]}"
    rm "${IMGUI_OBJS[@]}"
fi

# Build single-header third_party static lib if missing or out of date
if [ ! -f "$THIRDPARTY_LIB" ] || [ "$THIRDPARTY_SRC" -nt "$THIRDPARTY_LIB" ]; then
    echo "Building third_party single-header impls..."
    obj="third_party/third_party_impl.o"
    # When sokol is staged in, sokol_imgui.h needs ImGui's headers visible.
    EXTRA_TP_INCS=""
    if [ -n "$GSPLAT_USE_SOKOL" ]; then
        EXTRA_TP_INCS="-I$IMGUI_DIR"
    fi
    $CXX $CXXFLAGS -Ithird_party $EXTRA_TP_INCS -c "$THIRDPARTY_SRC" -o "$obj"
    ar rcs "$THIRDPARTY_LIB" "$obj"
    rm "$obj"
fi

echo "Building $OUT..."
$CXX $CXXFLAGS -I"$IMGUI_DIR" -I"$IMGUI_DIR/backends" -Ithird_party src/main.cpp -o "$OUT" "$IMGUI_LIB" "$THIRDPARTY_LIB" $LDFLAGS

echo "Done: ./$OUT"
