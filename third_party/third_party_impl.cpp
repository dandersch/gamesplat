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
// Staged behind GSPLAT_USE_SOKOL so the existing SDL_GPU build is unaffected.
// When the renderer port lands this guard is dropped and the impls become
// unconditional. To verify the impls compile cleanly without flipping the
// renderer over yet, build with:
//   GSPLAT_USE_SOKOL=1 ./build.sh
#ifdef GSPLAT_USE_SOKOL
    // Desktop OpenGL backend; same shaders/inputs work on GLES3 (WebGL2)
    // when we re-target for the web build later.
    #define SOKOL_IMPL
    #define SOKOL_GLCORE
    #include "sokol_gfx.h"

    // sokol_imgui.h is the ImGui rendering backend (NOT to be confused with
    // sokol_gfx_imgui.h, which is a debug inspector). We compile as C++ so it
    // talks to the ImGui C++ API directly. SOKOL_IMGUI_NO_SOKOL_APP drops the
    // sokol_app dependency since we use SDL3 for windowing/input.
    #define SOKOL_IMGUI_IMPL
    #define SOKOL_IMGUI_NO_SOKOL_APP
    #include "imgui.h"
    #include "sokol_imgui.h"

    // Include the sokol-shdc generated shader headers so any breakage in the
    // sokol-slang sources surfaces at compile time, not just when the renderer
    // is ported over to consume them. The headers reference sg_* types so they
    // must come after sokol_gfx.h above. Each defines static inline functions
    // (wireframe_shader_desc(), etc.) and per-backend source byte arrays.
    #include "../shaders/wireframe.glsl.h"
    #include "../shaders/overlay.glsl.h"
    #include "../shaders/darken.glsl.h"
    #include "../shaders/mesh.glsl.h"
    #include "../shaders/splat.glsl.h"
#endif
