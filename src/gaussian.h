#pragma once
#include <cstdint>

// SH degree 3 = 16 coefficients per channel. We store DC (degree 0) separately
// in `color`, and the remaining 15 per-channel coefficients in `sh_rest`,
// laid out per-coefficient as RGB triples: (k0R,k0G,k0B, k1R,k1G,k1B, ...).
#define GAUSSIAN_SH_REST_FLOATS 45

struct Gaussian {
    float position[3];
    float scale[3];
    float rotation[4]; // w, x, y, z
    float color[3];    // raw f_dc_0..2 (no SH_C0/bias applied; shader handles it)
    float opacity;
    float sh_rest[GAUSSIAN_SH_REST_FLOATS]; // raw f_rest, reordered to RGB triples per coeff
};

// GPU layout (std430, 64 floats / 256 bytes per gaussian):
//   [0..2]   position           [3]      opacity
//   [4..6]   scale              [7]      pad
//   [8..11]  rotation (w,x,y,z)
//   [12..14] color (raw DC)     [15]     pad
//   [16..60] sh_rest (45 floats, RGB triples per coefficient)
//   [61..63] pad
#define GPU_GAUSSIAN_FLOATS 64
struct GpuGaussian {
    float data[GPU_GAUSSIAN_FLOATS];
};

struct SortContext {
    const float*    depths;
    const uint32_t* input_indices;
    uint32_t        count;
    uint32_t*       sorted_indices;
    uint32_t*       scratch_indices;
    uint32_t*       scratch_keys;
    uint32_t*       scratch_keys2;
};

struct GaussianScene {
    Gaussian*  gaussians;
    uint32_t   gaussian_count;

    // Per-frame scratch (all sized to gaussian_count)
    uint32_t*  visible_indices;
    float*     visible_depths;
    uint32_t   visible_count;
    uint32_t*  sorted_indices;
    uint32_t*  scratch_indices;
    uint32_t*  scratch_keys;
    uint32_t*  scratch_keys2;
};

bool load_gaussian_scene(const char* path, GaussianScene* scene); // dispatches by file extension (.ply / .sog)

void free_scene(GaussianScene* scene);

// Pack gaussians into GpuGaussian layout. Caller frees returned pointer.
GpuGaussian* pack_gpu_gaussians(const GaussianScene* scene);

// Cull and record depth. Fills visible_indices/depths/count.
void cull_gaussians(GaussianScene* scene, const float* view_matrix, const float* proj_matrix, float ortho_blend = 0.0f);

// Radix sort, back-to-front.
void sort_gaussians(SortContext* ctx);
