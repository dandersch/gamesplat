#pragma once
#include <cstdint>

struct ColmapPaths {
    char root_dir[512];
    char model_dir[512];
    char image_dir[512];
    char database_path[512]; // empty when no database was found
};

struct ColmapImage {
    char     name[256];
    int      image_id;
    int      camera_id;
    float    position[3]; // world-space camera center, converted to renderer Y-up
    float    rotation[16]; // column-major world-to-camera matrix, Y-flip baked in
    float    yaw;
    float    pitch;
};

struct ColmapImageSet {
    ColmapImage* images;
    uint32_t     count;
};

struct ColmapCovisEdge {
    int      image_id_a;
    int      image_id_b;
    uint32_t inliers;
};

struct ColmapCovisibility {
    ColmapCovisEdge* edges;
    uint32_t         count;
};

// Resolve common COLMAP workspace/model layouts from an input path. Only text
// models are supported. Possible future convenience: add --images and
// --database overrides for uncommon layouts auto-detection cannot infer.
bool colmap_resolve_paths(ColmapPaths* out, const char* input_path);

// Load and parse <model_dir>/images.txt. Pose conversion from COLMAP's camera
// convention into this renderer's Y-up convention happens here.
bool colmap_load_images_txt(ColmapImageSet* out, const char* model_dir);

// Load verified image-pair covisibility from COLMAP's optional SQLite database.
bool colmap_load_covisibility(ColmapCovisibility* out, const char* database_path);

void colmap_free_image_set(ColmapImageSet* set);
void colmap_free_covisibility(ColmapCovisibility* covis);
