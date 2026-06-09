#pragma once
#include <cstdint>

struct MeshTexture {
    uint8_t* rgba;     // RGBA8 pixels
    uint32_t w, h;
};

struct MeshSubmesh {
    uint32_t index_offset;
    uint32_t index_count;
    int32_t  texture_id;   // index into Mesh::textures, -1 if no texture
};

struct Mesh {
    float*       vertices;     // interleaved: vec3 pos + vec2 uv per vertex
    uint32_t*    indices;
    uint32_t     vertex_count;
    uint32_t     index_count;
    MeshSubmesh* submeshes;
    uint32_t     submesh_count;
    MeshTexture* textures;
    uint32_t     texture_count;
    // Local-space axis-aligned bounding box over all vertex positions.
    // Computed in mesh_load. If vertex_count == 0, both are zeroed.
    float        aabb_min[3];
    float        aabb_max[3];
};

// TODO utils.h / maths.h
// Transform an axis-aligned local AABB by a column-major model matrix and
// return the world-space AABB enclosing the 8 transformed corners.
void mesh_aabb_world(const float local_min[3], const float local_max[3],
                     const float model[16], float out_min[3], float out_max[3]);

bool mesh_load(const char* path, Mesh* mesh);  // dispatches by file extension
void mesh_free(Mesh* mesh);
