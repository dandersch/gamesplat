#include "refview.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include "stb_image.h"
#include "sokol_gfx.h"

#if !defined(__EMSCRIPTEN__)
// let's not bother with adding a wasm build of sqlite since the feature that
// relies on it (building paths between nodes based on image covisibility) is
// questionable and the sqlite lookup could be done in a build step anyway
#include <sqlite3.h>
#endif

// Skip the rest of the current line (handles arbitrarily long POINTS2D lines)
static void skip_line(FILE* f) {
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {}
}

// Convert colmap quaternion (world-to-camera, colmap convention: X-right Y-down Z-forward)
// to world-space camera center and yaw/pitch matching our fly camera convention (Y-up).
static void colmap_to_camera(float qw, float qx, float qy, float qz,
                             float tx, float ty, float tz,
                             float* out_pos, float* out_yaw, float* out_pitch)
{
    // Build rotation matrix R from quaternion (world-to-camera rotation)
    float R[3][3];
    R[0][0] = 1 - 2*(qy*qy + qz*qz);
    R[0][1] = 2*(qx*qy - qw*qz);
    R[0][2] = 2*(qx*qz + qw*qy);
    R[1][0] = 2*(qx*qy + qw*qz);
    R[1][1] = 1 - 2*(qx*qx + qz*qz);
    R[1][2] = 2*(qy*qz - qw*qx);
    R[2][0] = 2*(qx*qz - qw*qy);
    R[2][1] = 2*(qy*qz + qw*qx);
    R[2][2] = 1 - 2*(qx*qx + qy*qy);

    // Camera center in world space: -R^T * T
    out_pos[0] = -(R[0][0]*tx + R[1][0]*ty + R[2][0]*tz);
    out_pos[1] = -(R[0][1]*tx + R[1][1]*ty + R[2][1]*tz);
    out_pos[2] = -(R[0][2]*tx + R[1][2]*ty + R[2][2]*tz);

    // Camera forward in world space: R^T * [0,0,1] (colmap Z-forward)
    // = third column of R^T = third row of R
    float fwd_colmap[3] = { R[2][0], R[2][1], R[2][2] };

    // Colmap: Y-down. Our renderer: Y-up. Negate Y component.
    float fwd[3] = { fwd_colmap[0], -fwd_colmap[1], fwd_colmap[2] };
    out_pos[1] = -out_pos[1]; // also flip camera position Y

    // Derive yaw/pitch from forward vector
    // forward = (cos(pitch)*sin(yaw), sin(pitch), cos(pitch)*cos(yaw))
    *out_pitch = asinf(fwd[1]);
    *out_yaw   = atan2f(fwd[0], fwd[2]);
}

bool refview_load(RefViewSet* set, const char* colmap_dir) {
    memset(set, 0, sizeof(*set));
    set->selected = -1;
    set->current_node = -1;
    set->lerp_speed = 2.0f;
    set->neighbor_radius = 5.5f;
    set->min_inliers = 50;
    set->use_covisibility = false;

    // Build path to images.txt
    char images_txt[512];
    snprintf(images_txt, sizeof(images_txt), "%s/images.txt", colmap_dir);

    FILE* f = fopen(images_txt, "r");
    if (!f) {
        SDL_Log("RefView: Could not open %s", images_txt);
        SDL_Log("RefView: If you only have images.bin, convert with:");
        SDL_Log("  colmap model_converter --input_path %s --output_path %s --output_type TXT", colmap_dir, colmap_dir);
        return false;
    }

    // Derive image directory: colmap_dir/../../images/
    snprintf(set->image_dir, sizeof(set->image_dir), "%s/../../images", colmap_dir);
    SDL_Log("RefView: image directory: %s", set->image_dir);

    // First pass: count images
    uint32_t count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        count++;
        skip_line(f); // skip POINTS2D line (can be very long)
    }

    if (count == 0) {
        SDL_Log("RefView: No images found in %s", images_txt);
        fclose(f);
        return false;
    }

    set->views = (RefView*)calloc(count, sizeof(RefView));
    set->count = count;

    // Second pass: parse
    rewind(f);
    uint32_t idx = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        int image_id, camera_id;
        float qw, qx, qy, qz, tx, ty, tz;
        char name[256] = {};

        int parsed = sscanf(line, "%d %f %f %f %f %f %f %f %d %255s",
                            &image_id, &qw, &qx, &qy, &qz, &tx, &ty, &tz, &camera_id, name);
        if (parsed < 10) {
            SDL_Log("RefView: Failed to parse line: %s", line);
            skip_line(f);
            continue;
        }

        RefView* v = &set->views[idx];
        v->colmap_id = image_id;
        snprintf(v->image_name, sizeof(v->image_name), "%s", name);
        v->rotation[0] = qw;
        v->rotation[1] = qx;
        v->rotation[2] = qy;
        v->rotation[3] = qz;

        colmap_to_camera(qw, qx, qy, qz, tx, ty, tz,
                         v->position, &v->yaw, &v->pitch);

        idx++;
        skip_line(f);
    }

    set->count = idx; // actual parsed count
    fclose(f);

    SDL_Log("RefView: Loaded %u camera nodes from %s", set->count, images_txt);
    return true;
}

void refview_load_covisibility(RefViewSet* set, const char* colmap_dir) {
#if !defined(__EMSCRIPTEN__)
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/../../database/database.db", colmap_dir);

    sqlite3* db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        SDL_Log("RefView: Could not open %s (%s), using distance-based neighbors",
                db_path, db ? sqlite3_errmsg(db) : "unknown error");
        if (db) sqlite3_close(db);
        return;
    }

    // Build map from colmap image_id -> internal index
    int max_colmap_id = 0;
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->views[i].colmap_id > max_colmap_id)
            max_colmap_id = set->views[i].colmap_id;
    }
    int* id_to_idx = (int*)malloc((max_colmap_id + 1) * sizeof(int));
    memset(id_to_idx, -1, (max_colmap_id + 1) * sizeof(int));
    for (uint32_t i = 0; i < set->count; i++) {
        id_to_idx[set->views[i].colmap_id] = (int)i;
    }

    // Query verified pairs
    const char* sql = "SELECT pair_id, rows FROM two_view_geometries WHERE config = 2 AND rows > 0";
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        SDL_Log("RefView: SQL error: %s", sqlite3_errmsg(db));
        free(id_to_idx);
        sqlite3_close(db);
        return;
    }

    // First pass: count edges
    uint32_t edge_cap = 64;
    uint32_t edge_count = 0;
    CovisEdge* edges = (CovisEdge*)malloc(edge_cap * sizeof(CovisEdge));

    const int64_t MAX_ID = 2147483647LL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t pair_id = sqlite3_column_int64(stmt, 0);
        int inliers = sqlite3_column_int(stmt, 1);

        int id1 = (int)(pair_id / MAX_ID);
        int id2 = (int)(pair_id % MAX_ID);

        if (id1 < 0 || id1 > max_colmap_id || id2 < 0 || id2 > max_colmap_id) continue;
        int idx1 = id_to_idx[id1];
        int idx2 = id_to_idx[id2];
        if (idx1 < 0 || idx2 < 0) continue;

        if (edge_count == edge_cap) {
            edge_cap *= 2;
            edges = (CovisEdge*)realloc(edges, edge_cap * sizeof(CovisEdge));
        }
        edges[edge_count++] = { (uint32_t)idx1, (uint32_t)idx2, (uint32_t)inliers };
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    free(id_to_idx);

    set->covis_edges = edges;
    set->covis_edge_count = edge_count;
    set->use_covisibility = true;

    SDL_Log("RefView: Loaded %u covisibility edges from %s", edge_count, db_path);
#endif
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
            SDL_Log("RefView: Could not load image %s", tasks[i].path);
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
            SDL_Log("RefView: Failed to create sg_image for %s", tasks[i].path);
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

    SDL_Log("RefView: Loaded %u / %u images as sokol_gfx images", loaded, set->count);
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

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

// Smooth step for nicer interpolation
static float smoothstep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

bool refview_update(RefViewSet* set, Camera* cam, float dt) {
    if (!set->lerping) return false;
    if (!set->inspect_mode && set->selected < 0) return false;

    set->lerp_t += dt / set->lerp_duration;
    if (set->lerp_t >= 1.0f) {
        set->lerp_t = 1.0f;
        set->lerping = false;
    }

    float t = smoothstep(set->lerp_t);

    if (set->inspect_mode) {
        cam->position[0] = lerpf(set->start_pos[0], set->inspect_target_pos[0], t);
        cam->position[1] = lerpf(set->start_pos[1], set->inspect_target_pos[1], t);
        cam->position[2] = lerpf(set->start_pos[2], set->inspect_target_pos[2], t);

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
            cam->pitch = lerpf(set->start_pitch, set->inspect_target_pitch, t);
        }

        if (!set->lerping) {
            set->inspect_mode = false;
            set->inspect_return = false;
        }
    } else {
        RefView* target = &set->views[set->selected];
        cam->position[0] = lerpf(set->start_pos[0], target->position[0], t);
        cam->position[1] = lerpf(set->start_pos[1], target->position[1], t);
        cam->position[2] = lerpf(set->start_pos[2], target->position[2], t);
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
    // Build world-to-camera rotation from colmap quaternion
    float qw = v->rotation[0], qx = v->rotation[1];
    float qy = v->rotation[2], qz = v->rotation[3];

    // R = colmap world-to-camera rotation (colmap world has Y-down)
    // We need R_adjusted = R * diag(1, -1, 1) to account for our Y-up world
    // This negates column 1 of R
    // Output is column-major mat4

    float R[3][3];
    R[0][0] = 1 - 2*(qy*qy + qz*qz);
    R[0][1] = 2*(qx*qy - qw*qz);
    R[0][2] = 2*(qx*qz + qw*qy);
    R[1][0] = 2*(qx*qy + qw*qz);
    R[1][1] = 1 - 2*(qx*qx + qz*qz);
    R[1][2] = 2*(qy*qz - qw*qx);
    R[2][0] = 2*(qx*qz - qw*qy);
    R[2][1] = 2*(qy*qz + qw*qx);
    R[2][2] = 1 - 2*(qx*qx + qy*qy);

    // Column-major mat4, with Y-flip (negate column 1)
    // Column 0
    m[0]  = R[0][0];  m[1]  = R[1][0];  m[2]  = R[2][0];  m[3]  = 0;
    // Column 1 (negated for Y-flip)
    m[4]  = -R[0][1]; m[5]  = -R[1][1]; m[6]  = -R[2][1]; m[7]  = 0;
    // Column 2
    m[8]  = R[0][2];  m[9]  = R[1][2];  m[10] = R[2][2];  m[11] = 0;
    // Column 3
    m[12] = 0;         m[13] = 0;         m[14] = 0;         m[15] = 1;
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
