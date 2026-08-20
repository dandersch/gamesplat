#pragma once
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

struct SplatDiagnostics {
    uint32_t total_quad_kpix;       // sum of projected bbox area / 1024
    uint32_t max_quad_px;           // largest projected bbox area in pixels
    uint32_t splats_over_1k_px;     // projected bbox area > 1K pixels
    uint32_t splats_over_16k_px;    // projected bbox area > 16K pixels
    uint32_t total_gps_kpoints;     // estimated Gaussian Point Splatting points / 1024
    uint32_t max_gps_points;        // largest estimated GPS point count
    uint32_t gps_splats_over_1k_points;
    uint32_t gps_splats_over_16k_points;
    bool     valid;
};

struct GpsCapabilities {
    bool compute;
    bool storage_images;
    bool shader_int64;
    bool atomic_min_64;
    bool supported;
};

// Shader-only splat effect parameters. The vec4-style layout mirrors the
// SplatEffectUBO in shaders/splat.glsl and keeps uniform packing simple:
// center_radius.xyz = effect center, .w = scene radius;
// params = (elapsed, duration, strength, active); color = tint.rgb + strength.
struct SplatEffectParams {
    float center_radius[4];
    float params[4];
    float color[4];
};

enum class SplatRenderMode {
    AlphaBlendSorted = 0,
    StochasticSplats = 1,
    // Placeholder for the compute/atomic Gaussian Point Splatting backend.
    // Not exposed in the UI until the pipeline exists.
    GaussianPointSplatting = 2,
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

// Reserved resource bucket for the future Gaussian Point Splatting backend.
// GPS will use compute passes rather than the current quad fragment pipeline:
// per-Gaussian work sizing -> distributed point work -> atomic point buffer ->
// resolved stochastic color/depth sample consumed by the existing accumulation.
struct GaussianPointSplatGpu {
    sg_buffer depth_key_buffer;
    sg_view   depth_key_buffer_view;
    sg_buffer color_buffer;
    sg_view   color_buffer_view;
    sg_buffer point_offset_buffer;
    sg_view   point_offset_buffer_view;
    sg_buffer point_work_buffer;
    sg_view   point_work_buffer_view;
    sg_image  atomic_color_depth_image;
    sg_view   atomic_color_depth_storage_view;
    sg_image  resolved_color_image;
    sg_view   resolved_color_view;
    sg_view   resolved_color_texture_view;
    sg_image  resolved_depth_image;
    sg_view   resolved_depth_view;
    sg_view   resolved_depth_texture_view;
    uint32_t  width;
    uint32_t  height;
    uint32_t  supersample_factor;
    uint32_t  max_work_items;
};

struct Renderer {
    SplatRenderMode splat_render_mode;
    sg_pipeline     splat_pipeline;
    sg_pipeline     splat_stochastic_pipeline;
    sg_pipeline     cull_pipeline;
    sg_pipeline     cull_reset_pipeline;
    sg_pipeline     radix_hist_pipeline;
    sg_pipeline     radix_prefix_pipeline;
    sg_pipeline     radix_scatter_pipeline;
    sg_pipeline     gps_clear_pipeline;
    sg_pipeline     gps_expand_pipeline;
    sg_pipeline     gps_splat_pipeline;
    sg_pipeline     gps_resolve_pipeline;
    sg_pipeline     accum_pipeline;
    sg_pipeline     taa_accum_pipeline;
    sg_pipeline     blit_pipeline;
    sg_pipeline     overlay_pipeline;
    sg_pipeline     darken_pipeline;     // fullscreen dim of FPS view behind map overlay
    sg_pipeline     wireframe_pipeline;
    sg_pipeline     mesh_pipeline;
    sg_sampler      overlay_sampler;
    // Gaussian data is stored in a readonly storage buffer. Layout matches
    // GpuGaussian byte-for-byte.
    sg_buffer       gaussian_buffer;
    sg_view         gaussian_buffer_view;
    sg_buffer       depth_key_buffer;
    sg_view         depth_key_buffer_view;
    sg_buffer       sort_temp_key_buffer;
    sg_view         sort_temp_key_buffer_view;
    sg_buffer       sort_temp_index_buffer;
    sg_view         sort_temp_index_buffer_view;
    sg_buffer       sort_histogram_buffer;
    sg_view         sort_histogram_buffer_view;
    sg_buffer       projected_splat_buffer;
    sg_view         projected_splat_buffer_view;
    sg_buffer       visible_count_buffer;
    sg_view         visible_count_buffer_view;
    sg_buffer       splat_diagnostics_buffer;
    sg_view         splat_diagnostics_buffer_view;
    sg_sampler      accum_sampler;
    sg_buffer       cube_vertex_buffer;
    sg_buffer       cube_index_buffer;
    MeshGpu         mesh_gpu;               // primary animated mesh (--mesh)
    MeshGpu         object_gpu;             // static scene object (--object)
    sg_sampler      mesh_sampler;
    GaussianPointSplatGpu gps_gpu;
    MeshTransform   mesh_transform;         // translation/rotation/scale applied each frame to mesh_gpu
    MeshTransform   object_transform;       // applied each frame to object_gpu (identity by default)
    sg_buffer       unsorted_index_buffer;  // GPU-written per-instance visible-index storage buffer
    sg_view         unsorted_index_buffer_view;
    uint32_t        sort_capacity;          // allocated depth/id sort capacity
    uint32_t        sort_group_count;
    uint32_t        gaussian_count;
    bool            splat_diagnostics_enabled;
    SplatDiagnostics splat_diagnostics;
    GpsCapabilities gps_capabilities;
    // SH degree cap (0..3) applied in the cull compute shader; lower degrees
    // skip the corresponding sh_rest fetches and basis-term ALU.
    int             sh_degree;
    // View-space distance beyond which splats use SH degree 0 (DC only).
    // <= 0 disables distance LOD.
    float           sh_lod_distance;

    sg_image        stochastic_sample_image;
    sg_view         stochastic_sample_color_view;
    sg_view         stochastic_sample_texture_view;
    sg_image        stochastic_depth_image;
    sg_view         stochastic_depth_view;
    sg_view         stochastic_depth_texture_view;
    sg_image        stochastic_accum_images[2];
    sg_view         stochastic_accum_color_views[2];
    sg_view         stochastic_accum_texture_views[2];
    sg_image        stochastic_taa_xyz_images[2];
    sg_view         stochastic_taa_xyz_color_views[2];
    sg_view         stochastic_taa_xyz_texture_views[2];
    sg_image        stochastic_frame_avg_images[2];
    sg_view         stochastic_frame_avg_color_views[2];
    sg_view         stochastic_frame_avg_texture_views[2];
    uint32_t        stochastic_width;
    uint32_t        stochastic_height;
    uint32_t        stochastic_sample_count;
    uint32_t        stochastic_samples_per_frame;
    uint32_t        stochastic_taa_current_samples;
    uint32_t        gps_supersample_factor;
    uint32_t        gps_max_work_items;
    uint32_t        stochastic_frame_seed;
    uint32_t        stochastic_accum_write_index;
    CameraUniforms  stochastic_prev_cam;
    SplatEffectParams stochastic_prev_effect;
    float           stochastic_taa_prev_view_proj[16];
    bool            stochastic_prev_effect_valid;
    bool            stochastic_prev_cam_valid;
    bool            stochastic_accumulation_enabled;
    bool            stochastic_taa_enabled;
    bool            stochastic_taa_prev_view_proj_valid;
};

bool renderer_init(Renderer* r);
void renderer_upload_gaussians(Renderer* r, const GaussianScene* scene);
bool renderer_upload_mesh(Renderer* r, const Mesh* mesh);
// Upload a second, static mesh that is drawn alongside the primary mesh with
// an identity model transform. Intended for scene props supplied via --object.
bool renderer_upload_object_mesh(Renderer* r, const Mesh* mesh);
// Recreate shader-backed pipeline objects from the currently-loaded generated
// shader descriptors. Intended for DLL hot reload after sokol-shdc regenerated
// the embedded shader headers and the app code DLL was reloaded.
bool renderer_reload_shaders(Renderer* r);
void renderer_reset_stochastic_accumulation(Renderer* r);
sg_pixel_format renderer_default_color_format(void);
// map_cam is reserved for the top-down overlay; the two-pass map_cam path is
// temporarily disabled in the sokol port (see TODO in renderer.cpp).
void renderer_draw_frame(Renderer* r, GaussianScene* scene, const CameraUniforms* cam, const OverlayParams* overlay, const NodeRenderParams* nodes, const SplatEffectParams* splat_effect = NULL, float wireframe_occlusion = 1.0f, const CameraUniforms* map_cam = NULL);
void renderer_destroy(Renderer* r);
