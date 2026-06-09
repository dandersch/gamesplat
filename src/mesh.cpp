#include "mesh.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mesh_loader.h"
#include "maths.h"

bool mesh_load(const char* path, Mesh* mesh) {
    memset(mesh, 0, sizeof(*mesh));

    // Find extension (case-insensitive).
    const char* dot = strrchr(path, '.');
    if (!dot || !dot[1]) {
        LOG(ERROR|MESH|LOAD, "Unrecognized mesh file extension: %s", path);
        return false;
    }

    char ext[16] = {};
    size_t i = 0;
    for (const char* p = dot + 1; *p && i < sizeof(ext) - 1; ++p, ++i) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        ext[i] = c;
    }

    bool ok = false;
    if (strcmp(ext, "obj") == 0) {
        ok = mesh_load_obj(path, mesh);
    } else if (strcmp(ext, "glb") == 0 || strcmp(ext, "gltf") == 0) {
        ok = mesh_load_gltf(path, mesh);
    } else {
        LOG(ERROR|MESH|LOAD, "Unrecognized mesh file extension: %s", path);
        return false;
    }
    if (!ok) return false;

    // Compute local-space AABB by sweeping interleaved positions
    // (stride 5 floats: vec3 pos + vec2 uv).
    if (mesh->vertex_count > 0 && mesh->vertices) {
        const float* v = mesh->vertices;
        mesh->aabb_min[0] = mesh->aabb_max[0] = v[0];
        mesh->aabb_min[1] = mesh->aabb_max[1] = v[1];
        mesh->aabb_min[2] = mesh->aabb_max[2] = v[2];
        for (uint32_t vi = 1; vi < mesh->vertex_count; ++vi) {
            float x = v[vi*5 + 0], y = v[vi*5 + 1], z = v[vi*5 + 2];
            if (x < mesh->aabb_min[0]) mesh->aabb_min[0] = x;
            if (y < mesh->aabb_min[1]) mesh->aabb_min[1] = y;
            if (z < mesh->aabb_min[2]) mesh->aabb_min[2] = z;
            if (x > mesh->aabb_max[0]) mesh->aabb_max[0] = x;
            if (y > mesh->aabb_max[1]) mesh->aabb_max[1] = y;
            if (z > mesh->aabb_max[2]) mesh->aabb_max[2] = z;
        }
        LOG(INFO|MESH|LOAD, "Mesh AABB: min(%.3f, %.3f, %.3f) max(%.3f, %.3f, %.3f)",
            mesh->aabb_min[0], mesh->aabb_min[1], mesh->aabb_min[2],
            mesh->aabb_max[0], mesh->aabb_max[1], mesh->aabb_max[2]);
    }
    return true;
}

void mesh_aabb_world(const float local_min[3], const float local_max[3],
                     const float model[16], float out_min[3], float out_max[3]) {
    // Transform all 8 corners; column-major matrix multiply (m * v).
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        float c[3] = {
            (i & 1) ? local_max[0] : local_min[0],
            (i & 2) ? local_max[1] : local_min[1],
            (i & 4) ? local_max[2] : local_min[2],
        };
        float wc[3];
        math_mat4_transform_point(model, c, wc);
        float wx = wc[0], wy = wc[1], wz = wc[2];
        if (first) {
            out_min[0] = out_max[0] = wx;
            out_min[1] = out_max[1] = wy;
            out_min[2] = out_max[2] = wz;
            first = false;
        } else {
            if (wx < out_min[0]) out_min[0] = wx;
            if (wx > out_max[0]) out_max[0] = wx;
            if (wy < out_min[1]) out_min[1] = wy;
            if (wy > out_max[1]) out_max[1] = wy;
            if (wz < out_min[2]) out_min[2] = wz;
            if (wz > out_max[2]) out_max[2] = wz;
        }
    }
}

void mesh_free(Mesh* mesh) {
    free(mesh->vertices);
    free(mesh->indices);
    free(mesh->submeshes);
    if (mesh->textures) {
        for (uint32_t i = 0; i < mesh->texture_count; ++i) {
            free(mesh->textures[i].rgba);
        }
        free(mesh->textures);
    }
    memset(mesh, 0, sizeof(*mesh));
}
