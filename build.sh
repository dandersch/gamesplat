#!/bin/bash
set -e

CXX="${CXX:-g++}"
CXXFLAGS="-O2 -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function"
# sokol_app's GLCORE backend needs the Linux windowing/OpenGL libraries plus
# libdl/pthread deps for sokol's built-in GL loader. SDL is still linked for
# utility calls (logging, file loading, threading), but no longer owns the
# window or event loop.
LDFLAGS="-lSDL3 -lsqlite3 -lm -lGL -lX11 -lXi -lXcursor -ldl -lpthread -lwebp"
OUT="gsplat"
ENABLE_PROFILER="${ENABLE_PROFILER:-0}"
HOTRELOAD="${HOTRELOAD:-0}"
if [ -z "${BUILD_DIR+x}" ]; then
    if [ "$HOTRELOAD" = "1" ]; then
        BUILD_DIR="build/hotreload"
    else
        BUILD_DIR="build"
    fi
fi
OBJ_DIR="$BUILD_DIR/obj"

VENDOR_DIR="vendor"
IMGUI_DIR="$VENDOR_DIR/imgui"
IMGUI_LIB="$BUILD_DIR/libimgui.a"
THIRDPARTY_LIB="$BUILD_DIR/libthirdparty.a"
THIRDPARTY_SRC="$VENDOR_DIR/third_party_impl.cpp"
TRACY_DIR="$VENDOR_DIR/tracy"
TRACY_LIB="$BUILD_DIR/libtracy.a"
TRACY_SRC="$TRACY_DIR/TracyClient.cpp"

mkdir -p "$BUILD_DIR" "$OBJ_DIR"

INCLUDE_FLAGS=(
    -I.
    -I"$IMGUI_DIR"
    -I"$IMGUI_DIR/backends"
    -I"$VENDOR_DIR"
    -I"$VENDOR_DIR/libwebp/src"
    -I"$TRACY_DIR"
)
SOKOL_BACKEND_FLAGS=("-DSOKOL_GLCORE")
PIC_FLAGS=()
if [ "$HOTRELOAD" = "1" ]; then
    PIC_FLAGS+=("-fPIC")
fi

TRACY_FLAGS=()
APP_PROFILE_FLAGS=()
PROFILE_LIBS=()
if [ "$ENABLE_PROFILER" = "1" ]; then
    TRACY_FLAGS+=("-DTRACY_ENABLE")
    APP_PROFILE_FLAGS+=("-DENABLE_PROFILER")
    PROFILE_LIBS+=("$TRACY_LIB")
    THIRDPARTY_LIB="$BUILD_DIR/libthirdparty_profiler.a"
fi

echo "Generating sokol-shdc headers..."
# Sokol-shdc transpiles annotated #version-450 GLSL into C headers containing
# embedded per-backend shader sources plus a code-generated sg_shader_desc.
# These are consumed at runtime by renderer.cpp via the *_shader_desc()
# helpers (one per program).
for name in wireframe overlay darken mesh splat accum; do
    ./bin/sokol-shdc --input "shaders/$name.glsl" --output "shaders/$name.glsl.h" --slang glsl430:glsl300es:wgsl
done

# Build imgui static lib if missing. sokol_imgui handles ImGui rendering and,
# with sokol_app, ImGui event translation too.
if [ ! -f "$IMGUI_LIB" ]; then
    echo "Building imgui..."
    IMGUI_OBJS=()
    for src in "$IMGUI_DIR"/imgui.cpp "$IMGUI_DIR"/imgui_draw.cpp \
               "$IMGUI_DIR"/imgui_tables.cpp "$IMGUI_DIR"/imgui_widgets.cpp; do
        rel="${src#$VENDOR_DIR/}"
        obj="$OBJ_DIR/${rel%.cpp}.o"
        mkdir -p "$(dirname "$obj")"
        $CXX $CXXFLAGS "${PIC_FLAGS[@]}" -I"$IMGUI_DIR" -I"$IMGUI_DIR/backends" -c "$src" -o "$obj"
        IMGUI_OBJS+=("$obj")
    done
    ar rcs "$IMGUI_LIB" "${IMGUI_OBJS[@]}"
    rm "${IMGUI_OBJS[@]}"
fi

if [ "$ENABLE_PROFILER" = "1" ]; then
    if [ ! -f "$TRACY_LIB" ] || [ -n "$(find "$TRACY_DIR" -type f -newer "$TRACY_LIB" -print -quit)" ]; then
        echo "Building tracy..."
        tracy_obj="$OBJ_DIR/tracy/TracyClient.o"
        mkdir -p "$(dirname "$tracy_obj")"
        $CXX $CXXFLAGS "${PIC_FLAGS[@]}" -w "${TRACY_FLAGS[@]}" -I"$TRACY_DIR" -c "$TRACY_SRC" -o "$tracy_obj"
        ar rcs "$TRACY_LIB" "$tracy_obj"
        rm "$tracy_obj"
    fi
fi

# Build single-header vendor static lib if missing or out of date.
# sokol_imgui needs ImGui's headers visible so the impl can talk to ImGui's
# C++ API directly.
if [ ! -f "$THIRDPARTY_LIB" ] || [ "$THIRDPARTY_SRC" -nt "$THIRDPARTY_LIB" ] || [ "$VENDOR_DIR/miniz.c" -nt "$THIRDPARTY_LIB" ] || ! ar t "$THIRDPARTY_LIB" | grep -q '^miniz\.o$'; then
    echo "Building vendor single-header impls..."
    objs=("$OBJ_DIR/third_party_impl.o" "$OBJ_DIR/miniz.o")
    $CXX $CXXFLAGS "${PIC_FLAGS[@]}" "${SOKOL_BACKEND_FLAGS[@]}" "${APP_PROFILE_FLAGS[@]}" -I. -I"$VENDOR_DIR" -I"$IMGUI_DIR" -c "$THIRDPARTY_SRC" -o "${objs[0]}"
    $CXX -O2 -Wall -Wextra -Wno-unused-function "${PIC_FLAGS[@]}" -I"$VENDOR_DIR" -x c -c "$VENDOR_DIR/miniz.c" -o "${objs[1]}"
    ar rcs "$THIRDPARTY_LIB" "${objs[@]}"
    rm "${objs[@]}"
fi

if [ "$HOTRELOAD" = "1" ]; then
    CODE_SO="$BUILD_DIR/gsplat_code.so"
    SHIM_OUT="gsplat_hot"

    echo "Building $CODE_SO..."
    $CXX $CXXFLAGS -fPIC -shared -DCOMPILE_AS_DLL "${SOKOL_BACKEND_FLAGS[@]}" "${APP_PROFILE_FLAGS[@]}" "${INCLUDE_FLAGS[@]}" src/main.cpp -o "$CODE_SO" "$IMGUI_LIB" "$THIRDPARTY_LIB" "${PROFILE_LIBS[@]}" $LDFLAGS

    echo "Building $SHIM_OUT..."
    $CXX $CXXFLAGS src/shim.cpp -o "$SHIM_OUT" -lSDL3 -ldl

    echo "Done: ./$SHIM_OUT (reloads $CODE_SO)"
else
    echo "Building $OUT..."
    $CXX $CXXFLAGS "${SOKOL_BACKEND_FLAGS[@]}" "${APP_PROFILE_FLAGS[@]}" "${INCLUDE_FLAGS[@]}" src/main.cpp -o "$OUT" "$IMGUI_LIB" "$THIRDPARTY_LIB" "${PROFILE_LIBS[@]}" $LDFLAGS

    echo "Done: ./$OUT"
fi
