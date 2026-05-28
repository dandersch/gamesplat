#!/bin/bash
set -e

CXX="${CXX:-g++}"
CXXFLAGS="-O2 -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function"
# sokol_gfx's GLCORE backend needs an OpenGL context plus the libdl/pthread
# deps for its built-in GL loader.
LDFLAGS="-lSDL3 -lsqlite3 -lm -lGL -ldl -lpthread -lwebp"
OUT="gsplat"
ENABLE_PROFILER="${ENABLE_PROFILER:-0}"

IMGUI_DIR="third_party/imgui"
IMGUI_LIB="third_party/libimgui.a"
THIRDPARTY_LIB="third_party/libthirdparty.a"
THIRDPARTY_SRC="third_party/third_party_impl.cpp"
TRACY_DIR="third_party/tracy"
TRACY_LIB="third_party/libtracy.a"
TRACY_SRC="$TRACY_DIR/TracyClient.cpp"

INCLUDE_FLAGS=(
    -I"$IMGUI_DIR"
    -I"$IMGUI_DIR/backends"
    -Ithird_party
    -I"$TRACY_DIR"
)
SOKOL_BACKEND_FLAGS=("-DSOKOL_GLCORE")

TRACY_FLAGS=()
APP_PROFILE_FLAGS=()
PROFILE_LIBS=()
if [ "$ENABLE_PROFILER" = "1" ]; then
    TRACY_FLAGS+=("-DTRACY_ENABLE")
    APP_PROFILE_FLAGS+=("-DENABLE_PROFILER")
    PROFILE_LIBS+=("$TRACY_LIB")
    THIRDPARTY_LIB="third_party/libthirdparty_profiler.a"
fi

echo "Generating sokol-shdc headers..."
# Sokol-shdc transpiles annotated #version-450 GLSL into C headers containing
# embedded per-backend shader sources plus a code-generated sg_shader_desc.
# These are consumed at runtime by renderer.cpp via the *_shader_desc()
# helpers (one per program).
for name in wireframe overlay darken mesh splat; do
    ./bin/sokol-shdc --input "shaders/$name.glsl" --output "shaders/$name.glsl.h" --slang glsl430:glsl300es
done

# Build imgui static lib if missing. imgui_impl_sdlgpu3.cpp is no longer part
# of the build — sokol_imgui handles ImGui rendering. We keep imgui_impl_sdl3
# for translating SDL events into ImGui IO state.
if [ ! -f "$IMGUI_LIB" ]; then
    echo "Building imgui..."
    IMGUI_OBJS=()
    for src in "$IMGUI_DIR"/imgui.cpp "$IMGUI_DIR"/imgui_draw.cpp \
               "$IMGUI_DIR"/imgui_tables.cpp "$IMGUI_DIR"/imgui_widgets.cpp \
               "$IMGUI_DIR"/backends/imgui_impl_sdl3.cpp; do
        obj="${src%.cpp}.o"
        $CXX $CXXFLAGS -I"$IMGUI_DIR" -I"$IMGUI_DIR/backends" -c "$src" -o "$obj"
        IMGUI_OBJS+=("$obj")
    done
    ar rcs "$IMGUI_LIB" "${IMGUI_OBJS[@]}"
    rm "${IMGUI_OBJS[@]}"
fi

if [ "$ENABLE_PROFILER" = "1" ]; then
    if [ ! -f "$TRACY_LIB" ] || [ -n "$(find "$TRACY_DIR" -type f -newer "$TRACY_LIB" -print -quit)" ]; then
        echo "Building tracy..."
        tracy_obj="third_party/tracy/TracyClient.o"
        $CXX $CXXFLAGS -w "${TRACY_FLAGS[@]}" -I"$TRACY_DIR" -c "$TRACY_SRC" -o "$tracy_obj"
        ar rcs "$TRACY_LIB" "$tracy_obj"
        rm "$tracy_obj"
    fi
fi

# Build single-header third_party static lib if missing or out of date.
# sokol_imgui needs ImGui's headers visible so the impl can talk to ImGui's
# C++ API directly.
if [ ! -f "$THIRDPARTY_LIB" ] || [ "$THIRDPARTY_SRC" -nt "$THIRDPARTY_LIB" ] || [ third_party/miniz.c -nt "$THIRDPARTY_LIB" ] || ! ar t "$THIRDPARTY_LIB" | grep -q '^miniz\.o$'; then
    echo "Building third_party single-header impls..."
    objs=("third_party/third_party_impl.o" "third_party/miniz.o")
    $CXX $CXXFLAGS "${SOKOL_BACKEND_FLAGS[@]}" "${APP_PROFILE_FLAGS[@]}" -Ithird_party -I"$IMGUI_DIR" -c "$THIRDPARTY_SRC" -o "${objs[0]}"
    $CXX -O2 -Wall -Wextra -Wno-unused-function -Ithird_party -x c -c third_party/miniz.c -o "${objs[1]}"
    ar rcs "$THIRDPARTY_LIB" "${objs[@]}"
    rm "${objs[@]}"
fi

echo "Building $OUT..."
$CXX $CXXFLAGS "${SOKOL_BACKEND_FLAGS[@]}" "${APP_PROFILE_FLAGS[@]}" "${INCLUDE_FLAGS[@]}" src/main.cpp -o "$OUT" "$IMGUI_LIB" "$THIRDPARTY_LIB" "${PROFILE_LIBS[@]}" $LDFLAGS

echo "Done: ./$OUT"
