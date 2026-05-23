#pragma once
#include <SDL3/SDL.h>
#include "sokol_gfx.h"
#include "gaussian.h"
#include "camera.h"
#include "mesh.h"

struct OverlayParams {
    sg_image  texture;             // panorama texture (id == 0 -> no overlay)
    sg_view   texture_view;        // sampled-texture view of `texture`
    float     camera_ray_basis[16];
    float     camera_tan_half_fov[2];
    float     ref_rotation[16];    // mat4, 3x3 in upper-left, Y-flip baked in
    float     alpha;
};

struct MeshTransform {
    float translation[3];
    float rotation_euler[3]; // radians, applied as Z * Y * X (intrinsic)
    float scale;
};

struct NodeRenderParams {
    const float* positions;   // float[3] per node (world-space centers)
    uint32_t     count;
    float        half_size;   // AABB half-extent for the wireframe cubes
};

// Per-mesh GPU resources. The renderer owns one of these for the animated
// `--mesh` slot and one for the static `--object` slot. The pipeline and
// sampler are shared (same vertex format / shading).
struct MeshGpu {
    sg_buffer    vertex_buffer;
    sg_buffer    index_buffer;
    sg_image*    textures;          // one GPU texture per material texture
    sg_view*     texture_views;     // parallel sampled-texture views
    uint32_t     texture_count;
    sg_image     default_texture;        // 1x1 white fallback for submeshes w/o texture
    sg_view      default_texture_view;
    MeshSubmesh* submeshes;         // per-submesh draw ranges + texture id
    uint32_t     submesh_count;
};

struct Renderer {
    SDL_Window*     window;
    sg_pipeline     splat_pipeline;
    sg_pipeline     overlay_pipeline;
    sg_pipeline     darken_pipeline;     // fullscreen dim of FPS view behind map overlay
    sg_pipeline     wireframe_pipeline;
    sg_pipeline     mesh_pipeline;
    sg_sampler      overlay_sampler;
    // Gaussian data is stored in an RGBA32F texture (16 texels per gaussian)
    // instead of a storage buffer; this keeps the splat shader compatible with
    // GLES3 / WebGL2 which has no SSBOs. Layout matches GpuGaussian byte-for-byte.
    sg_image        gaussian_texture;
    sg_view         gaussian_texture_view;
    sg_sampler      gaussian_sampler;
    uint32_t        gaussian_tex_w;
    uint32_t        gaussian_tex_h;
    sg_buffer       cube_vertex_buffer;
    sg_buffer       cube_index_buffer;
    MeshGpu         mesh_gpu;               // primary animated mesh (--mesh)
    MeshGpu         object_gpu;             // static scene object (--object)
    sg_sampler      mesh_sampler;
    MeshTransform   mesh_transform;         // translation/rotation/scale applied each frame to mesh_gpu
    MeshTransform   object_transform;       // applied each frame to object_gpu (identity by default)
    sg_buffer       index_buffer;           // dynamic per-instance sorted-index buffer (stream)
    uint32_t        index_buffer_capacity;  // in elements (uint32_t)
    uint32_t        gaussian_count;
};

bool renderer_init(Renderer* r, SDL_Window* window);
void renderer_upload_gaussians(Renderer* r, const GaussianScene* scene);
bool renderer_upload_mesh(Renderer* r, const Mesh* mesh);
// Upload a second, static mesh that is drawn alongside the primary mesh with
// an identity model transform. Intended for scene props supplied via --object.
bool renderer_upload_object_mesh(Renderer* r, const Mesh* mesh);
// map_cam is reserved for the top-down overlay; the two-pass map_cam path is
// temporarily disabled in the sokol port (see TODO in renderer.cpp).
void renderer_draw_frame(Renderer* r, GaussianScene* scene, const CameraUniforms* cam, const OverlayParams* overlay, const NodeRenderParams* nodes, float wireframe_occlusion = 1.0f, const CameraUniforms* map_cam = NULL);
void renderer_destroy(Renderer* r);
