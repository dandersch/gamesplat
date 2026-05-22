#pragma once
#include <SDL3/SDL.h>
#include "gaussian.h"
#include "camera.h"
#include "mesh.h"

// Number of frames that can be in flight simultaneously
#define MAX_FRAMES_IN_FLIGHT 3

struct OverlayParams {
    SDL_GPUTexture* texture;       // panorama texture to render (NULL = no overlay)
    float           camera_ray_basis[16];
    float           camera_tan_half_fov[2];
    float           ref_rotation[16]; // mat4, 3x3 in upper-left, Y-flip baked in
    float           alpha;
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
    SDL_GPUBuffer*   vertex_buffer;
    SDL_GPUBuffer*   index_buffer;
    SDL_GPUTexture** textures;          // one GPU texture per material texture
    uint32_t         texture_count;
    SDL_GPUTexture*  default_texture;   // 1x1 white fallback for submeshes w/o texture
    MeshSubmesh*     submeshes;         // per-submesh draw ranges + texture id
    uint32_t         submesh_count;
};

struct Renderer {
    SDL_GPUDevice*          device;
    SDL_Window*             window;
    SDL_GPUGraphicsPipeline* splat_pipeline;
    SDL_GPUGraphicsPipeline* overlay_pipeline;
    SDL_GPUGraphicsPipeline* darken_pipeline;     // fullscreen dim of FPS view behind map overlay
    SDL_GPUGraphicsPipeline* wireframe_pipeline;
    SDL_GPUGraphicsPipeline* mesh_pipeline;
    SDL_GPUSampler*         overlay_sampler;
    // Gaussian data is stored in an RGBA32F texture (16 texels per gaussian)
    // instead of a storage buffer; this keeps the splat shader compatible with
    // GLES3 / WebGL2 which has no SSBOs. Layout matches GpuGaussian byte-for-byte.
    SDL_GPUTexture*         gaussian_texture;
    SDL_GPUSampler*         gaussian_sampler;
    uint32_t                gaussian_tex_w;
    uint32_t                gaussian_tex_h;
    SDL_GPUBuffer*          cube_vertex_buffer;
    SDL_GPUBuffer*          cube_index_buffer;
    MeshGpu                 mesh_gpu;               // primary animated mesh (--mesh)
    MeshGpu                 object_gpu;             // static scene object (--object)
    SDL_GPUSampler*         mesh_sampler;
    MeshTransform           mesh_transform;         // translation/rotation/scale applied each frame to mesh_gpu
    MeshTransform           object_transform;       // applied each frame to object_gpu (identity by default)
    SDL_GPUBuffer*          index_buffer;
    SDL_GPUTexture*         depth_texture;
    uint32_t                depth_w, depth_h;
    SDL_GPUTransferBuffer*  transfer_bufs[MAX_FRAMES_IN_FLIGHT];
    SDL_GPUFence*           frame_fences[MAX_FRAMES_IN_FLIGHT];
    uint32_t                current_frame;
    SDL_GPUTextureFormat    swapchain_format;
    uint32_t                gaussian_count;
};

bool renderer_init(Renderer* r, SDL_GPUDevice* device, SDL_Window* window);
void renderer_upload_gaussians(Renderer* r, const GaussianScene* scene);
bool renderer_upload_mesh(Renderer* r, const Mesh* mesh);
// Upload a second, static mesh that is drawn alongside the primary mesh with
// an identity model transform. Intended for scene props supplied via --object.
bool renderer_upload_object_mesh(Renderer* r, const Mesh* mesh);
// map_cam: if non-NULL, a second render pass is appended that draws mesh,
// splats, and wireframe nodes from this camera on top of the FPS frame
// (load_op=LOAD on color, clear depth). Used for the top-down map overlay.
// scene must be non-const because this path runs a second cull+sort.
void renderer_draw_frame(Renderer* r, GaussianScene* scene, const CameraUniforms* cam, const OverlayParams* overlay, const NodeRenderParams* nodes, float wireframe_occlusion = 1.0f, const CameraUniforms* map_cam = NULL);
void renderer_destroy(Renderer* r);
