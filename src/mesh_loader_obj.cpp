#include "mesh_loader.h"
#include <string>
#include <vector>
#include <cstring>
#include "stb_image.h"
#include "tiny_obj_loader.h"

bool mesh_load_obj(const char* obj_path, Mesh* mesh) {
    memset(mesh, 0, sizeof(*mesh));

    // Extract directory from obj_path for MTL/texture lookup
    std::string obj_str(obj_path);
    std::string base_dir;
    size_t last_slash = obj_str.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        base_dir = obj_str.substr(0, last_slash + 1);
    }

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
                               obj_path, base_dir.empty() ? NULL : base_dir.c_str());

    if (!warn.empty()) LOG(WARN|MESH|PARSE, "OBJ warning: %s", warn.c_str());
    if (!err.empty())  LOG(ERROR|MESH|PARSE, "OBJ error: %s", err.c_str());
    if (!ok) return false;

    // Load all material textures up front. material_to_texture[i] = texture_id
    // (index into mesh->textures) or -1 if material i has no usable texture.
    std::vector<int32_t> material_to_texture(materials.size(), -1);
    std::vector<MeshTexture> loaded_textures;

    for (size_t mi = 0; mi < materials.size(); ++mi) {
        const auto& mat = materials[mi];
        if (mat.diffuse_texname.empty()) continue;

        std::string tex_path = base_dir + mat.diffuse_texname;
        int w, h, channels;
        uint8_t* pixels = stbi_load(tex_path.c_str(), &w, &h, &channels, 4);
        if (!pixels) {
            LOG(WARN|MESH|LOAD, "Could not load texture: %s (%s)", tex_path.c_str(), stbi_failure_reason());
            continue;
        }

        MeshTexture tex = {};
        tex.w = (uint32_t)w;
        tex.h = (uint32_t)h;
        uint32_t tex_size = (uint32_t)(w * h * 4);
        tex.rgba = (uint8_t*)malloc(tex_size);
        memcpy(tex.rgba, pixels, tex_size);
        stbi_image_free(pixels);

        material_to_texture[mi] = (int32_t)loaded_textures.size();
        loaded_textures.push_back(tex);
    }

    // Deduplicate vertices: OBJ has separate indices for pos/uv/normal,
    // but the GPU wants a single index buffer into interleaved vertices.
    struct VertexKey {
        int v, vt;
        bool operator<(const VertexKey& o) const {
            if (v != o.v) return v < o.v;
            return vt < o.vt;
        }
    };

    std::vector<float> verts;       // interleaved pos3 + uv2
    std::map<VertexKey, uint32_t> vertex_map;

    auto get_or_add_vertex = [&](const tinyobj::index_t& idx) -> uint32_t {
        VertexKey key = { idx.vertex_index, idx.texcoord_index };
        auto it = vertex_map.find(key);
        if (it != vertex_map.end()) return it->second;

        uint32_t new_idx = (uint32_t)(verts.size() / 5);
        vertex_map[key] = new_idx;

        // Position (negate Y: OBJ is Y-up, renderer is Y-down)
        verts.push_back(attrib.vertices[3 * idx.vertex_index + 0]);
        verts.push_back(-attrib.vertices[3 * idx.vertex_index + 1]);
        verts.push_back(attrib.vertices[3 * idx.vertex_index + 2]);

        // UV (default to 0,0 if missing)
        if (idx.texcoord_index >= 0) {
            verts.push_back(attrib.texcoords[2 * idx.texcoord_index + 0]);
            verts.push_back(1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]); // flip V
        } else {
            verts.push_back(0.0f);
            verts.push_back(0.0f);
        }
        return new_idx;
    };

    // Bucket triangle indices by material id. Key = material id (-1 = no
    // material). Each bucket becomes a submesh with its own texture binding.
    std::map<int, std::vector<uint32_t>> buckets;

    for (auto& shape : shapes) {
        const auto& mesh_data = shape.mesh;
        // Tinyobj triangulates by default, so each face has 3 indices.
        size_t face_count = mesh_data.material_ids.size();
        for (size_t f = 0; f < face_count; ++f) {
            int mat_id = mesh_data.material_ids[f];
            auto& bucket = buckets[mat_id];
            for (int k = 0; k < 3; ++k) {
                bucket.push_back(get_or_add_vertex(mesh_data.indices[f * 3 + k]));
            }
        }
    }

    // Concatenate buckets into a single index buffer with one submesh per bucket.
    std::vector<uint32_t> indices;
    std::vector<MeshSubmesh> submeshes;
    submeshes.reserve(buckets.size());
    for (auto& kv : buckets) {
        int mat_id = kv.first;
        auto& bucket = kv.second;
        if (bucket.empty()) continue;

        MeshSubmesh sm = {};
        sm.index_offset = (uint32_t)indices.size();
        sm.index_count  = (uint32_t)bucket.size();
        sm.texture_id   = (mat_id >= 0 && mat_id < (int)material_to_texture.size())
                          ? material_to_texture[mat_id] : -1;
        submeshes.push_back(sm);

        indices.insert(indices.end(), bucket.begin(), bucket.end());
    }

    mesh->vertex_count = (uint32_t)(verts.size() / 5);
    mesh->index_count  = (uint32_t)indices.size();

    mesh->vertices = (float*)malloc(verts.size() * sizeof(float));
    memcpy(mesh->vertices, verts.data(), verts.size() * sizeof(float));

    mesh->indices = (uint32_t*)malloc(indices.size() * sizeof(uint32_t));
    memcpy(mesh->indices, indices.data(), indices.size() * sizeof(uint32_t));

    mesh->submesh_count = (uint32_t)submeshes.size();
    mesh->submeshes = (MeshSubmesh*)malloc(submeshes.size() * sizeof(MeshSubmesh));
    memcpy(mesh->submeshes, submeshes.data(), submeshes.size() * sizeof(MeshSubmesh));

    mesh->texture_count = (uint32_t)loaded_textures.size();
    if (!loaded_textures.empty()) {
        mesh->textures = (MeshTexture*)malloc(loaded_textures.size() * sizeof(MeshTexture));
        memcpy(mesh->textures, loaded_textures.data(), loaded_textures.size() * sizeof(MeshTexture));
    }

    LOG(INFO|MESH|LOAD, "Loaded OBJ: %u verts, %u indices, %zu materials, %u textures, %u submeshes",
        mesh->vertex_count, mesh->index_count, materials.size(),
        mesh->texture_count, mesh->submesh_count);

    return true;
}
