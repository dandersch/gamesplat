#include "gaussian.h"
#include "maths.h"
#include "miniz.h"
#include "sj.h"
#include <webp/decode.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cctype>

#include "gaussian_loader.h"

static bool str_ends_with_ci(const char* s, const char* suffix) {
    size_t sl = strlen(s);
    size_t tl = strlen(suffix);
    if (tl > sl) return false;
    s += sl - tl;
    for (size_t i = 0; i < tl; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i])) return false;
    }
    return true;
}

bool load_gaussian_scene(const char* path, GaussianScene* scene) {
    if (str_ends_with_ci(path, ".ply")) {
        return gaussian_load_ply(path, scene);
    }
    if (str_ends_with_ci(path, ".sog")) {
        return gaussian_load_sog(path, scene);
    }

    LOG(ERROR|GAUSSIAN|LOAD, "Unsupported gaussian scene format: %s", path);
    return false;
}

void free_scene(GaussianScene* scene) {
    free(scene->gaussians);
    free(scene->visible_indices);
    free(scene->visible_depths);
    free(scene->sorted_indices);
    free(scene->scratch_indices);
    free(scene->scratch_keys);
    free(scene->scratch_keys2);
    memset(scene, 0, sizeof(GaussianScene));
}

GpuGaussian* pack_gpu_gaussians(const GaussianScene* scene) {
    GpuGaussian* gpu = (GpuGaussian*)calloc(scene->gaussian_count, sizeof(GpuGaussian));
    for (uint32_t i = 0; i < scene->gaussian_count; i++) {
        const Gaussian* g = &scene->gaussians[i];
        float* d = gpu[i].data;
        // [0..3] pos.xyz, opacity
        d[0] = g->position[0]; d[1] = g->position[1]; d[2] = g->position[2];
        d[3] = g->opacity;
        // [4..7] scale.xyz, pad
        d[4] = g->scale[0]; d[5] = g->scale[1]; d[6] = g->scale[2];
        // [8..11] rotation w,x,y,z
        d[8] = g->rotation[0]; d[9] = g->rotation[1];
        d[10] = g->rotation[2]; d[11] = g->rotation[3];
        // [12..15] color.rgb (raw DC), pad
        d[12] = g->color[0]; d[13] = g->color[1]; d[14] = g->color[2];
        // [16..60] sh_rest (45 floats)
        for (int k = 0; k < GAUSSIAN_SH_REST_FLOATS; k++) {
            d[16 + k] = g->sh_rest[k];
        }
    }
    return gpu;
}

// --- Culling ---

void cull_gaussians(GaussianScene* scene, const float* view, const float* proj, float ortho_blend) {
    scene->visible_count = 0;

    for (uint32_t i = 0; i < scene->gaussian_count; i++) {
        float p_view[3];
        math_mat4_transform_point(view, scene->gaussians[i].position, p_view);

        // Near-plane cull: camera looks -Z, visible objects have z < 0
        if (p_view[2] > -0.2f) continue;

        // Frustum cull: project to NDC and check with margin
        // Lerp between perspective division and no division
        float inv_z = -1.0f / p_view[2];
        float ndc_x = (proj[0] * p_view[0]) * inv_z * (1.0f - ortho_blend) + proj[0] * p_view[0] * ortho_blend;
        float ndc_y = (proj[5] * p_view[1]) * inv_z * (1.0f - ortho_blend) + proj[5] * p_view[1] * ortho_blend;

        float abs_ndc_x = ndc_x < 0 ? -ndc_x : ndc_x;
        float abs_ndc_y = ndc_y < 0 ? -ndc_y : ndc_y;
        if (abs_ndc_x > 1.3f) continue;
        if (abs_ndc_y > 1.3f) continue;

        scene->visible_indices[scene->visible_count] = i;
        scene->visible_depths[scene->visible_count] = -p_view[2]; // positive, larger = farther
        scene->visible_count++;
    }
}

// --- Radix Sort ---

static uint32_t float_to_sortable(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    // If sign bit set, flip all bits; else flip only sign bit
    if (bits & 0x80000000u)
        return ~bits;
    else
        return bits ^ 0x80000000u;
}

void sort_gaussians(SortContext* ctx) {
    if (ctx->count == 0) return;

    // Convert depths to sortable keys
    uint32_t* keys_a = ctx->scratch_keys;
    uint32_t* keys_b = ctx->scratch_keys2;

    uint32_t* idx_a = ctx->sorted_indices;
    uint32_t* idx_b = ctx->scratch_indices;

    // Initialize
    for (uint32_t i = 0; i < ctx->count; i++) {
        keys_a[i] = float_to_sortable(ctx->depths[i]);
        idx_a[i] = ctx->input_indices[i];
    }

    // 4-pass 8-bit radix sort (LSB first)
    for (int pass = 0; pass < 4; pass++) {
        int shift = pass * 8;

        // Count
        uint32_t count[256];
        memset(count, 0, sizeof(count));
        for (uint32_t i = 0; i < ctx->count; i++) {
            uint8_t bucket = (keys_a[i] >> shift) & 0xFF;
            count[bucket]++;
        }

        // Prefix sum
        uint32_t total = 0;
        for (int b = 0; b < 256; b++) {
            uint32_t c = count[b];
            count[b] = total;
            total += c;
        }

        // Scatter
        for (uint32_t i = 0; i < ctx->count; i++) {
            uint8_t bucket = (keys_a[i] >> shift) & 0xFF;
            uint32_t dst = count[bucket]++;
            keys_b[dst] = keys_a[i];
            idx_b[dst] = idx_a[i];
        }

        // Swap
        uint32_t* tmp;
        tmp = keys_a; keys_a = keys_b; keys_b = tmp;
        tmp = idx_a; idx_a = idx_b; idx_b = tmp;
    }

    // After 4 passes, result is in keys_a/idx_a (ascending order)
    // We need back-to-front = descending depth, so reverse
    if (idx_a != ctx->sorted_indices) {
        // Result ended up in scratch; reverse-copy to sorted_indices
        for (uint32_t i = 0; i < ctx->count; i++) {
            ctx->sorted_indices[i] = idx_a[ctx->count - 1 - i];
        }
    } else {
        // Result is already in sorted_indices; reverse in place
        for (uint32_t i = 0; i < ctx->count / 2; i++) {
            uint32_t tmp_val = ctx->sorted_indices[i];
            ctx->sorted_indices[i] = ctx->sorted_indices[ctx->count - 1 - i];
            ctx->sorted_indices[ctx->count - 1 - i] = tmp_val;
        }
    }
}
