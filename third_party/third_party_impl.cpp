// Single translation unit that instantiates the implementations of the
// single-header libraries in third_party/. Built once into libthirdparty.a
// and cached so we don't pay the parse cost on every incremental build.

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

// --- sokol_gfx + sokol_imgui ---------------------------------------------
// Desktop OpenGL backend; the same generated shader headers also carry the
// glsl300es flavour so the same sources will work on WebGL2 when we re-target
// for the web build.
#define SOKOL_IMPL
#define SOKOL_GLES3
#include "sokol_gfx.h"

// sokol_imgui.h is the ImGui rendering backend (NOT to be confused with
// sokol_gfx_imgui.h, which is a debug inspector). We compile as C++ so it
// talks to the ImGui C++ API directly. SOKOL_IMGUI_NO_SOKOL_APP drops the
// sokol_app dependency since we use SDL3 for windowing/input.
#define SOKOL_IMGUI_IMPL
#define SOKOL_IMGUI_NO_SOKOL_APP
#include "imgui.h"
#include "sokol_imgui.h"

// Pull the sokol-shdc generated shader headers into the same TU as
// sokol_gfx.h so the *_shader_desc() / SLOT_* / uniform-block-struct symbols
// are linkable from renderer.cpp.
#include "../shaders/wireframe.glsl.h"
#include "../shaders/overlay.glsl.h"
#include "../shaders/darken.glsl.h"
#include "../shaders/mesh.glsl.h"
#include "../shaders/splat.glsl.h"
