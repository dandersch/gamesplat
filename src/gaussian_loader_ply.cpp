#include "gaussian_loader.h"
#include <cstdint>
#include <cstdio>
#include "maths.h"
#include "gaussian.h"

struct PlyProperty {
    char name[64];
    int  byte_size; // 4 for float/int, etc.
    int  offset;    // byte offset within vertex
};

bool gaussian_load_ply(const char* path, GaussianScene* scene) {
    FILE* f = fopen(path, "rb");
    if (!f) { LOG(ERROR|GAUSSIAN|IO, "Failed to open %s", path); return false; }

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
                LOG(ERROR|GAUSSIAN|PARSE, "Only binary_little_endian PLY supported");
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
        LOG(ERROR|GAUSSIAN|PARSE, "Invalid PLY header");
        fclose(f); return false;
    }

    int stride = current_offset;
    LOG(INFO|GAUSSIAN|PARSE, "PLY: %u vertices, stride %d bytes, %d properties", vertex_count, stride, prop_count);

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
        LOG(ERROR|GAUSSIAN|PARSE, "Missing position properties");
        fclose(f); return false;
    }

    // Read all vertex data
    uint8_t* raw = (uint8_t*)malloc((size_t)vertex_count * stride);
    if (!raw) { fclose(f); return false; }
    size_t read = fread(raw, stride, vertex_count, f);
    fclose(f);
    if (read != vertex_count) {
        LOG(ERROR|GAUSSIAN|IO, "Short read: got %zu of %u vertices", read, vertex_count);
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
            g->opacity = math_sigmoid(op);
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

    LOG(INFO|GAUSSIAN|PARSE, "PLY: found %d/45 f_rest_* coefficients (SH degree %s)",
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

