// Single translation unit that instantiates the implementations of the
// single-header libraries in third_party/. Built once into libthirdparty.a
// and cached so we don't pay the parse cost on every incremental build.

#include <cstddef>
#include <cstring>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

// --- sokol_gfx + sokol_imgui ---------------------------------------------
// Keep the native build on the desktop GL backend and switch only the
// Emscripten build to WebGL2/GLES3. The generated shader headers carry both
// glsl430 and glsl300es variants.
#define SOKOL_IMPL
#if defined(ENABLE_PROFILER)
#define SOKOL_TRACE_HOOKS
#endif
#if defined(__EMSCRIPTEN__)
#if !defined(SOKOL_GLES3)
#define SOKOL_GLES3
#endif
#else
#if !defined(SOKOL_GLCORE)
#define SOKOL_GLCORE
#endif
#endif
#include "sokol_gfx.h"

// sokol_imgui.h is the ImGui rendering backend (NOT to be confused with
// sokol_gfx_imgui.h, which is a debug inspector). We compile as C++ so it
// talks to the ImGui C++ API directly. SOKOL_IMGUI_NO_SOKOL_APP drops the
// sokol_app dependency since we use SDL3 for windowing/input.
#define SOKOL_IMGUI_IMPL
#define SOKOL_IMGUI_NO_SOKOL_APP
#include "imgui.h"
#include "sokol_imgui.h"
#if defined(ENABLE_PROFILER)
#define SOKOL_GFX_IMGUI_IMPL
#include "sokol_gfx_imgui.h"
#endif

// Pull the sokol-shdc generated shader headers into the same TU as
// sokol_gfx.h so the *_shader_desc() / SLOT_* / uniform-block-struct symbols
// are linkable from renderer.cpp.
#include "../shaders/wireframe.glsl.h"
#include "../shaders/overlay.glsl.h"
#include "../shaders/darken.glsl.h"
#include "../shaders/mesh.glsl.h"
#include "../shaders/splat.glsl.h"

#if defined(_MSC_VER)
#define GSPLAT_HOTRELOAD_EXPORT extern "C" __declspec(dllexport)
#else
#define GSPLAT_HOTRELOAD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// POC hot-reload hooks for preserving the private file-scope state owned by
// sokol_gfx.h and sokol_imgui.h across DLL reloads. These intentionally live in
// the same translation unit that defines SOKOL_*_IMPL so they can see the
// otherwise-internal `_sg` and `_simgui` objects.
GSPLAT_HOTRELOAD_EXPORT size_t gsplat_sg_state_size(void) {
    return sizeof(_sg);
}

GSPLAT_HOTRELOAD_EXPORT void gsplat_sg_state_save(void* dst, size_t dst_size) {
    if (dst && dst_size == sizeof(_sg)) {
        memcpy(dst, &_sg, sizeof(_sg));
    }
}

GSPLAT_HOTRELOAD_EXPORT void gsplat_sg_state_load(const void* src, size_t src_size) {
    if (src && src_size == sizeof(_sg)) {
        memcpy(&_sg, src, sizeof(_sg));
    }
}

GSPLAT_HOTRELOAD_EXPORT size_t gsplat_simgui_state_size(void) {
    return sizeof(_simgui);
}

GSPLAT_HOTRELOAD_EXPORT void gsplat_simgui_state_save(void* dst, size_t dst_size) {
    if (dst && dst_size == sizeof(_simgui)) {
        memcpy(dst, &_simgui, sizeof(_simgui));
    }
}

GSPLAT_HOTRELOAD_EXPORT void gsplat_simgui_state_load(const void* src, size_t src_size) {
    if (src && src_size == sizeof(_simgui)) {
        memcpy(&_simgui, src, sizeof(_simgui));
    }
}
