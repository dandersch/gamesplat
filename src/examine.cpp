#include "examine.h"

#include <cmath>

#include "maths.h"

bool examine_locks_input(const Examine& e) { return e.state != Examine::OFF; }

// Linear interp between two MeshTransforms with smoothstep easing on t.
// Rotation uses naive per-axis Euler lerp (good enough for the small swings
// we get going from rest -> camera-aligned pose over 0.4s).
static void examine_lerp_transform(const MeshTransform& a, const MeshTransform& b,
                                   float t, MeshTransform* out) {
    float k = math_smoothstep01(t);
    for (int i = 0; i < 3; ++i) {
        out->translation[i]    = a.translation[i]    + (b.translation[i]    - a.translation[i]) * k;
        out->rotation_euler[i] = a.rotation_euler[i] + (b.rotation_euler[i] - a.rotation_euler[i]) * k;
    }
    out->scale = a.scale + (b.scale - a.scale) * k;
}

// Compute the world-space pose that puts the mesh's AABB center at a fixed
// distance in front of the camera with the object's local axes aligned to the
// camera's right/up/forward basis. Distance is auto-derived so the AABB sphere
// fits ~80% of the vertical viewport at the camera's current FOV.
//
// rest_scale is preserved from the object's resting transform.
void examine_compute_target(const Camera& cam,
                            const float aabb_center_local[3],
                            float aabb_radius,
                            const MeshTransform& rest,
                            MeshTransform* out) {
    // Camera basis in world space (right-handed; matches
    // camera_get_overlay_ray_basis). `cam_fwd` is the actual look direction
    // and is used unmodified for placing the object in front of the camera.
    float cam_fwd[3];
    camera_get_forward(&cam, cam_fwd);
    float world_up[3] = { 0.0f, 1.0f, 0.0f };
    float right[3] = {
        world_up[1]*cam_fwd[2] - world_up[2]*cam_fwd[1],
        world_up[2]*cam_fwd[0] - world_up[0]*cam_fwd[2],
        world_up[0]*cam_fwd[1] - world_up[1]*cam_fwd[0],
    };
    float rl = sqrtf(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
    if (rl > 1e-8f) { right[0]/=rl; right[1]/=rl; right[2]/=rl; }
    float cam_up[3] = {
        cam_fwd[1]*right[2] - cam_fwd[2]*right[1],
        cam_fwd[2]*right[0] - cam_fwd[0]*right[2],
        cam_fwd[0]*right[1] - cam_fwd[1]*right[0],
    };

    // Target rotation columns. We want:
    //   - model local +Y -> world up   (right-side up)
    //   - model local +Z -> -cam_fwd   (model's "front" faces the camera)
    // For the basis to be a proper rotation (det = +1), the right column
    // must also be negated relative to the camera right.
    float right_col[3] = { -right[0], -right[1], -right[2] };
    float up[3]        = {  cam_up[0],   cam_up[1],   cam_up[2]  };
    float fwd[3]       = { -cam_fwd[0], -cam_fwd[1], -cam_fwd[2] };

    // Distance: place the AABB sphere so its diameter spans 80% of the
    // vertical viewport. Clamped to a sane minimum so tiny meshes don't sit
    // on the near plane.
    float effective_r = aabb_radius * rest.scale;
    if (effective_r < 1e-4f) effective_r = 1e-4f;
    float dist = effective_r / tanf(cam.fov_y * 0.5f) / 0.8f;
    if (dist < effective_r * 1.5f) dist = effective_r * 1.5f;

    // Decompose [right_col | up | fwd] into Z-Y-X intrinsic Euler (inverse of
    // mat4_from_transform in renderer.cpp).
    //   r20 = -sy
    //   r21 =  sx*cy   r22 = cx*cy   -> x = atan2(r21, r22)
    //   r00 =  cy*cz   r10 = cy*sz   -> z = atan2(r10, r00)
    float r20 = right_col[2];   // column 0 = right_col, row 2
    float r21 = up[2];          // column 1 = up,        row 2
    float r22 = fwd[2];         // column 2 = fwd,       row 2
    float r00 = right_col[0];
    float r10 = right_col[1];
    float sy  = -r20;
    if (sy >  1.0f) sy =  1.0f;
    if (sy < -1.0f) sy = -1.0f;
    out->rotation_euler[1] = asinf(sy);
    if (fabsf(sy) < 0.99995f) {
        out->rotation_euler[0] = atan2f(r21, r22);
        out->rotation_euler[2] = atan2f(r10, r00);
    } else {
        // Gimbal lock fallback: fold roll into yaw.
        out->rotation_euler[0] = 0.0f;
        out->rotation_euler[2] = atan2f(-right_col[1], up[1]);
    }

    out->scale = rest.scale;

    // Translation: cam_pos + fwd*dist places the AABB *center* there, so we
    // subtract the rotated+scaled local center offset from the target point.
    // Local center in world = R * (center * scale) (no T yet).
    float cs = rest.scale;
    float cx = aabb_center_local[0] * cs;
    float cy = aabb_center_local[1] * cs;
    float cz = aabb_center_local[2] * cs;
    float center_w[3] = {
        right_col[0]*cx + up[0]*cy + fwd[0]*cz,
        right_col[1]*cx + up[1]*cy + fwd[1]*cz,
        right_col[2]*cx + up[2]*cy + fwd[2]*cz,
    };
    // Use the actual camera look direction (cam_fwd) for placement; the
    // negated `fwd` is only used to orient the model's local axes.
    out->translation[0] = cam.position[0] + cam_fwd[0]*dist - center_w[0];
    out->translation[1] = cam.position[1] + cam_fwd[1]*dist - center_w[1];
    out->translation[2] = cam.position[2] + cam_fwd[2]*dist - center_w[2];
}

// Advance the examine lerp and write the resulting object_transform.
// Returns true if any state was touched (caller uses this to skip FPS input).
bool examine_tick(Examine* e, MeshTransform* object_transform, float dt) {
    if (e->state == Examine::OFF || e->state == Examine::ACTIVE) return e->state != Examine::OFF;
    e->t += dt / EXAMINE_LERP_DURATION;
    if (e->t >= 1.0f) {
        e->t = 1.0f;
        examine_lerp_transform(e->start, e->target, 1.0f, object_transform);
        if (e->state == Examine::LERP_IN) {
            e->state = Examine::ACTIVE;
        } else { // LERP_OUT
            *object_transform = e->rest;
            e->state = Examine::OFF;
        }
    } else {
        examine_lerp_transform(e->start, e->target, e->t, object_transform);
    }
    return true;
}
