#include "gaussian.h"
#include "miniz.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cctype>

// --- PLY Parser ---

static float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

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

struct SogArchiveFile {
    const char* name;
    void*       data;
    size_t      size;
    bool        required;
};

static void free_sog_archive_files(SogArchiveFile* files, int count) {
    for (int i = 0; i < count; i++) {
        free(files[i].data);
        files[i].data = NULL;
        files[i].size = 0;
    }
}

static bool load_sog(const char* path, GaussianScene* scene) {
    (void)scene;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) {
        fprintf(stderr, "SOG: failed to open zip archive: %s\n", path);
        return false;
    }

    // SOG v2 allows filenames to be chosen by meta.json. This first plumbing
    // step only extracts the conventional splat-transform names; the metadata
    // parser added next will resolve filenames from meta.json before decode.
    SogArchiveFile files[] = {
        { "meta.json",          NULL, 0, true  },
        { "means_l.webp",       NULL, 0, true  },
        { "means_u.webp",       NULL, 0, true  },
        { "scales.webp",        NULL, 0, true  },
        { "quats.webp",         NULL, 0, true  },
        { "sh0.webp",           NULL, 0, true  },
        { "shN_centroids.webp", NULL, 0, false },
        { "shN_labels.webp",    NULL, 0, false },
    };
    const int file_count = (int)(sizeof(files) / sizeof(files[0]));

    bool ok = true;
    for (int i = 0; i < file_count; i++) {
        int index = mz_zip_reader_locate_file(&zip, files[i].name, NULL, 0);
        if (index < 0) {
            if (files[i].required) {
                fprintf(stderr, "SOG: archive missing required file: %s\n", files[i].name);
                ok = false;
                break;
            }
            continue;
        }

        files[i].data = mz_zip_reader_extract_file_to_heap(&zip, files[i].name, &files[i].size, 0);
        if (!files[i].data) {
            fprintf(stderr, "SOG: failed to extract file: %s\n", files[i].name);
            ok = false;
            break;
        }
    }

    mz_zip_reader_end(&zip);
    if (!ok) {
        free_sog_archive_files(files, file_count);
        return false;
    }

    fprintf(stderr, "SOG: extracted bundled archive files from %s\n", path);
    for (int i = 0; i < file_count; i++) {
        if (files[i].data) fprintf(stderr, "SOG:   %s (%zu bytes)\n", files[i].name, files[i].size);
    }
    fprintf(stderr, "SOG: metadata parsing and gaussian decode not implemented yet\n");

    free_sog_archive_files(files, file_count);
    return false;
}

struct PlyProperty {
    char name[64];
    int  byte_size; // 4 for float/int, etc.
    int  offset;    // byte offset within vertex
};

bool load_ply(const char* path, GaussianScene* scene) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Failed to open %s\n", path); return false; }

    // Parse header
    char line[512];
    uint32_t vertex_count = 0;
    PlyProperty props[128];
    int prop_count = 0;
    int current_offset = 0;
    bool in_vertex = false;
    bool header_done = false;

    while (fgets(line, sizeof(line), f)) {
        // Remove trailing newline
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;

        if (strcmp(line, "end_header") == 0) { header_done = true; break; }

        if (strncmp(line, "format ", 7) == 0) {
            if (strstr(line, "binary_little_endian") == NULL) {
                fprintf(stderr, "Only binary_little_endian PLY supported\n");
                fclose(f); return false;
            }
        }

        if (strncmp(line, "element vertex ", 15) == 0) {
            vertex_count = (uint32_t)atoi(line + 15);
            in_vertex = true;
            continue;
        }
        if (strncmp(line, "element ", 8) == 0) {
            in_vertex = false;
            continue;
        }

        if (in_vertex && strncmp(line, "property ", 9) == 0) {
            // "property float x" or "property uchar red"
            char type[32], name[64];
            if (sscanf(line, "property %31s %63s", type, name) == 2) {
                int sz = 0;
                if (strcmp(type, "float") == 0 || strcmp(type, "float32") == 0) sz = 4;
                else if (strcmp(type, "double") == 0 || strcmp(type, "float64") == 0) sz = 8;
                else if (strcmp(type, "uchar") == 0 || strcmp(type, "uint8") == 0) sz = 1;
                else if (strcmp(type, "int") == 0 || strcmp(type, "int32") == 0) sz = 4;
                else if (strcmp(type, "uint") == 0 || strcmp(type, "uint32") == 0) sz = 4;
                else if (strcmp(type, "short") == 0 || strcmp(type, "int16") == 0) sz = 2;
                else if (strcmp(type, "ushort") == 0 || strcmp(type, "uint16") == 0) sz = 2;
                else sz = 4; // fallback

                if (prop_count < 128) {
                    snprintf(props[prop_count].name, 64, "%s", name);
                    props[prop_count].byte_size = sz;
                    props[prop_count].offset = current_offset;
                    prop_count++;
                }
                current_offset += sz;
            }
        }
    }

    if (!header_done || vertex_count == 0) {
        fprintf(stderr, "Invalid PLY header\n");
        fclose(f); return false;
    }

    int stride = current_offset;
    fprintf(stderr, "PLY: %u vertices, stride %d bytes, %d properties\n", vertex_count, stride, prop_count);

    // Find property offsets
    auto find_prop = [&](const char* name) -> int {
        for (int i = 0; i < prop_count; i++)
            if (strcmp(props[i].name, name) == 0) return props[i].offset;
        return -1;
    };

    int off_x = find_prop("x"), off_y = find_prop("y"), off_z = find_prop("z");
    int off_s0 = find_prop("scale_0"), off_s1 = find_prop("scale_1"), off_s2 = find_prop("scale_2");
    int off_r0 = find_prop("rot_0"), off_r1 = find_prop("rot_1"), off_r2 = find_prop("rot_2"), off_r3 = find_prop("rot_3");
    int off_op = find_prop("opacity");
    int off_dc0 = find_prop("f_dc_0"), off_dc1 = find_prop("f_dc_1"), off_dc2 = find_prop("f_dc_2");

    // f_rest_0..f_rest_44 (3DGS PLY layout: 15 coeffs per channel, R then G then B).
    // We support up to SH degree 3 (45 rest coeffs). Lower-degree PLYs leave
    // the missing slots at 0 (no contribution).
    int off_rest[45];
    int rest_count = 0;
    for (int k = 0; k < 45; k++) {
        char name[32];
        snprintf(name, sizeof(name), "f_rest_%d", k);
        off_rest[k] = find_prop(name);
        if (off_rest[k] >= 0) rest_count++;
    }

    if (off_x < 0 || off_y < 0 || off_z < 0) {
        fprintf(stderr, "Missing position properties\n");
        fclose(f); return false;
    }

    // Read all vertex data
    uint8_t* raw = (uint8_t*)malloc((size_t)vertex_count * stride);
    if (!raw) { fclose(f); return false; }
    size_t read = fread(raw, stride, vertex_count, f);
    fclose(f);
    if (read != vertex_count) {
        fprintf(stderr, "Short read: got %zu of %u vertices\n", read, vertex_count);
        free(raw); return false;
    }

    // Allocate scene
    scene->gaussian_count = vertex_count;
    scene->gaussians = (Gaussian*)malloc(vertex_count * sizeof(Gaussian));

    for (uint32_t i = 0; i < vertex_count; i++) {
        uint8_t* v = raw + (size_t)i * stride;
        Gaussian* g = &scene->gaussians[i];

        // Position
        memcpy(&g->position[0], v + off_x, 4);
        memcpy(&g->position[1], v + off_y, 4);
        memcpy(&g->position[2], v + off_z, 4);

        // Scale (apply exp)
        if (off_s0 >= 0) {
            float s0, s1, s2;
            memcpy(&s0, v + off_s0, 4); memcpy(&s1, v + off_s1, 4); memcpy(&s2, v + off_s2, 4);
            g->scale[0] = expf(s0); g->scale[1] = expf(s1); g->scale[2] = expf(s2);
        } else {
            g->scale[0] = g->scale[1] = g->scale[2] = 0.01f;
        }

        // Rotation (normalize quaternion) - PLY order: rot_0=w, rot_1=x, rot_2=y, rot_3=z
        if (off_r0 >= 0) {
            float rw, rx, ry, rz;
            memcpy(&rw, v + off_r0, 4); memcpy(&rx, v + off_r1, 4);
            memcpy(&ry, v + off_r2, 4); memcpy(&rz, v + off_r3, 4);
            float len = sqrtf(rw*rw + rx*rx + ry*ry + rz*rz);
            if (len > 1e-8f) { rw /= len; rx /= len; ry /= len; rz /= len; }
            g->rotation[0] = rw; g->rotation[1] = rx; g->rotation[2] = ry; g->rotation[3] = rz;
        } else {
            g->rotation[0] = 1; g->rotation[1] = g->rotation[2] = g->rotation[3] = 0;
        }

        // Opacity (apply sigmoid)
        if (off_op >= 0) {
            float op;
            memcpy(&op, v + off_op, 4);
            g->opacity = sigmoid(op);
        } else {
            g->opacity = 1.0f;
        }

        // Color: store raw f_dc_0..2; the shader applies SH_C0, adds higher
        // SH bands, biases by +0.5 and clamps. (Storing raw values lets the
        // higher-degree contributions push the color in either direction
        // before the final clamp, which is what produces the saturated
        // view-dependent shading.)
        if (off_dc0 >= 0) {
            memcpy(&g->color[0], v + off_dc0, 4);
            memcpy(&g->color[1], v + off_dc1, 4);
            memcpy(&g->color[2], v + off_dc2, 4);
        } else {
            // Encode mid-grey: SH_C0 * dc + 0.5 = 0.5 → dc = 0
            g->color[0] = g->color[1] = g->color[2] = 0.0f;
        }

        // f_rest: PLY stores all R coeffs (k=0..14), then G (15..29), then B (30..44).
        // Repack into per-coefficient RGB triples for shader-friendly access:
        //   sh_rest[k*3 + 0] = R, sh_rest[k*3 + 1] = G, sh_rest[k*3 + 2] = B
        for (int k = 0; k < 15; k++) {
            float r = 0.0f, gg = 0.0f, b = 0.0f;
            if (off_rest[k]      >= 0) memcpy(&r,  v + off_rest[k],      4);
            if (off_rest[15 + k] >= 0) memcpy(&gg, v + off_rest[15 + k], 4);
            if (off_rest[30 + k] >= 0) memcpy(&b,  v + off_rest[30 + k], 4);
            g->sh_rest[k * 3 + 0] = r;
            g->sh_rest[k * 3 + 1] = gg;
            g->sh_rest[k * 3 + 2] = b;
        }
    }

    fprintf(stderr, "PLY: found %d/45 f_rest_* coefficients (SH degree %s)\n",
            rest_count,
            rest_count >= 45 ? "3" : rest_count >= 24 ? "2" : rest_count >= 9 ? "1" : "0");

    free(raw);

    // Allocate scratch buffers
    scene->visible_indices  = (uint32_t*)malloc(vertex_count * sizeof(uint32_t));
    scene->visible_depths   = (float*)malloc(vertex_count * sizeof(float));
    scene->sorted_indices   = (uint32_t*)malloc(vertex_count * sizeof(uint32_t));
    scene->scratch_indices  = (uint32_t*)malloc(vertex_count * sizeof(uint32_t));
    scene->scratch_keys     = (uint32_t*)malloc(vertex_count * sizeof(uint32_t));
    scene->scratch_keys2    = (uint32_t*)malloc(vertex_count * sizeof(uint32_t));
    scene->visible_count    = 0;

    return true;
}

bool load_gaussian_scene(const char* path, GaussianScene* scene) {
    if (str_ends_with_ci(path, ".ply")) {
        return load_ply(path, scene);
    }
    if (str_ends_with_ci(path, ".sog")) {
        return load_sog(path, scene);
    }

    fprintf(stderr, "Unsupported gaussian scene format: %s\n", path);
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

// Transform point by column-major 4x4 matrix, return xyz
static void mat4_transform_point(const float* m, const float* p, float* out) {
    out[0] = m[0]*p[0] + m[4]*p[1] + m[8]*p[2]  + m[12];
    out[1] = m[1]*p[0] + m[5]*p[1] + m[9]*p[2]  + m[13];
    out[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
}

void cull_gaussians(GaussianScene* scene, const float* view, const float* proj, float ortho_blend) {
    scene->visible_count = 0;

    for (uint32_t i = 0; i < scene->gaussian_count; i++) {
        float p_view[3];
        mat4_transform_point(view, scene->gaussians[i].position, p_view);

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
