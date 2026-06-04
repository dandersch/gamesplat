#pragma once

#include "camera.h"
#include "renderer.h"

// Examine mode: while active, the --object mesh is lerped in front of the
// camera for inspection (separate from the refview hotspot inspect mode).
// State transitions:
//   OFF      --left-click on object-->  LERP_IN
//   LERP_IN  --t reaches 1.0------->    ACTIVE     (orbit + zoom controls live)
//   ACTIVE   --right-click-------->     LERP_OUT
//   LERP_OUT --t reaches 1.0------->    OFF        (object_transform restored to `rest`)
struct Examine {
    enum State { OFF, LERP_IN, ACTIVE, LERP_OUT };
    State         state;
    float         t;
    MeshTransform start;             // object_transform at the moment the current lerp began
    MeshTransform target;            // pose the current lerp is heading to
    MeshTransform rest;              // pose to restore when leaving examine
    float         aabb_center_local[3];
    float         aabb_radius;

    // Orbit state, valid once we reach (or pass through) ACTIVE.
    // Camera basis snapshot at examine-entry: drives the zoom axis and the
    // pitch axis so they stay stable even though the FPS camera is locked.
    float         cam_fwd_entry[3];
    float         cam_right_entry[3];
    float         dist_base;             // auto-fit distance computed at entry
    float         base_rotation_euler[3];// target rotation at entry (R0); orbit composes on top
    float         orbit_yaw;             // accumulated mouse-X drag (about world up)
    float         orbit_pitch;           // accumulated mouse-Y drag (about cam_right_entry)
    float         distance_scale;        // scroll-wheel zoom multiplier (1.0 = entry distance)
};

static const float EXAMINE_LERP_DURATION = 0.4f;
static const float EXAMINE_ORBIT_SENSITIVITY = 0.005f;
static const float EXAMINE_PITCH_LIMIT       = 1.50f;  // ~86 deg
static const float EXAMINE_ZOOM_MIN          = 0.40f;
static const float EXAMINE_ZOOM_MAX          = 3.00f;

bool examine_locks_input(const Examine& e);
void examine_compute_target(const Camera& cam,
                            const float aabb_center_local[3],
                            float aabb_radius,
                            const MeshTransform& rest,
                            MeshTransform* out);
bool examine_tick(Examine* e, MeshTransform* object_transform, float dt);
