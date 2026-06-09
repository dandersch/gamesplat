#include "refview.h"
#include "maths.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "stb_image.h"
#include "sokol_gfx.h"

bool refview_load(RefViewSet* set, const ColmapImageSet* images, const char* image_dir) {
    memset(set, 0, sizeof(*set));
    set->selected = -1;
    set->current_node = -1;
    set->lerp_speed = 2.0f;
    set->neighbor_radius = 5.5f;
    set->min_inliers = 50;
    set->use_covisibility = false;

    if (!images || images->count == 0) {
        LOG(ERROR|REFVIEW|LOAD, "No COLMAP images to load");
        return false;
    }

    snprintf(set->image_dir, sizeof(set->image_dir), "%s", image_dir ? image_dir : "");
    LOG(INFO|REFVIEW|LOAD, "image directory: %s", set->image_dir);

    set->views = (RefView*)calloc(images->count, sizeof(RefView));
    set->count = images->count;

    for (uint32_t i = 0; i < images->count; i++) {
        const ColmapImage* image = &images->images[i];
        RefView* v = &set->views[i];
        v->colmap_id = image->image_id;
        snprintf(v->image_name, sizeof(v->image_name), "%s", image->name);
        memcpy(v->rotation, image->rotation, sizeof(v->rotation));
        v->position[0] = image->position[0];
        v->position[1] = image->position[1];
        v->position[2] = image->position[2];
        v->yaw = image->yaw;
        v->pitch = image->pitch;
    }

    LOG(INFO|REFVIEW|LOAD, "Loaded %u camera nodes", set->count);
    return true;
}

void refview_load_covisibility(RefViewSet* set, const ColmapCovisibility* covis) {
    if (!covis || covis->count == 0) {
        LOG(WARN|REFVIEW|NAV, "No covisibility data; using distance-based neighbors");
        return;
    }

    // Build map from source image_id -> internal index
    int max_colmap_id = 0;
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->views[i].colmap_id > max_colmap_id)
            max_colmap_id = set->views[i].colmap_id;
    }
    int* id_to_idx = (int*)SDL_malloc((max_colmap_id + 1) * sizeof(int));
    memset(id_to_idx, -1, (max_colmap_id + 1) * sizeof(int));
    for (uint32_t i = 0; i < set->count; i++) {
        id_to_idx[set->views[i].colmap_id] = (int)i;
    }

    // First pass: count edges
    uint32_t edge_cap = 64;
    uint32_t edge_count = 0;
    CovisEdge* edges = (CovisEdge*)SDL_malloc(edge_cap * sizeof(CovisEdge));

    for (uint32_t i = 0; i < covis->count; i++) {
        int id1 = covis->edges[i].image_id_a;
        int id2 = covis->edges[i].image_id_b;

        if (id1 < 0 || id1 > max_colmap_id || id2 < 0 || id2 > max_colmap_id) continue;
        int idx1 = id_to_idx[id1];
        int idx2 = id_to_idx[id2];
        if (idx1 < 0 || idx2 < 0) continue;

        if (edge_count == edge_cap) {
            edge_cap *= 2;
            edges = (CovisEdge*)realloc(edges, edge_cap * sizeof(CovisEdge));
        }
        edges[edge_count++] = { (uint32_t)idx1, (uint32_t)idx2, covis->edges[i].inliers };
    }

    free(id_to_idx);

    set->covis_edges = edges;
    set->covis_edge_count = edge_count;
    set->use_covisibility = true;

    LOG(INFO|REFVIEW|NAV, "Loaded %u covisibility edges", edge_count);
}

struct ImageLoadTask {
    char     path[768];
    uint8_t* pixels;  // RGBA8, NULL on failure; free with stbi_image_free
    int      w, h;
};

static int image_load_thread(void* data) {
    ImageLoadTask* task = (ImageLoadTask*)data;
    int channels;
    task->pixels = stbi_load(task->path, &task->w, &task->h, &channels, 4);
    return 0;
}

void refview_load_images(RefViewSet* set) {
#if defined(__EMSCRIPTEN__)
    // The web build does not enable pthreads, so SDL_CreateThread won't run
    // the image decode tasks. Decode serially instead; this also keeps peak
    // wasm heap usage lower for the large equirect PNGs.
    ImageLoadTask task = {};
    uint32_t loaded = 0;

    for (uint32_t i = 0; i < set->count; i++) {
        snprintf(task.path, sizeof(task.path), "%s/%s", set->image_dir, set->views[i].image_name);
        task.pixels = NULL;
        task.w = task.h = 0;
        image_load_thread(&task);

        RefView* v = &set->views[i];
        uint8_t* pixels = task.pixels;
        int img_w = task.w;
        int img_h = task.h;

        if (!pixels) {
            LOG(WARN|REFVIEW|LOAD, "Could not load image %s (%s)", task.path, stbi_failure_reason());
            continue;
        }

        v->width = img_w;
        v->height = img_h;

        sg_image_desc id = {};
        id.type = SG_IMAGETYPE_2D;
        id.width = img_w;
        id.height = img_h;
        id.num_slices = 1;
        id.num_mipmaps = 1;
        id.pixel_format = SG_PIXELFORMAT_RGBA8;
        id.data.mip_levels[0].ptr = pixels;
        id.data.mip_levels[0].size = (size_t)img_w * (size_t)img_h * 4u;
        id.label = "refview-tex";
        v->texture = sg_make_image(&id);
        stbi_image_free(pixels);
        if (v->texture.id == 0) {
            LOG(ERROR|REFVIEW|GPU, "Failed to create sg_image for %s", task.path);
            continue;
        }

        sg_view_desc vd = {};
        vd.texture.image = v->texture;
        vd.label = "refview-tex-view";
        v->texture_view = sg_make_view(&vd);

        loaded++;
    }

    LOG(INFO|REFVIEW|LOAD, "Loaded %u / %u images as sokol_gfx images", loaded, set->count);
#else
    // Decode all images in parallel on separate threads. Uploads happen on
    // the main thread (sokol_gfx isn't thread-safe) but with all pixels
    // already decoded, the per-image work shrinks to a single sg_make_image
    // call -- sokol handles its own staging internally.
    ImageLoadTask* tasks = (ImageLoadTask*)calloc(set->count, sizeof(ImageLoadTask));
    SDL_Thread** threads = (SDL_Thread**)calloc(set->count, sizeof(SDL_Thread*));

    for (uint32_t i = 0; i < set->count; i++) {
        snprintf(tasks[i].path, sizeof(tasks[i].path), "%s/%s", set->image_dir, set->views[i].image_name);
        char name[32];
        snprintf(name, sizeof(name), "img_%u", i);
        threads[i] = SDL_CreateThread(image_load_thread, name, &tasks[i]);
    }

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < set->count; i++) {
        SDL_WaitThread(threads[i], NULL);
        RefView* v = &set->views[i];
        uint8_t* pixels = tasks[i].pixels;
        int img_w = tasks[i].w;
        int img_h = tasks[i].h;

        if (!pixels) {
            LOG(WARN|REFVIEW|LOAD, "Could not load image %s (%s)", tasks[i].path, stbi_failure_reason());
            continue;
        }

        v->width = img_w;
        v->height = img_h;

        sg_image_desc id = {};
        id.type = SG_IMAGETYPE_2D;
        id.width = img_w;
        id.height = img_h;
        id.num_slices = 1;
        id.num_mipmaps = 1;
        id.pixel_format = SG_PIXELFORMAT_RGBA8;
        id.data.mip_levels[0].ptr = pixels;
        id.data.mip_levels[0].size = (size_t)img_w * (size_t)img_h * 4u;
        id.label = "refview-tex";
        v->texture = sg_make_image(&id);
        stbi_image_free(pixels);
        if (v->texture.id == 0) {
            LOG(ERROR|REFVIEW|GPU, "Failed to create sg_image for %s", tasks[i].path);
            continue;
        }

        sg_view_desc vd = {};
        vd.texture.image = v->texture;
        vd.label = "refview-tex-view";
        v->texture_view = sg_make_view(&vd);

        loaded++;
    }

    free(threads);
    free(tasks);

    LOG(INFO|REFVIEW|LOAD, "Loaded %u / %u images as sokol_gfx images", loaded, set->count);
#endif
}

void refview_release_images(RefViewSet* set) {
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->views[i].texture_view.id) {
            sg_destroy_view(set->views[i].texture_view);
            set->views[i].texture_view.id = 0;
        }
        if (set->views[i].texture.id) {
            sg_destroy_image(set->views[i].texture);
            set->views[i].texture.id = 0;
        }
    }
}

bool refview_update(RefViewSet* set, Camera* cam, float dt) {
    if (!set->lerping) return false;
    if (!set->inspect_mode && set->selected < 0) return false;

    set->lerp_t += dt / set->lerp_duration;
    if (set->lerp_t >= 1.0f) {
        set->lerp_t = 1.0f;
        set->lerping = false;
    }

    float t = math_smoothstep01(set->lerp_t);

    if (set->inspect_mode) {
        cam->position[0] = math_lerp(set->start_pos[0], set->inspect_target_pos[0], t);
        cam->position[1] = math_lerp(set->start_pos[1], set->inspect_target_pos[1], t);
        cam->position[2] = math_lerp(set->start_pos[2], set->inspect_target_pos[2], t);

        // Drive the perspective<->ortho blend off the same eased t as the
        // position lerp so both finish together. Decoupled timers (the
        // standalone ramp in main.cpp) caused a visible zoom-in/out wobble
        // when the inspect target was close enough that the perspective
        // magnification at that distance exceeded 1/ortho_size.
        cam->ortho_blend = set->inspect_return ? (1.0f - t) : t;

        // On the return leg, leave yaw/pitch alone so the user can look around
        // freely while we slide back to the source position.
        if (!set->inspect_return) {
            // Shortest-arc yaw lerp (wrap to [-pi, pi]).
            const float PI = 3.14159265358979f;
            float dyaw = set->inspect_target_yaw - set->start_yaw;
            while (dyaw >  PI) dyaw -= 2.0f * PI;
            while (dyaw < -PI) dyaw += 2.0f * PI;
            cam->yaw   = set->start_yaw + dyaw * t;
            cam->pitch = math_lerp(set->start_pitch, set->inspect_target_pitch, t);
        }

        if (!set->lerping) {
            set->inspect_mode = false;
            set->inspect_return = false;
        }
    } else {
        RefView* target = &set->views[set->selected];
        cam->position[0] = math_lerp(set->start_pos[0], target->position[0], t);
        cam->position[1] = math_lerp(set->start_pos[1], target->position[1], t);
        cam->position[2] = math_lerp(set->start_pos[2], target->position[2], t);
    }

    return true;
}

uint32_t refview_get_neighbors(const RefViewSet* set, float* out_positions, uint32_t* out_indices, uint32_t max_count) {
    if (set->current_node < 0 || set->current_node >= (int32_t)set->count) return 0;

    uint32_t n = 0;

    if (set->use_covisibility) {
        // Covisibility-based: walk edges, return neighbors of current_node above threshold
        uint32_t cur = (uint32_t)set->current_node;
        for (uint32_t e = 0; e < set->covis_edge_count && n < max_count; e++) {
            const CovisEdge* edge = &set->covis_edges[e];
            if ((int)edge->inliers < set->min_inliers) continue;
            uint32_t other = UINT32_MAX;
            if (edge->idx_a == cur) other = edge->idx_b;
            else if (edge->idx_b == cur) other = edge->idx_a;
            if (other == UINT32_MAX) continue;

            out_positions[n * 3 + 0] = set->views[other].position[0];
            out_positions[n * 3 + 1] = set->views[other].position[1];
            out_positions[n * 3 + 2] = set->views[other].position[2];
            out_indices[n] = other;
            n++;
        }
    } else {
        // Distance-based fallback
        const RefView* current = &set->views[set->current_node];
        float radius2 = set->neighbor_radius * set->neighbor_radius;
        for (uint32_t i = 0; i < set->count && n < max_count; i++) {
            if ((int32_t)i == set->current_node) continue;
            float dx = set->views[i].position[0] - current->position[0];
            float dy = set->views[i].position[1] - current->position[1];
            float dz = set->views[i].position[2] - current->position[2];
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 <= radius2) {
                out_positions[n * 3 + 0] = set->views[i].position[0];
                out_positions[n * 3 + 1] = set->views[i].position[1];
                out_positions[n * 3 + 2] = set->views[i].position[2];
                out_indices[n] = i;
                n++;
            }
        }
    }
    return n;
}

void refview_get_rotation_matrix(const RefView* v, float* m) {
    memcpy(m, v->rotation, sizeof(v->rotation));
}

void refview_free(RefViewSet* set) {
    if (set->views) {
        for (uint32_t i = 0; i < set->count; i++) {
            hotspot_free_array(set->views[i].hotspots, set->views[i].hotspot_count);
            set->views[i].hotspots = NULL;
            set->views[i].hotspot_count = 0;
        }
    }
    free(set->views);
    free(set->covis_edges);
    memset(set, 0, sizeof(*set));
    set->selected = -1;
}
