#include "mesh_loader.h"
#include "cgltf.h"
#include "stb_image.h"

// Load a single glTF image into an RGBA8 MeshTexture. Returns true on success.
// Tries (in order): buffer_view (embedded) -> data: URI -> file URI relative to gltf_dir.
static bool gltf_load_image(const cgltf_image* img, const std::string& gltf_dir, MeshTexture* out_tex) {
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
    out_tex->rgba = (uint8_t*)SDL_malloc(tex_size);
    memcpy(out_tex->rgba, pixels, tex_size);
    stbi_image_free(pixels);
    return true;
}

bool mesh_load_gltf(const char* gltf_path, Mesh* mesh) {
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

    mesh->vertices = (float*)SDL_malloc(verts.size() * sizeof(float));
    memcpy(mesh->vertices, verts.data(), verts.size() * sizeof(float));

    mesh->indices = (uint32_t*)SDL_malloc(indices.size() * sizeof(uint32_t));
    memcpy(mesh->indices, indices.data(), indices.size() * sizeof(uint32_t));

    mesh->submesh_count = (uint32_t)submeshes.size();
    mesh->submeshes = (MeshSubmesh*)SDL_malloc(submeshes.size() * sizeof(MeshSubmesh));
    memcpy(mesh->submeshes, submeshes.data(), submeshes.size() * sizeof(MeshSubmesh));

    mesh->texture_count = (uint32_t)loaded_textures.size();
    if (!loaded_textures.empty()) {
        mesh->textures = (MeshTexture*)SDL_malloc(loaded_textures.size() * sizeof(MeshTexture));
        memcpy(mesh->textures, loaded_textures.data(), loaded_textures.size() * sizeof(MeshTexture));
    }

    LOG(INFO|MESH|LOAD, "Loaded glTF: %u verts, %u indices, %zu materials, %u textures, %u submeshes",
        mesh->vertex_count, mesh->index_count, data->materials_count,
        mesh->texture_count, mesh->submesh_count);

    cgltf_free(data);
    return true;
}

