#pragma once
#include "sokol_gfx.h"
#include "camera.h"
#include "hotspot.h"
#include "colmap.h"
#include <cstdint>

struct RefView {
    char    image_name[256];
    int     colmap_id;         // source IMAGE_ID
    float   position[3];       // world-space camera center (-R^T * T)
    float   rotation[16];      // column-major world-to-camera matrix
    float   yaw, pitch;        // derived from rotation for lerp target
    sg_image texture;          // id == 0 until image loaded
    sg_view  texture_view;     // sampled view of `texture`
    int     width, height;

    // Authored clickable regions on this view's panorama. NULL until a
    // sidecar is loaded via hotspot_load_for_set.
    Hotspot* hotspots;
    uint32_t hotspot_count;
};

struct CovisEdge {
    uint32_t idx_a, idx_b;     // internal RefView indices
    uint32_t inliers;          // geometrically verified inlier matches
};

struct RefViewSet {
    RefView* views;
    uint32_t count;
    int32_t  selected;         // -1 = none
    int32_t  current_node;     // nearest node to camera (updated each frame externally)
    char     image_dir[512];   // resolved path to images/

    // lerp state
    bool     lerping;
    float    lerp_t;
    float    lerp_duration;    // computed per-transition from distance / speed
    float    lerp_speed;       // world units per second
    float    start_pos[3];
    float    start_yaw, start_pitch;

    // Inspect-mode lerp: when true, refview_update interpolates camera to
    // the inspect_target_* transform (rather than to views[selected].position)
    // and also lerps yaw/pitch. The orthographic switch itself is driven by
    // cam->orthographic / cam->ortho_blend (set by the caller on click).
    bool     inspect_mode;
    float    inspect_target_pos[3];
    float    inspect_target_yaw;
    float    inspect_target_pitch;

    // True between an inspect-hotspot click and the user exiting via right-click.
    // While true, right-click triggers a return-to-source lerp instead of the
    // usual camera_mode toggle.
    bool     in_inspect;
    float    inspect_return_pos[3];   // camera position at the moment of inspect click

    // True for a return-from-inspect lerp: refview_update lerps position only,
    // leaving yaw/pitch under the user's mouse control.
    bool     inspect_return;

    // neighbor discovery
    float    neighbor_radius;  // only show nodes within this distance of current_node

    // covisibility graph
    CovisEdge* covis_edges;
    uint32_t   covis_edge_count;
    int        min_inliers;        // threshold: minimum inlier count to consider connected
    bool       use_covisibility;   // true = covis graph, false = distance-based
};

// Build reference views from already-parsed camera poses.
bool refview_load(RefViewSet* set, const ColmapImageSet* images, const char* image_dir);

// Build covisibility graph from already-parsed source image-pair edges.
void refview_load_covisibility(RefViewSet* set, const ColmapCovisibility* covis);

// Load images as sokol_gfx images + views. Call after refview_load.
void refview_load_images(RefViewSet* set);

// Release sokol_gfx images + views.
void refview_release_images(RefViewSet* set);

// Advance interpolation, write into cam. Returns true while lerping (camera locked).
bool refview_update(RefViewSet* set, Camera* cam, float dt);

// Collect neighbor node positions (within neighbor_radius of current_node).
// Writes up to max_count positions (float[3] each) and refview indices into out arrays.
// Returns actual count written.
uint32_t refview_get_neighbors(const RefViewSet* set, float* out_positions, uint32_t* out_indices, uint32_t max_count);

// Copy this view's column-major world-to-camera rotation matrix.
void refview_get_rotation_matrix(const RefView* v, float* out_mat4);

void refview_free(RefViewSet* set);
