#include "mesh.h"
#include "maths.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <map>

#include "tiny_obj_loader.h"
#include "stb_image.h"
#include "cgltf.h"

static bool mesh_load_obj(const char* obj_path, Mesh* mesh) {
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

// Load a single glTF image into an RGBA8 MeshTexture. Returns true on success.
// Tries (in order): buffer_view (embedded) -> data: URI -> file URI relative to gltf_dir.
static bool gltf_load_image(const cgltf_image* img,
                            const std::string& gltf_dir,
                            MeshTexture* out_tex) {
    int w = 0, h = 0, channels = 0;
    uint8_t* pixels = NULL;

    if (img->buffer_view) {
        const uint8_t* src = cgltf_buffer_view_data(img->buffer_view);
        if (!src) return false;
        pixels = stbi_load_from_memory(src, (int)img->buffer_view->size,
                                       &w, &h, &channels, 4);
    } else if (img->uri) {
        // data: URI? base64-decode then run through stb_image
        if (strncmp(img->uri, "data:", 5) == 0) {
            const char* comma = strchr(img->uri, ',');
            if (!comma) return false;
            // The size we pass is just an upper bound; cgltf will allocate exactly
            // what's needed.
            cgltf_options opts = {};
            void* decoded = NULL;
            cgltf_size decoded_size = strlen(comma + 1);
            cgltf_result r = cgltf_load_buffer_base64(&opts, decoded_size,
                                                     comma + 1, &decoded);
            if (r != cgltf_result_success || !decoded) return false;
            pixels = stbi_load_from_memory((const uint8_t*)decoded,
                                           (int)decoded_size,
                                           &w, &h, &channels, 4);
            free(decoded);
        } else {
            // Relative file URI. cgltf_decode_uri operates in place; copy first.
            std::string uri_copy = img->uri;
            cgltf_decode_uri(&uri_copy[0]);
            uri_copy.resize(strlen(uri_copy.c_str()));
            std::string full = gltf_dir + uri_copy;
            pixels = stbi_load(full.c_str(), &w, &h, &channels, 4);
        }
    }

    if (!pixels) {
        LOG(WARN|MESH|LOAD, "Could not load glTF image '%s': %s",
            img->uri ? img->uri : (img->name ? img->name : "<embedded>"),
            stbi_failure_reason());
        return false;
    }

    out_tex->w = (uint32_t)w;
    out_tex->h = (uint32_t)h;
    uint32_t tex_size = (uint32_t)(w * h * 4);
    out_tex->rgba = (uint8_t*)malloc(tex_size);
    memcpy(out_tex->rgba, pixels, tex_size);
    stbi_image_free(pixels);
    return true;
}

static bool mesh_load_gltf(const char* gltf_path, Mesh* mesh) {
    memset(mesh, 0, sizeof(*mesh));

    cgltf_options options = {};
    cgltf_data* data = NULL;

    cgltf_result res = cgltf_parse_file(&options, gltf_path, &data);
    if (res != cgltf_result_success) {
        LOG(ERROR|MESH|PARSE, "glTF parse failed (%d): %s", (int)res, gltf_path);
        return false;
    }

    res = cgltf_load_buffers(&options, data, gltf_path);
    if (res != cgltf_result_success) {
        LOG(ERROR|MESH|LOAD, "glTF buffer load failed (%d): %s", (int)res, gltf_path);
        cgltf_free(data);
        return false;
    }

    res = cgltf_validate(data);
    if (res != cgltf_result_success) {
        LOG(ERROR|MESH|PARSE, "glTF validation failed (%d): %s", (int)res, gltf_path);
        cgltf_free(data);
        return false;
    }

    // Directory of the gltf file (for relative image URIs).
    std::string gltf_str(gltf_path);
    std::string gltf_dir;
    {
        size_t last_slash = gltf_str.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            gltf_dir = gltf_str.substr(0, last_slash + 1);
        }
    }

    // Lazy-load images on first use, indexed by image pointer.
    // image_to_texture[image*] = index into loaded_textures, or -1 if load failed.
    std::map<const cgltf_image*, int32_t> image_to_texture;
    std::vector<MeshTexture> loaded_textures;

    auto get_or_load_texture = [&](const cgltf_material* mat) -> int32_t {
        if (!mat || !mat->has_pbr_metallic_roughness) return -1;
        const cgltf_texture* tex = mat->pbr_metallic_roughness.base_color_texture.texture;
        if (!tex) return -1;
        // Prefer KHR_texture_basisu / EXT_texture_webp images if present? stb_image
        // can't decode KTX2/Basis, so fall back to the standard image.
        const cgltf_image* img = tex->image;
        if (!img) return -1;

        auto it = image_to_texture.find(img);
        if (it != image_to_texture.end()) return it->second;

        MeshTexture t = {};
        if (!gltf_load_image(img, gltf_dir, &t)) {
            image_to_texture[img] = -1;
            return -1;
        }
        int32_t idx = (int32_t)loaded_textures.size();
        loaded_textures.push_back(t);
        image_to_texture[img] = idx;
        return idx;
    };

    // Build vertex/index buffers by walking the scene's node tree and applying
    // each node's world transform to its primitives.
    std::vector<float> verts;          // interleaved pos3 + uv2
    std::vector<uint32_t> indices;
    // Bucket triangle indices by texture_id so submeshes can share GPU state.
    std::map<int32_t, std::vector<uint32_t>> buckets;

    // Recursive node walker (lambda needs std::function for self-reference).
    // Avoid std::function dep by hand-rolling a stack.
    std::vector<const cgltf_node*> stack;
    if (data->scene) {
        for (cgltf_size i = 0; i < data->scene->nodes_count; ++i) {
            stack.push_back(data->scene->nodes[i]);
        }
    } else {
        // No default scene: walk all root nodes.
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (!data->nodes[i].parent) stack.push_back(&data->nodes[i]);
        }
    }

    while (!stack.empty()) {
        const cgltf_node* node = stack.back();
        stack.pop_back();

        for (cgltf_size i = 0; i < node->children_count; ++i) {
            stack.push_back(node->children[i]);
        }

        if (!node->mesh) continue;

        float world[16];
        cgltf_node_transform_world(node, world);

        for (cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi) {
            const cgltf_primitive* prim = &node->mesh->primitives[pi];
            if (prim->type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* pos_acc = NULL;
            const cgltf_accessor* uv_acc  = NULL;
            for (cgltf_size ai = 0; ai < prim->attributes_count; ++ai) {
                const cgltf_attribute* a = &prim->attributes[ai];
                if (a->type == cgltf_attribute_type_position && !pos_acc) {
                    pos_acc = a->data;
                } else if (a->type == cgltf_attribute_type_texcoord && !uv_acc) {
                    uv_acc = a->data;
                }
            }
            if (!pos_acc) continue;

            uint32_t base_vertex = (uint32_t)(verts.size() / 5);
            cgltf_size vcount = pos_acc->count;

            for (cgltf_size vi = 0; vi < vcount; ++vi) {
                float p[3] = {0, 0, 0};
                cgltf_accessor_read_float(pos_acc, vi, p, 3);
                float wp[3];
                math_mat4_transform_point(world, p, wp);

                // Negate Y to match renderer's Y-down convention (same as OBJ loader).
                verts.push_back(wp[0]);
                verts.push_back(-wp[1]);
                verts.push_back(wp[2]);

                if (uv_acc) {
                    float uv[2] = {0, 0};
                    cgltf_accessor_read_float(uv_acc, vi, uv, 2);
                    // glTF UVs already have origin at top-left; no V-flip needed.
                    verts.push_back(uv[0]);
                    verts.push_back(uv[1]);
                } else {
                    verts.push_back(0.0f);
                    verts.push_back(0.0f);
                }
            }

            int32_t tex_id = get_or_load_texture(prim->material);
            auto& bucket = buckets[tex_id];

            if (prim->indices) {
                cgltf_size icount = prim->indices->count;
                for (cgltf_size ii = 0; ii < icount; ++ii) {
                    cgltf_size idx = cgltf_accessor_read_index(prim->indices, ii);
                    bucket.push_back(base_vertex + (uint32_t)idx);
                }
            } else {
                // Non-indexed: synthesize sequential indices.
                for (cgltf_size ii = 0; ii < vcount; ++ii) {
                    bucket.push_back(base_vertex + (uint32_t)ii);
                }
            }
        }
    }

    // Concatenate buckets into a single index buffer with one submesh per bucket.
    std::vector<MeshSubmesh> submeshes;
    submeshes.reserve(buckets.size());
    for (auto& kv : buckets) {
        auto& bucket = kv.second;
        if (bucket.empty()) continue;

        MeshSubmesh sm = {};
        sm.index_offset = (uint32_t)indices.size();
        sm.index_count  = (uint32_t)bucket.size();
        sm.texture_id   = kv.first;
        submeshes.push_back(sm);

        indices.insert(indices.end(), bucket.begin(), bucket.end());
    }

    mesh->vertex_count = (uint32_t)(verts.size() / 5);
    mesh->index_count  = (uint32_t)indices.size();

    if (mesh->vertex_count == 0 || mesh->index_count == 0) {
        LOG(ERROR|MESH|PARSE, "glTF has no triangle data: %s", gltf_path);
        // Free any textures we already loaded.
        for (auto& t : loaded_textures) free(t.rgba);
        cgltf_free(data);
        return false;
    }

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

    LOG(INFO|MESH|LOAD, "Loaded glTF: %u verts, %u indices, %zu materials, %u textures, %u submeshes",
        mesh->vertex_count, mesh->index_count, data->materials_count,
        mesh->texture_count, mesh->submesh_count);

    cgltf_free(data);
    return true;
}

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
