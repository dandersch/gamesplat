#include "colmap.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <dirent.h>

#if !defined(__EMSCRIPTEN__)
#include <sqlite3.h>
#endif

// TODO utils.h / strings.h
static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// TODO strings.h
static void copy_string(char* dst, size_t dst_size, const char* src) {
    snprintf(dst, dst_size, "%s", src ? src : "");
}

// TODO utils.h / strings.h
static void join_path(char* dst, size_t dst_size, const char* a, const char* b) {
    copy_string(dst, dst_size, a);
    size_t len = strlen(dst);
    if (len + 1 < dst_size) {
        dst[len++] = '/';
        dst[len] = '\0';
    }
    if (len < dst_size) {
        strncat(dst, b, dst_size - len - 1);
    }
}

// TODO utils.h / strings.h
static bool is_numeric_name(const char* name) {
    if (!name || !name[0]) return false;
    for (const char* p = name; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    return true;
}

static bool text_model_exists(const char* dir) {
    char path[512];
    join_path(path, sizeof(path), dir, "images.txt");
    if (!file_exists(path)) return false;
    join_path(path, sizeof(path), dir, "cameras.txt");
    return file_exists(path);
}

static bool binary_model_exists(const char* dir) {
    char path[512];
    join_path(path, sizeof(path), dir, "images.bin");
    if (!file_exists(path)) return false;
    join_path(path, sizeof(path), dir, "cameras.bin");
    return file_exists(path);
}

static void log_binary_unsupported(const char* dir) {
    LOG(WARN|COLMAP|LOAD, "Found binary model at %s, but only text models are supported", dir);
    LOG(INFO|COLMAP|LOAD, "Convert it with:");
    LOG(INFO|COLMAP|LOAD, "  colmap model_converter --input_path %s --output_path %s --output_type TXT", dir, dir);
}

static bool try_model_candidate(ColmapPaths* out, const char* model_dir, const char* root_dir) {
    if (text_model_exists(model_dir)) {
        copy_string(out->model_dir, sizeof(out->model_dir), model_dir);
        copy_string(out->root_dir, sizeof(out->root_dir), root_dir);
        LOG(INFO|COLMAP|LOAD, "model directory: %s", out->model_dir);
        return true;
    }
    if (binary_model_exists(model_dir)) {
        log_binary_unsupported(model_dir);
    }
    return false;
}

static bool find_first_numeric_sparse_model(ColmapPaths* out, const char* input_path) {
    char sparse_dir[512];
    join_path(sparse_dir, sizeof(sparse_dir), input_path, "sparse");

    DIR* dir = opendir(sparse_dir);
    if (!dir) return false;

    int best = -1;
    dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_numeric_name(ent->d_name)) continue;
        int value = atoi(ent->d_name);
        char candidate[512];
        join_path(candidate, sizeof(candidate), sparse_dir, ent->d_name);
        if (!text_model_exists(candidate)) {
            if (binary_model_exists(candidate)) log_binary_unsupported(candidate);
            continue;
        }
        if (best < 0 || value < best) best = value;
    }
    closedir(dir);

    if (best < 0) return false;

    char model_dir[512];
    char best_name[32];
    snprintf(best_name, sizeof(best_name), "%d", best);
    join_path(model_dir, sizeof(model_dir), sparse_dir, best_name);
    return try_model_candidate(out, model_dir, input_path);
}

static bool read_first_image_name(char* out_name, size_t out_size, const char* model_dir) {
    char images_txt[512];
    join_path(images_txt, sizeof(images_txt), model_dir, "images.txt");

    FILE* f = fopen(images_txt, "r");
    if (!f) return false;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        int image_id, camera_id;
        float qw, qx, qy, qz, tx, ty, tz;
        char name[256] = {};
        int parsed = sscanf(line, "%d %f %f %f %f %f %f %f %d %255s",
                            &image_id, &qw, &qx, &qy, &qz, &tx, &ty, &tz, &camera_id, name);
        fclose(f);
        if (parsed < 10) return false;
        copy_string(out_name, out_size, name);
        return true;
    }

    fclose(f);
    return false;
}

static bool image_exists_under(const char* dir, const char* image_name) {
    char path[768];
    join_path(path, sizeof(path), dir, image_name);
    return file_exists(path);
}

static void resolve_image_dir(ColmapPaths* out, const char* input_path) {
    char first_image[256];
    if (!read_first_image_name(first_image, sizeof(first_image), out->model_dir)) {
        join_path(out->image_dir, sizeof(out->image_dir), out->root_dir, "images");
        LOG(WARN|COLMAP|LOAD, "Could not read first image name; using image directory: %s", out->image_dir);
        return;
    }

    const char* candidates[6];
    char root_images[512], input_images[512], model_images[512], model_parent_images[512], model_grandparent_images[512];
    join_path(root_images, sizeof(root_images), out->root_dir, "images");
    join_path(input_images, sizeof(input_images), input_path, "images");
    join_path(model_images, sizeof(model_images), out->model_dir, "images");
    join_path(model_parent_images, sizeof(model_parent_images), out->model_dir, "../images");
    join_path(model_grandparent_images, sizeof(model_grandparent_images), out->model_dir, "../../images");
    candidates[0] = root_images;
    candidates[1] = input_images;
    candidates[2] = model_images;
    candidates[3] = model_parent_images;
    candidates[4] = model_grandparent_images;
    candidates[5] = out->model_dir;

    for (int i = 0; i < 6; i++) {
        if (image_exists_under(candidates[i], first_image)) {
            copy_string(out->image_dir, sizeof(out->image_dir), candidates[i]);
            LOG(INFO|COLMAP|LOAD, "image directory: %s", out->image_dir);
            return;
        }
    }

    join_path(out->image_dir, sizeof(out->image_dir), out->root_dir, "images");
    LOG(WARN|COLMAP|LOAD, "Could not verify image directory using %s; using %s", first_image, out->image_dir);
}

static void resolve_database_path(ColmapPaths* out, const char* input_path) {
    const char* candidates[8];
    char root_db[512], root_database_db[512], input_db[512], input_database_db[512];
    char model_db[512], model_parent_db[512], model_grandparent_db[512], model_grandparent_database_db[512];
    join_path(root_db, sizeof(root_db), out->root_dir, "database.db");
    join_path(root_database_db, sizeof(root_database_db), out->root_dir, "database/database.db");
    join_path(input_db, sizeof(input_db), input_path, "database.db");
    join_path(input_database_db, sizeof(input_database_db), input_path, "database/database.db");
    join_path(model_db, sizeof(model_db), out->model_dir, "database.db");
    join_path(model_parent_db, sizeof(model_parent_db), out->model_dir, "../database.db");
    join_path(model_grandparent_db, sizeof(model_grandparent_db), out->model_dir, "../../database.db");
    join_path(model_grandparent_database_db, sizeof(model_grandparent_database_db), out->model_dir, "../../database/database.db");
    candidates[0] = root_db;
    candidates[1] = root_database_db;
    candidates[2] = input_db;
    candidates[3] = input_database_db;
    candidates[4] = model_db;
    candidates[5] = model_parent_db;
    candidates[6] = model_grandparent_db;
    candidates[7] = model_grandparent_database_db;

    out->database_path[0] = '\0';
    for (int i = 0; i < 8; i++) {
        if (file_exists(candidates[i])) {
            copy_string(out->database_path, sizeof(out->database_path), candidates[i]);
            LOG(INFO|COLMAP|LOAD, "database: %s", out->database_path);
            return;
        }
    }

    LOG(WARN|COLMAP|LOAD, "No database.db found; covisibility will fall back to distance-based neighbors");
}

bool colmap_resolve_paths(ColmapPaths* out, const char* input_path) {
    memset(out, 0, sizeof(*out));
    if (!input_path || !input_path[0]) return false;

    char candidate[512];
    if (!try_model_candidate(out, input_path, input_path)) {
        join_path(candidate, sizeof(candidate), input_path, "sparse/0");
        if (!try_model_candidate(out, candidate, input_path)) {
            join_path(candidate, sizeof(candidate), input_path, "sparse");
            if (!try_model_candidate(out, candidate, input_path)) {
                if (!find_first_numeric_sparse_model(out, input_path)) {
                    LOG(ERROR|COLMAP|LOAD, "Could not find supported text model under %s", input_path);
                    LOG(INFO|COLMAP|LOAD, "Expected images.txt and cameras.txt in one of: path, path/sparse/0, path/sparse, path/sparse/<number>");
                    return false;
                }
            }
        }
    }

    resolve_image_dir(out, input_path);
    resolve_database_path(out, input_path);
    return true;
}

// TODO utils.h / strings.h
// Skip the rest of the current line (handles arbitrarily long POINTS2D lines)
static void skip_line(FILE* f) {
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {}
}

// Convert COLMAP quaternion (world-to-camera, X-right Y-down Z-forward) to
// world-space camera center and yaw/pitch matching this renderer's Y-up fly camera.
static void colmap_pose_to_camera(float qw, float qx, float qy, float qz,
                                  float tx, float ty, float tz,
                                  float* out_pos, float* out_rotation, float* out_yaw, float* out_pitch)
{
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

    // Column-major mat4, with Y-flip (negate column 1) baked in for renderer use.
    out_rotation[0]  = R[0][0];  out_rotation[1]  = R[1][0];  out_rotation[2]  = R[2][0];  out_rotation[3]  = 0;
    out_rotation[4]  = -R[0][1]; out_rotation[5]  = -R[1][1]; out_rotation[6]  = -R[2][1]; out_rotation[7]  = 0;
    out_rotation[8]  = R[0][2];  out_rotation[9]  = R[1][2];  out_rotation[10] = R[2][2];  out_rotation[11] = 0;
    out_rotation[12] = 0;         out_rotation[13] = 0;         out_rotation[14] = 0;         out_rotation[15] = 1;

    out_pos[0] = -(R[0][0]*tx + R[1][0]*ty + R[2][0]*tz);
    out_pos[1] = -(R[0][1]*tx + R[1][1]*ty + R[2][1]*tz);
    out_pos[2] = -(R[0][2]*tx + R[1][2]*ty + R[2][2]*tz);

    float fwd_colmap[3] = { R[2][0], R[2][1], R[2][2] };
    float fwd[3] = { fwd_colmap[0], -fwd_colmap[1], fwd_colmap[2] };
    out_pos[1] = -out_pos[1];

    *out_pitch = asinf(fwd[1]);
    *out_yaw   = atan2f(fwd[0], fwd[2]);
}

bool colmap_load_images_txt(ColmapImageSet* out, const char* model_dir) {
    memset(out, 0, sizeof(*out));

    char images_txt[512];
    snprintf(images_txt, sizeof(images_txt), "%s/images.txt", model_dir);

    FILE* f = fopen(images_txt, "r");
    if (!f) {
        LOG(ERROR|COLMAP|IO, "Could not open %s", images_txt);
        return false;
    }

    uint32_t count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        count++;
        skip_line(f);
    }

    if (count == 0) {
        LOG(ERROR|COLMAP|PARSE, "No images found in %s", images_txt);
        fclose(f);
        return false;
    }

    out->images = (ColmapImage*)calloc(count, sizeof(ColmapImage));
    out->count = count;

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
            LOG(WARN|COLMAP|PARSE, "Failed to parse line: %s", line);
            skip_line(f);
            continue;
        }

        ColmapImage* image = &out->images[idx];
        image->image_id = image_id;
        image->camera_id = camera_id;
        copy_string(image->name, sizeof(image->name), name);
        colmap_pose_to_camera(qw, qx, qy, qz, tx, ty, tz,
                              image->position, image->rotation, &image->yaw, &image->pitch);

        idx++;
        skip_line(f);
    }

    out->count = idx;
    fclose(f);

    LOG(INFO|COLMAP|LOAD, "Loaded %u camera poses from %s", out->count, images_txt);
    return true;
}

void colmap_free_image_set(ColmapImageSet* set) {
    free(set->images);
    memset(set, 0, sizeof(*set));
}

bool colmap_load_covisibility(ColmapCovisibility* out, const char* database_path) {
    memset(out, 0, sizeof(*out));

#if !defined(__EMSCRIPTEN__)
    if (!database_path || !database_path[0]) {
        LOG(WARN|COLMAP|LOAD, "No database path; covisibility unavailable");
        return false;
    }

    sqlite3* db = NULL;
    if (sqlite3_open_v2(database_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        LOG(ERROR|COLMAP|IO, "Could not open %s (%s)", database_path, db ? sqlite3_errmsg(db) : "unknown error");
        if (db) sqlite3_close(db);
        return false;
    }

    const char* sql = "SELECT pair_id, rows FROM two_view_geometries WHERE config = 2 AND rows > 0";
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG(ERROR|COLMAP|IO, "SQL error: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return false;
    }

    uint32_t edge_cap = 64;
    out->edges = (ColmapCovisEdge*)SDL_malloc(edge_cap * sizeof(ColmapCovisEdge));

    const int64_t MAX_ID = 2147483647LL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t pair_id = sqlite3_column_int64(stmt, 0);
        int inliers = sqlite3_column_int(stmt, 1);

        int id1 = (int)(pair_id / MAX_ID);
        int id2 = (int)(pair_id % MAX_ID);

        if (out->count == edge_cap) {
            edge_cap *= 2;
            out->edges = (ColmapCovisEdge*)realloc(out->edges, edge_cap * sizeof(ColmapCovisEdge));
        }
        out->edges[out->count++] = { id1, id2, (uint32_t)inliers };
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    LOG(INFO|COLMAP|LOAD, "Loaded %u covisibility edges from %s", out->count, database_path);
    return true;
#else
    (void)database_path;
    return false;
#endif
}

void colmap_free_covisibility(ColmapCovisibility* covis) {
    free(covis->edges);
    memset(covis, 0, sizeof(*covis));
}
