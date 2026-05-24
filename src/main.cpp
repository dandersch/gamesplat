#include <SDL3/SDL.h>
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>
#include <cstdio>
#include <cstring>

#include "sokol_gfx.h"
#define SOKOL_IMGUI_NO_SOKOL_APP
#include "imgui.h"
#include "sokol_imgui.h"
#include "imgui_impl_sdl3.h"

#include "camera.cpp"
#include "gaussian.cpp"
#include "mesh.cpp"
#include "renderer.cpp"
#include "json_mini.cpp"
#include "hotspot.cpp"
#include "refview.cpp"
#include "audio.cpp"

// Returns true if file at `path` exists and is readable.
static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

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

// 3x3 column-major rotation helpers. mat3_from_euler_zyx mirrors the layout
// used by mat4_from_transform in renderer.cpp so decomposition round-trips.
static void mat3_from_euler_zyx(const float e[3], float m[9]) {
    float cx = cosf(e[0]), sx = sinf(e[0]);
    float cy = cosf(e[1]), sy = sinf(e[1]);
    float cz = cosf(e[2]), sz = sinf(e[2]);
    // column 0
    m[0] = cy * cz;
    m[1] = cy * sz;
    m[2] = -sy;
    // column 1
    m[3] = sx * sy * cz - cx * sz;
    m[4] = sx * sy * sz + cx * cz;
    m[5] = sx * cy;
    // column 2
    m[6] = cx * sy * cz + sx * sz;
    m[7] = cx * sy * sz - sx * cz;
    m[8] = cx * cy;
}

static void mat3_mul(const float a[9], const float b[9], float out[9]) {
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++) {
            out[c*3+r] = a[0*3+r]*b[c*3+0] + a[1*3+r]*b[c*3+1] + a[2*3+r]*b[c*3+2];
        }
    }
}

// Rodrigues: rotation by `theta` (rad) about unit axis `a`. Column-major.
static void mat3_axis_angle(const float a[3], float theta, float m[9]) {
    float c = cosf(theta), s = sinf(theta), C = 1.0f - c;
    float x = a[0], y = a[1], z = a[2];
    m[0] = c + x*x*C;
    m[1] = y*x*C + z*s;
    m[2] = z*x*C - y*s;
    m[3] = x*y*C - z*s;
    m[4] = c + y*y*C;
    m[5] = z*y*C + x*s;
    m[6] = x*z*C + y*s;
    m[7] = y*z*C - x*s;
    m[8] = c + z*z*C;
}

// Inverse of mat3_from_euler_zyx; uses the same gimbal-lock fallback as
// examine_compute_target.
static void mat3_decompose_zyx(const float m[9], float e[3]) {
    float sy = -m[2]; // -r20
    if (sy > 1.0f) sy = 1.0f;
    if (sy < -1.0f) sy = -1.0f;
    e[1] = asinf(sy);
    if (fabsf(sy) < 0.99995f) {
        e[0] = atan2f(m[5], m[8]);   // atan2(r21, r22)
        e[2] = atan2f(m[1], m[0]);   // atan2(r10, r00)
    } else {
        e[0] = 0.0f;
        e[2] = atan2f(-m[3], m[4]);  // atan2(-r01, r11)
    }
}

static bool examine_locks_input(const Examine& e) { return e.state != Examine::OFF; }

static float smoothstep01(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// Linear interp between two MeshTransforms with smoothstep easing on t.
// Rotation uses naive per-axis Euler lerp (good enough for the small swings
// we get going from rest -> camera-aligned pose over 0.4s).
static void examine_lerp_transform(const MeshTransform& a, const MeshTransform& b,
                                   float t, MeshTransform* out) {
    float k = smoothstep01(t);
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
static void examine_compute_target(const Camera& cam,
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
static bool examine_tick(Examine* e, MeshTransform* object_transform, float dt) {
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

// Slab-style ray vs axis-aligned box intersection. On hit, returns true and
// writes the near intersection distance to *out_t (clamped to >= 0 so that
// the camera being inside the box still counts as a hit at t = 0).
static bool ray_aabb(const float origin[3], const float dir[3],
                     const float bmin[3], const float bmax[3], float* out_t) {
    float tmin = -1e30f, tmax = 1e30f;
    for (int axis = 0; axis < 3; axis++) {
        float o = origin[axis], d = dir[axis];
        if (fabsf(d) < 1e-8f) {
            if (o < bmin[axis] || o > bmax[axis]) return false;
        } else {
            float t1 = (bmin[axis] - o) / d, t2 = (bmax[axis] - o) / d;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    if (tmax < 0.0f) return false;
    *out_t = tmin > 0.0f ? tmin : 0.0f;
    return true;
}


static const uint32_t MAX_NEIGHBORS = 64;

struct AppState {
    const char* ply_path;
    const char* colmap_dir;
    const char* mesh_path;
    const char* object_path;
    char        colmap_dir_buf[512];

    SDL_Window*    window;
    SDL_GLContext  gl_context;
    bool           sg_setup_done;
    bool           simgui_setup_done;
    bool           imgui_sdl3_initialized;
    bool           renderer_started;

    Sfx           sfx_transition;
    Renderer      renderer;
    GaussianScene scene;
    bool          scene_loaded;
    Mesh          mesh;
    Mesh          object;
    RefViewSet    refviews;
    bool          refviews_loaded;

    Camera cam;
    bool   keys[9]; // W A S D Space LCtrl LShift E Q
    uint64_t last_time;
    uint64_t freq;
    float    refview_max_alpha;
    float    node_half_size;
    bool     show_node_boxes;
    bool     show_hotspot_debug;

    // Mesh path animation (walks the mesh from refview node 0..n-1 and loops)
    uint32_t anim_node;
    float    anim_t;
    float    anim_speed; // world units per second
    float    anim_yaw;
    bool     anim_yaw_initialized;
    float    anim_y_offset; // refview nodes are at head height; drop feet ~1.6m

    // Neighbor scratch buffers
    float    neighbor_positions[MAX_NEIGHBORS * 3];
    uint32_t neighbor_indices[MAX_NEIGHBORS];
    uint32_t neighbor_count;

    // Top-down map overlay: separate camera, never mutates the FPS `cam`.
    // Right-click toggles `map_view_active`; while active a second render
    // pass in renderer_draw_frame draws mesh+splats+wireframe from this
    // camera on top of the FPS frame (transparent background = FPS shows
    // through where the map didn't draw).
    bool   map_view_active;
    Camera map_cam;

    // Map overlay interaction: left-drag pans, wheel zooms toward cursor.
    bool map_dragging;

    // Object examine mode (--object). Disabled until the user clicks the mesh.
    Examine examine;

    // Accumulated by SDL_AppEvent and consumed once per SDL_AppIterate.
    float mouse_dx;
    float mouse_dy;
};

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    AppState* state = new AppState();
    *appstate = state;

    const char*& ply_path = state->ply_path;
    const char*& colmap_dir = state->colmap_dir;
    const char*& mesh_path = state->mesh_path;
    const char*& object_path = state->object_path;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--colmap") == 0 && i + 1 < argc) {
            colmap_dir = argv[++i];
        } else if (strcmp(argv[i], "--mesh") == 0 && i + 1 < argc) {
            mesh_path = argv[++i];
        } else if (strcmp(argv[i], "--object") == 0 && i + 1 < argc) {
            object_path = argv[++i];
        } else if (!ply_path) {
            ply_path = argv[i];
        }
    }

    // Accept either a colmap base directory (containing sparse/0/) or the
    // sparse/0 directory directly. If the user passed the base, resolve it
    // to <base>/sparse/0 so the rest of the code (which derives image_dir
    // and database.db via ../../) keeps working unchanged.
    if (colmap_dir) {
        char probe[512];
        snprintf(probe, sizeof(probe), "%s/images.txt", colmap_dir);
        bool has_model_here = file_exists(probe);
        if (!has_model_here) {
            snprintf(probe, sizeof(probe), "%s/images.bin", colmap_dir);
            has_model_here = file_exists(probe);
        }
        if (!has_model_here) {
            snprintf(state->colmap_dir_buf, sizeof(state->colmap_dir_buf), "%s/sparse/0", colmap_dir);
            colmap_dir = state->colmap_dir_buf;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // Audio is non-fatal: if init fails (no device, etc.) sfx_play becomes a
    // no-op via the g_audio_ready flag and the rest of the app keeps running.
    audio_init();
    sfx_load(&state->sfx_transition, "res/transition.wav");

    // Request a compatible GL context for sokol_gfx's GLCORE backend. 3.3
    // core is the floor for SOKOL_GLCORE; we don't need anything newer for
    // any of the generated shaders (glsl430).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    state->window = SDL_CreateWindow("gsplat", 1280, 720,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
    if (!state->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    state->gl_context = SDL_GL_CreateContext(state->window);
    if (!state->gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_GL_MakeCurrent(state->window, state->gl_context);
    SDL_GL_SetSwapInterval(1); // vsync

    // sokol_gfx setup. The GL backend has its own loader so we don't need
    // gl3w/glad. defaults must match the swapchain we'll pass into sg_pass.
    sg_desc sgd = {};
    sgd.environment.defaults.color_format = SG_PIXELFORMAT_RGBA8;
    sgd.environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    sgd.environment.defaults.sample_count = 1;
    sg_setup(&sgd);
    if (!sg_isvalid()) {
        fprintf(stderr, "sg_setup failed\n");
        return SDL_APP_FAILURE;
    }
    state->sg_setup_done = true;

    // sokol_imgui creates the ImGui context itself, so we don't call
    // ImGui::CreateContext(). ImGui_ImplSDL3 stays in the loop for event
    // translation; it expects an existing context, so it goes after setup.
    IMGUI_CHECKVERSION();
    simgui_desc_t sid = {};
    sid.color_format = SG_PIXELFORMAT_RGBA8;
    sid.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    sid.sample_count = 1;
    simgui_setup(&sid);
    state->simgui_setup_done = true;

    ImGui_ImplSDL3_InitForOther(state->window);
    state->imgui_sdl3_initialized = true;

    // Renderer (no more SDL_GPUDevice; sokol_gfx is set up globally).
    state->renderer_started = true;
    if (!renderer_init(&state->renderer, state->window)) {
        fprintf(stderr, "Renderer init failed\n");
        return SDL_APP_FAILURE;
    }

    // Scene
    if (ply_path) {
        state->scene_loaded = load_ply(ply_path, &state->scene);
        if (state->scene_loaded) {
            renderer_upload_gaussians(&state->renderer, &state->scene);
        }
    }

    // Mesh
    if (mesh_path) {
        if (mesh_load(mesh_path, &state->mesh)) {
            renderer_upload_mesh(&state->renderer, &state->mesh);
        }
    }

    // Static scene object (no animation, identity transform)
    if (object_path) {
        if (mesh_load(object_path, &state->object)) {
            renderer_upload_object_mesh(&state->renderer, &state->object);
        }
    }

    // Reference views
    state->refviews.selected = -1;
    if (colmap_dir) {
        state->refviews_loaded = refview_load(&state->refviews, colmap_dir);
        if (state->refviews_loaded) {
            refview_load_covisibility(&state->refviews, colmap_dir);
            refview_load_images(&state->refviews);
            hotspot_load_for_set(&state->refviews);
        }
    }

    // Camera
    camera_init(&state->cam);
    SDL_SetWindowRelativeMouseMode(state->window, true); // start in camera mode
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;

    state->last_time = SDL_GetPerformanceCounter();
    state->freq = SDL_GetPerformanceFrequency();
    state->refview_max_alpha = 0.5f;
    state->node_half_size = 0.5f;
    state->show_node_boxes = true;
    state->show_hotspot_debug = false;

    state->anim_speed = 1.0f;
    state->anim_y_offset = 1.4f;

    camera_init(&state->map_cam);
    state->map_cam.position[0]  = 1.1f;
    state->map_cam.position[1]  = -1.0f;
    state->map_cam.position[2]  = 0.6f;
    state->map_cam.yaw          = -6.3f;
    state->map_cam.pitch        = 1.56f;
    state->map_cam.orthographic = true;
    state->map_cam.ortho_blend  = 1.0f;
    state->map_cam.ortho_size   = 5.0f;

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    AppState* state = (AppState*)appstate;
    if (!state) return SDL_APP_FAILURE;
    SDL_Event& ev = *event;

    Renderer& renderer = state->renderer;
    RefViewSet& refviews = state->refviews;
    Camera& cam = state->cam;
    Camera& map_cam = state->map_cam;
    SDL_Window* window = state->window;
    Mesh& object = state->object;
    Examine& examine = state->examine;
    Sfx& sfx_transition = state->sfx_transition;
    const char* object_path = state->object_path;
    bool& refviews_loaded = state->refviews_loaded;
    bool& map_view_active = state->map_view_active;
    bool& map_dragging = state->map_dragging;
    bool* keys = state->keys;
    float* neighbor_positions = state->neighbor_positions;
    uint32_t* neighbor_indices = state->neighbor_indices;
    uint32_t& neighbor_count = state->neighbor_count;
    float& node_half_size = state->node_half_size;
    float& mouse_dx = state->mouse_dx;
    float& mouse_dy = state->mouse_dy;

    ImGui_ImplSDL3_ProcessEvent(event);

    switch (ev.type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = (ev.type == SDL_EVENT_KEY_DOWN);
        switch (ev.key.scancode) {
            case SDL_SCANCODE_W: keys[0] = down; break;
            case SDL_SCANCODE_A: keys[1] = down; break;
            case SDL_SCANCODE_S: keys[2] = down; break;
            case SDL_SCANCODE_D: keys[3] = down; break;
            case SDL_SCANCODE_SPACE: keys[4] = down; break;
            case SDL_SCANCODE_LCTRL: keys[5] = down; break;
            case SDL_SCANCODE_LSHIFT: keys[6] = down; break;
            case SDL_SCANCODE_E: keys[7] = down; break;
            case SDL_SCANCODE_Q: keys[8] = down; break;
            case SDL_SCANCODE_ESCAPE: if (down) return SDL_APP_SUCCESS; break;
            default: break;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev.button.button == SDL_BUTTON_RIGHT && examine.state != Examine::OFF) {
            // While examining, right-click exits. Cancels any in-flight
            // lerp by retargeting from the current pose back to `rest`.
            if (examine.state == Examine::ACTIVE) {
                examine.start = renderer.object_transform;
                examine.target = examine.rest;
                examine.t = 0.0f;
                examine.state = Examine::LERP_OUT;
            } else if (examine.state == Examine::LERP_IN) {
                // Mid-entry: swap direction so the easing stays smooth.
                examine.start = renderer.object_transform;
                examine.target = examine.rest;
                examine.t = 0.0f;
                examine.state = Examine::LERP_OUT;
            }
            // Do NOT toggle cam.camera_mode or map overlay.
            break;
        }
        if (ev.button.button == SDL_BUTTON_RIGHT) {
            if (refviews_loaded && refviews.in_inspect) {
                // Exit inspect: lerp position back to where we clicked
                // the hotspot from, drop ortho, re-enter FPS controls.
                // Yaw/pitch are not lerped (see refview_update); the
                // user can look around during the return.
                refviews.inspect_target_pos[0] = refviews.inspect_return_pos[0];
                refviews.inspect_target_pos[1] = refviews.inspect_return_pos[1];
                refviews.inspect_target_pos[2] = refviews.inspect_return_pos[2];
                float dx = refviews.inspect_return_pos[0] - cam.position[0];
                float dy = refviews.inspect_return_pos[1] - cam.position[1];
                float dz = refviews.inspect_return_pos[2] - cam.position[2];
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                refviews.selected = -1;
                refviews.inspect_mode = true;
                refviews.inspect_return = true;
                refviews.lerping = true;
                refviews.lerp_t = 0.0f;
                refviews.lerp_duration = (dist > 1e-6f) ? dist / refviews.lerp_speed : 0.1f;
                refviews.start_pos[0] = cam.position[0];
                refviews.start_pos[1] = cam.position[1];
                refviews.start_pos[2] = cam.position[2];
                refviews.start_yaw = cam.yaw;
                refviews.start_pitch = cam.pitch;
                refviews.in_inspect = false;
                cam.orthographic = false;
                cam.camera_mode = true;
                SDL_SetWindowRelativeMouseMode(window, true);
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
            } else {
                cam.camera_mode = !cam.camera_mode;
                SDL_SetWindowRelativeMouseMode(window, cam.camera_mode);
                ImGuiIO& io = ImGui::GetIO();
                if (cam.camera_mode) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
                else                 io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

                // Toggle the top-down map overlay on top of the
                // existing camera-mode toggle.
                map_view_active = !map_view_active;
            }
        }
        if (ev.button.button == SDL_BUTTON_LEFT && map_view_active &&
            !ImGui::GetIO().WantCaptureMouse) {
            // Begin panning the top-down map.
            map_dragging = true;
            break;
        }
        if (ev.button.button == SDL_BUTTON_LEFT && cam.camera_mode &&
            examine.state == Examine::OFF && !map_view_active) {
            // Ray from screen center (crosshair) into scene
            float forward[3];
            camera_get_forward(&cam, forward);

            // 1. Hotspot pick on the currently-overlaid view (if any).
            //    Hotspots take precedence over object/neighbor clicks.
            int  hotspot_view  = -1;
            int32_t hotspot_idx = -1;
            if (refviews_loaded && !refviews.lerping && refviews.current_node >= 0) {
                RefView* cv = &refviews.views[refviews.current_node];
                if (cv->hotspot_count > 0) {
                    // Gate on overlay-visible distance (matches fade_dist=0.1 used below).
                    float dx0 = cam.position[0] - cv->position[0];
                    float dy0 = cam.position[1] - cv->position[1];
                    float dz0 = cam.position[2] - cv->position[2];
                    float d2  = dx0*dx0 + dy0*dy0 + dz0*dz0;
                    if (d2 < 0.01f) {
                        // World forward -> ref-camera frame (matches overlay shader).
                        float R[16];
                        refview_get_rotation_matrix(cv, R);
                        float rx = R[0]*forward[0] + R[4]*forward[1] + R[8] *forward[2];
                        float ry = R[1]*forward[0] + R[5]*forward[1] + R[9] *forward[2];
                        float rz = R[2]*forward[0] + R[6]*forward[1] + R[10]*forward[2];
                        const float PI = 3.14159265358979f;
                        float u = atan2f(rx, rz) / (2.0f * PI) + 0.5f;
                        float ry_c = ry < -1.0f ? -1.0f : (ry > 1.0f ? 1.0f : ry);
                        float v = -asinf(ry_c) / PI + 0.5f;
                        hotspot_idx = hotspot_pick(cv, u, v);
                        if (hotspot_idx >= 0) hotspot_view = refviews.current_node;
                    }
                }
            }

            if (hotspot_idx >= 0) {
                const Hotspot* h = &refviews.views[hotspot_view].hotspots[hotspot_idx];
                if (h->action.type == HOTSPOT_ACTION_WARP) {
                    sfx_play(&sfx_transition, 1.2f);
                    int32_t warp_target = h->action.warp.target_view;
                    RefView* tv = &refviews.views[warp_target];
                    float dx = tv->position[0] - cam.position[0];
                    float dy = tv->position[1] - cam.position[1];
                    float dz = tv->position[2] - cam.position[2];
                    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                    refviews.selected = warp_target;
                    refviews.inspect_mode = false;
                    refviews.lerping = true;
                    refviews.lerp_t = 0.0f;
                    refviews.lerp_duration = (dist > 1e-6f) ? dist / refviews.lerp_speed : 0.1f;
                    refviews.start_pos[0] = cam.position[0];
                    refviews.start_pos[1] = cam.position[1];
                    refviews.start_pos[2] = cam.position[2];
                    refviews.start_yaw = cam.yaw;
                    refviews.start_pitch = cam.pitch;
                    break;
                } else if (h->action.type == HOTSPOT_ACTION_INSPECT) {
                    sfx_play(&sfx_transition, 1.2f);
                    const HotspotActionInspect* it = &h->action.inspect;
                    float dx = it->position[0] - cam.position[0];
                    float dy = it->position[1] - cam.position[1];
                    float dz = it->position[2] - cam.position[2];
                    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                    refviews.selected = -1;
                    refviews.inspect_mode = true;
                    refviews.inspect_return = false;
                    refviews.inspect_target_pos[0] = it->position[0];
                    refviews.inspect_target_pos[1] = it->position[1];
                    refviews.inspect_target_pos[2] = it->position[2];
                    refviews.inspect_target_yaw   = it->yaw;
                    refviews.inspect_target_pitch = it->pitch;
                    refviews.lerping = true;
                    refviews.lerp_t = 0.0f;
                    refviews.lerp_duration = (dist > 1e-6f) ? dist / refviews.lerp_speed : 0.1f;
                    refviews.start_pos[0] = cam.position[0];
                    refviews.start_pos[1] = cam.position[1];
                    refviews.start_pos[2] = cam.position[2];
                    refviews.start_yaw = cam.yaw;
                    refviews.start_pitch = cam.pitch;
                    // Remember where to lerp back to on right-click exit.
                    refviews.inspect_return_pos[0] = cam.position[0];
                    refviews.inspect_return_pos[1] = cam.position[1];
                    refviews.inspect_return_pos[2] = cam.position[2];
                    refviews.in_inspect = true;
                    // Drive the existing ortho_blend transition.
                    cam.orthographic = true;
                    cam.ortho_size   = it->ortho_size;
                    // Switch to cursor mode (point & click); right-click
                    // will exit inspect and restore FPS controls.
                    cam.camera_mode = false;
                    SDL_SetWindowRelativeMouseMode(window, false);
                    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
                    break;
                }
            }

            // 2. Object pick (--object mesh, world-AABB) vs nearest
            //    neighbor-node pick. Closer hit wins; object > node by
            //    depth. Hotspot above already short-circuited if hit.
            float object_t = 1e30f;
            if (object_path) {
                float obj_model[16];
                mat4_from_transform(renderer.object_transform, obj_model);
                float obmin[3], obmax[3];
                mesh_aabb_world(object.aabb_min, object.aabb_max, obj_model, obmin, obmax);
                float t;
                if (ray_aabb(cam.position, forward, obmin, obmax, &t)) object_t = t;
            }

            float node_t = 1e30f;
            int best_hit = -1;
            if (refviews_loaded && !refviews.lerping) {
                for (uint32_t ni = 0; ni < neighbor_count; ni++) {
                    const float* c = &neighbor_positions[ni*3];
                    float hs = node_half_size;
                    float bmin[3] = { c[0]-hs, c[1]-hs, c[2]-hs };
                    float bmax[3] = { c[0]+hs, c[1]+hs, c[2]+hs };
                    float t;
                    if (ray_aabb(cam.position, forward, bmin, bmax, &t) && t < node_t) {
                        node_t = t;
                        best_hit = (int)ni;
                    }
                }
            }

            if (object_t < 1e30f && object_t <= node_t) {
                // Start examine: capture rest pose, compute target,
                // snapshot camera basis + distance for orbit/zoom,
                // begin LERP_IN. AABB radius = half-diagonal.
                sfx_play(&sfx_transition, 1.2f);
                examine.rest  = renderer.object_transform;
                examine.start = renderer.object_transform;
                examine.aabb_center_local[0] = (object.aabb_min[0] + object.aabb_max[0]) * 0.5f;
                examine.aabb_center_local[1] = (object.aabb_min[1] + object.aabb_max[1]) * 0.5f;
                examine.aabb_center_local[2] = (object.aabb_min[2] + object.aabb_max[2]) * 0.5f;
                float ex = object.aabb_max[0] - object.aabb_min[0];
                float ey = object.aabb_max[1] - object.aabb_min[1];
                float ez = object.aabb_max[2] - object.aabb_min[2];
                examine.aabb_radius = 0.5f * sqrtf(ex*ex + ey*ey + ez*ez);
                examine_compute_target(cam, examine.aabb_center_local,
                                       examine.aabb_radius, examine.rest,
                                       &examine.target);

                // Snapshot camera basis at examine entry. Reused each
                // frame while ACTIVE so zoom + pitch axes stay stable.
                camera_get_forward(&cam, examine.cam_fwd_entry);
                float wu[3] = { 0.0f, 1.0f, 0.0f };
                examine.cam_right_entry[0] = wu[1]*examine.cam_fwd_entry[2] - wu[2]*examine.cam_fwd_entry[1];
                examine.cam_right_entry[1] = wu[2]*examine.cam_fwd_entry[0] - wu[0]*examine.cam_fwd_entry[2];
                examine.cam_right_entry[2] = wu[0]*examine.cam_fwd_entry[1] - wu[1]*examine.cam_fwd_entry[0];
                float rl = sqrtf(examine.cam_right_entry[0]*examine.cam_right_entry[0]
                               + examine.cam_right_entry[1]*examine.cam_right_entry[1]
                               + examine.cam_right_entry[2]*examine.cam_right_entry[2]);
                if (rl > 1e-8f) {
                    examine.cam_right_entry[0] /= rl;
                    examine.cam_right_entry[1] /= rl;
                    examine.cam_right_entry[2] /= rl;
                }

                // Recompute dist with the same formula compute_target
                // used (kept in sync; if you change one, change both).
                float r = examine.aabb_radius * examine.rest.scale;
                if (r < 1e-4f) r = 1e-4f;
                examine.dist_base = r / tanf(cam.fov_y * 0.5f) / 0.8f;
                if (examine.dist_base < r * 1.5f) examine.dist_base = r * 1.5f;

                examine.base_rotation_euler[0] = examine.target.rotation_euler[0];
                examine.base_rotation_euler[1] = examine.target.rotation_euler[1];
                examine.base_rotation_euler[2] = examine.target.rotation_euler[2];
                examine.orbit_yaw      = 0.0f;
                examine.orbit_pitch    = 0.0f;
                examine.distance_scale = 1.0f;

                examine.t = 0.0f;
                examine.state = Examine::LERP_IN;
            } else if (best_hit >= 0) {
                sfx_play(&sfx_transition, 1.2f);
                uint32_t view_idx = neighbor_indices[best_hit];
                RefView* tv = &refviews.views[view_idx];
                float dx = tv->position[0] - cam.position[0];
                float dy = tv->position[1] - cam.position[1];
                float dz = tv->position[2] - cam.position[2];
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                refviews.selected = (int32_t)view_idx;
                refviews.lerping = true;
                refviews.lerp_t = 0.0f;
                refviews.lerp_duration = (dist > 1e-6f) ? dist / refviews.lerp_speed : 0.1f;
                refviews.start_pos[0] = cam.position[0];
                refviews.start_pos[1] = cam.position[1];
                refviews.start_pos[2] = cam.position[2];
                refviews.start_yaw = cam.yaw;
                refviews.start_pitch = cam.pitch;
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev.button.button == SDL_BUTTON_LEFT) map_dragging = false;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (cam.camera_mode) {
            mouse_dx += ev.motion.xrel;
            mouse_dy += ev.motion.yrel;
        }
        if (map_dragging && map_view_active) {
            // Pan along the map camera's own right/up basis (which lies
            // in the world XZ plane for a top-down view). We use the
            // camera-local axes rather than world XZ so that dragging
            // still tracks screen-space movement if the map is rotated
            // about the world up axis.
            int ww, wh;
            SDL_GetWindowSize(window, &ww, &wh);
            float fwd[3], right[3], up[3];
            float wup[3] = {0.0f, 1.0f, 0.0f};
            camera_get_forward(&map_cam, fwd);
            right[0] = fwd[1]*wup[2] - fwd[2]*wup[1];
            right[1] = fwd[2]*wup[0] - fwd[0]*wup[2];
            right[2] = fwd[0]*wup[1] - fwd[1]*wup[0];
            float rl = sqrtf(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
            if (rl > 1e-8f) { right[0]/=rl; right[1]/=rl; right[2]/=rl; }
            up[0] = right[1]*fwd[2] - right[2]*fwd[1];
            up[1] = right[2]*fwd[0] - right[0]*fwd[2];
            up[2] = right[0]*fwd[1] - right[1]*fwd[0];

            // Note: camera.cpp's `right` = cross(forward, world_up)
            // actually points to screen-LEFT (see camera_update's A/D
            // strafe direction), so the signs here are flipped from
            // what a textbook drag-pan would suggest.
            float wpp = (2.0f * map_cam.ortho_size) / (float)wh;
            float dr =  ev.motion.xrel * wpp;
            float du = -ev.motion.yrel * wpp;
            map_cam.position[0] += dr * right[0] + du * up[0];
            map_cam.position[1] += dr * right[1] + du * up[1];
            map_cam.position[2] += dr * right[2] + du * up[2];
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!ImGui::GetIO().WantCaptureMouse) {
            if (examine.state != Examine::OFF) {
                // Zoom while examining: scale the AABB-fit distance.
                // Clamped so the object can't be shoved into the
                // camera or flown off to infinity.
                float factor = (ev.wheel.y > 0) ? (1.0f / 1.1f) : 1.1f;
                examine.distance_scale *= factor;
                if (examine.distance_scale < EXAMINE_ZOOM_MIN) examine.distance_scale = EXAMINE_ZOOM_MIN;
                if (examine.distance_scale > EXAMINE_ZOOM_MAX) examine.distance_scale = EXAMINE_ZOOM_MAX;
                break;
            }
            if (map_view_active) {
                // Zoom toward the cursor: scale ortho_size, then shift
                // map_cam.position so the world point under the cursor
                // before the zoom remains under the cursor after.
                float factor = (ev.wheel.y > 0) ? (1.0f / 1.2f) : 1.2f;
                float new_size = map_cam.ortho_size * factor;
                if (new_size < 0.05f) { factor = 0.05f / map_cam.ortho_size; new_size = 0.05f; }
                if (new_size > 50.0f) { factor = 50.0f  / map_cam.ortho_size; new_size = 50.0f;  }

                int ww, wh;
                SDL_GetWindowSize(window, &ww, &wh);
                float aspect_m = (float)ww / (float)wh;
                float half_h = map_cam.ortho_size;
                float half_w = half_h * aspect_m;

                float mx = ev.wheel.mouse_x;
                float my = ev.wheel.mouse_y;
                float off_r = (2.0f * mx / (float)ww - 1.0f) * half_w;
                float off_u = (1.0f - 2.0f * my / (float)wh) * half_h;

                float fwd[3], right[3], up[3];
                float wup[3] = {0.0f, 1.0f, 0.0f};
                camera_get_forward(&map_cam, fwd);
                right[0] = fwd[1]*wup[2] - fwd[2]*wup[1];
                right[1] = fwd[2]*wup[0] - fwd[0]*wup[2];
                right[2] = fwd[0]*wup[1] - fwd[1]*wup[0];
                float rl = sqrtf(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
                if (rl > 1e-8f) { right[0]/=rl; right[1]/=rl; right[2]/=rl; }
                up[0] = right[1]*fwd[2] - right[2]*fwd[1];
                up[1] = right[2]*fwd[0] - right[0]*fwd[2];
                up[2] = right[0]*fwd[1] - right[1]*fwd[0];

                // Sign matches the pan: camera.cpp's right/up basis is
                // mirrored relative to standard convention, so the
                // shift is negated.
                float k = factor - 1.0f;
                map_cam.position[0] += k * (off_r * right[0] + off_u * up[0]);
                map_cam.position[1] += k * (off_r * right[1] + off_u * up[1]);
                map_cam.position[2] += k * (off_r * right[2] + off_u * up[2]);

                map_cam.ortho_size = new_size;
            } else if (cam.camera_mode) {
                // FPS camera: wheel zooms by adjusting FOV.
                // Wheel up -> FOV down (zoom in), wheel down -> FOV up (zoom out).
                cam.fov_y *= (ev.wheel.y > 0) ? (1.0f / 1.1f) : 1.1f;
                float min_fov = 10.0f * (3.14159265358979f / 180.0f);
                float max_fov = 120.0f * (3.14159265358979f / 180.0f);
                if (cam.fov_y < min_fov) cam.fov_y = min_fov;
                if (cam.fov_y > max_fov) cam.fov_y = max_fov;
            } else {
                cam.move_speed *= (ev.wheel.y > 0) ? 1.2f : (1.0f / 1.2f);
                if (cam.move_speed < 0.1f) cam.move_speed = 0.1f;
                if (cam.move_speed > 100.0f) cam.move_speed = 100.0f;
            }
        }
        break;
    }


    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    AppState* state = (AppState*)appstate;
    if (!state) return SDL_APP_FAILURE;

    const char* mesh_path = state->mesh_path;
    const char* object_path = state->object_path;
    Renderer& renderer = state->renderer;
    GaussianScene& scene = state->scene;
    bool& scene_loaded = state->scene_loaded;
    Mesh& object = state->object;
    RefViewSet& refviews = state->refviews;
    bool& refviews_loaded = state->refviews_loaded;
    Camera& cam = state->cam;
    SDL_Window* window = state->window;
    bool* keys = state->keys;
    float& refview_max_alpha = state->refview_max_alpha;
    float& node_half_size = state->node_half_size;
    bool& show_node_boxes = state->show_node_boxes;
    bool& show_hotspot_debug = state->show_hotspot_debug;
    uint32_t& anim_node = state->anim_node;
    float& anim_t = state->anim_t;
    float& anim_speed = state->anim_speed;
    float& anim_yaw = state->anim_yaw;
    bool& anim_yaw_initialized = state->anim_yaw_initialized;
    float& anim_y_offset = state->anim_y_offset;
    const uint32_t max_neighbors = MAX_NEIGHBORS;
    float* neighbor_positions = state->neighbor_positions;
    uint32_t* neighbor_indices = state->neighbor_indices;
    uint32_t& neighbor_count = state->neighbor_count;
    bool& map_view_active = state->map_view_active;
    Camera& map_cam = state->map_cam;
    Examine& examine = state->examine;

    uint64_t now = SDL_GetPerformanceCounter();
    float dt = (float)(now - state->last_time) / (float)state->freq;
    state->last_time = now;

    float mouse_dx = state->mouse_dx;
    float mouse_dy = state->mouse_dy;
    state->mouse_dx = 0.0f;
    state->mouse_dy = 0.0f;

    // Mesh path animation: walk between consecutive refview nodes and loop forever.
    if (mesh_path && refviews_loaded && refviews.count >= 2) {
        uint32_t a = anim_node % refviews.count;
        uint32_t b = (a + 1) % refviews.count;
        const float* pa = refviews.views[a].position;
        const float* pb = refviews.views[b].position;
        float dx = pb[0] - pa[0], dy = pb[1] - pa[1], dz = pb[2] - pa[2];
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        float dur = (dist > 1e-6f) ? (dist / anim_speed) : 0.1f;

        anim_t += dt / dur;
        // Advance through nodes if we passed multiple segments in one frame.
        int safety = (int)refviews.count + 1;
        while (anim_t >= 1.0f && safety-- > 0) {
            anim_t -= 1.0f;
            anim_node = (anim_node + 1) % refviews.count;
            a = anim_node;
            b = (a + 1) % refviews.count;
            pa = refviews.views[a].position;
            pb = refviews.views[b].position;
            dx = pb[0] - pa[0]; dy = pb[1] - pa[1]; dz = pb[2] - pa[2];
            dist = sqrtf(dx*dx + dy*dy + dz*dz);
            dur = (dist > 1e-6f) ? (dist / anim_speed) : 0.1f;
        }

        float t = anim_t;
        renderer.mesh_transform.translation[0] = pa[0] + dx * t;
        renderer.mesh_transform.translation[1] = pa[1] + dy * t + anim_y_offset;
        renderer.mesh_transform.translation[2] = pa[2] + dz * t;

        // Yaw faces direction of travel (matches camera yaw convention: yaw=0 -> +Z).
        float horiz2 = dx*dx + dz*dz;
        if (horiz2 > 1e-10f) {
            float target_yaw = atan2f(dx, dz);
            if (!anim_yaw_initialized) {
                anim_yaw = target_yaw;
                anim_yaw_initialized = true;
            } else {
                float diff = target_yaw - anim_yaw;
                const float PI = 3.14159265358979f;
                while (diff >  PI) diff -= 2.0f * PI;
                while (diff < -PI) diff += 2.0f * PI;
                float k = dt * 5.0f;
                if (k > 1.0f) k = 1.0f;
                anim_yaw += diff * k;
            }
            renderer.mesh_transform.rotation_euler[1] = anim_yaw;
        }
    }

    // Update reference view interpolation (locks camera input while active)
    bool camera_locked = refview_update(&refviews, &cam, dt);

    // Drive the examine lerp; while examining, FPS controls are fully
    // muted (no mouse look, no WASD) to keep the cam stationary for the
    // eventual return-lerp.
    bool examine_active = examine_locks_input(examine);
    examine_tick(&examine, &renderer.object_transform, dt);

    // While ACTIVE, mouse drag orbits the object around its AABB center
    // and the wheel zooms (handled in the wheel event). Composes a
    // world-up yaw with a fixed-axis pitch on top of the entry rotation.
    if (examine.state == Examine::ACTIVE) {
        examine.orbit_yaw   -= mouse_dx * EXAMINE_ORBIT_SENSITIVITY;
        examine.orbit_pitch += mouse_dy * EXAMINE_ORBIT_SENSITIVITY;
        if (examine.orbit_pitch >  EXAMINE_PITCH_LIMIT) examine.orbit_pitch =  EXAMINE_PITCH_LIMIT;
        if (examine.orbit_pitch < -EXAMINE_PITCH_LIMIT) examine.orbit_pitch = -EXAMINE_PITCH_LIMIT;

        // R_orbit = R_yaw(world_up) * R_pitch(cam_right_entry).
        float wu[3] = { 0.0f, 1.0f, 0.0f };
        float Ry[9], Rp[9], R_orbit[9];
        mat3_axis_angle(wu,                       examine.orbit_yaw,   Ry);
        mat3_axis_angle(examine.cam_right_entry,  examine.orbit_pitch, Rp);
        mat3_mul(Ry, Rp, R_orbit);

        // Compose with the base rotation captured at examine entry.
        float R_base[9], R_new[9];
        mat3_from_euler_zyx(examine.base_rotation_euler, R_base);
        mat3_mul(R_orbit, R_base, R_new);

        // Write Euler back into object_transform.
        mat3_decompose_zyx(R_new, renderer.object_transform.rotation_euler);

        // Place the AABB center at the (zoom-adjusted) pivot along the
        // entry forward, then back the translation off by the rotated
        // local center so the pivot stays nailed there.
        float s = renderer.object_transform.scale;
        float cl[3] = {
            examine.aabb_center_local[0] * s,
            examine.aabb_center_local[1] * s,
            examine.aabb_center_local[2] * s,
        };
        float cw[3] = {
            R_new[0]*cl[0] + R_new[3]*cl[1] + R_new[6]*cl[2],
            R_new[1]*cl[0] + R_new[4]*cl[1] + R_new[7]*cl[2],
            R_new[2]*cl[0] + R_new[5]*cl[1] + R_new[8]*cl[2],
        };
        float dist = examine.dist_base * examine.distance_scale;
        renderer.object_transform.translation[0] = cam.position[0] + examine.cam_fwd_entry[0]*dist - cw[0];
        renderer.object_transform.translation[1] = cam.position[1] + examine.cam_fwd_entry[1]*dist - cw[1];
        renderer.object_transform.translation[2] = cam.position[2] + examine.cam_fwd_entry[2]*dist - cw[2];
    }

    // Update camera (allow mouse look during lerp, but block WASD movement)
    if (examine_active) {
        camera_update(&cam, keys, 0.0f, 0.0f, 0.0f);
    } else if (camera_locked) {
        camera_update(&cam, keys, mouse_dx, mouse_dy, 0);
    } else if (cam.camera_mode || !ImGui::GetIO().WantCaptureKeyboard) {
        camera_update(&cam, keys, mouse_dx, mouse_dy, dt);
    } else {
        camera_update(&cam, keys, mouse_dx, mouse_dy, 0);
    }

    // Get window size
    int win_w, win_h;
    SDL_GetWindowSize(window, &win_w, &win_h);
    float aspect = (float)win_w / (float)win_h;

    // Animate ortho blend toward target. Skipped while an inspect lerp is
    // active: refview_update drives ortho_blend from the same eased t as
    // the position lerp so both finish together (avoids the zoom wobble
    // that an independent timer caused, especially with ortho_size < 1).
    //
    // NOTE: A more involved fix would replace the element-wise lerp in
    // camera_get_proj_matrix with a parameter-based interpolation (e.g.
    // gradually collapse the perspective frustum into an ortho box of the
    // chosen ortho_size, or lerp an effective focal length and rebuild a
    // single coherent matrix). That would make the apparent scale at the
    // focused subject strictly monotonic and remove the residual
    // nonlinearity from blending m[11] (-1 -> 0). Synchronizing the
    // timers, as we do here, is good enough in practice.
    if (!(refviews_loaded && refviews.lerping && refviews.inspect_mode)) {
        float target = cam.orthographic ? 1.0f : 0.0f;
        float blend_speed = 1.0f; // 1/speed seconds for full transition
        if (cam.ortho_blend < target) {
            cam.ortho_blend += blend_speed * dt;
            if (cam.ortho_blend > target) cam.ortho_blend = target;
        } else if (cam.ortho_blend > target) {
            cam.ortho_blend -= blend_speed * dt;
            if (cam.ortho_blend < target) cam.ortho_blend = target;
        }
    }

    // Build camera uniforms
    CameraUniforms cam_uniforms = {};
    camera_get_view_matrix(&cam, cam_uniforms.view);
    camera_get_proj_matrix(&cam, aspect, cam_uniforms.proj);
    cam_uniforms.viewport[0] = (float)win_w;
    cam_uniforms.viewport[1] = (float)win_h;
    cam_uniforms.orthographic = cam.ortho_blend;
    // Provide pure persp/ortho focal lengths so the splat shader can use
    // each in its own mix() branch (see comment in camera.h).
    cam_uniforms.persp_focal = (1.0f / tanf(cam.fov_y * 0.5f)) * (float)win_h * 0.5f;
    cam_uniforms.ortho_focal = (float)win_h / (2.0f * cam.ortho_size);

    // Cull + sort
    if (scene_loaded) {
        cull_gaussians(&scene, cam_uniforms.view, cam_uniforms.proj, cam.ortho_blend);

        if (scene.visible_count > 0) {
            SortContext sort_ctx = {};
            sort_ctx.depths = scene.visible_depths;
            sort_ctx.input_indices = scene.visible_indices;
            sort_ctx.count = scene.visible_count;
            sort_ctx.sorted_indices = scene.sorted_indices;
            sort_ctx.scratch_indices = scene.scratch_indices;
            sort_ctx.scratch_keys = scene.scratch_keys;
            sort_ctx.scratch_keys2 = scene.scratch_keys2;
            sort_gaussians(&sort_ctx);
        }
    }

    // ImGui frame. simgui_new_frame calls ImGui::NewFrame internally and
    // sets DisplaySize/DeltaTime. ImGui_ImplSDL3_NewFrame still runs before
    // it to forward mouse/keyboard state into ImGui IO.
    ImGui_ImplSDL3_NewFrame();
    simgui_new_frame({ win_w, win_h, dt, 1.0f });

    ImGui::Begin("Info");
    ImGui::Text("FPS: %.1f", dt > 0 ? 1.0f / dt : 0.0f);
    if (scene_loaded) {
        ImGui::Text("Visible: %u / %u", scene.visible_count, scene.gaussian_count);
    }
    ImGui::Text("Camera: %.1f, %.1f, %.1f  yaw %.3f  pitch %.3f",
                cam.position[0], cam.position[1], cam.position[2],
                cam.yaw, cam.pitch);

    // Cursor UV on the active overlay panorama (matches .hotspots shape.points).
    // Only meaningful while the overlay is visible (same gate as overlay alpha).
    if (refviews_loaded && refviews.current_node >= 0) {
        RefView* cv = &refviews.views[refviews.current_node];
        float dx0 = cam.position[0] - cv->position[0];
        float dy0 = cam.position[1] - cv->position[1];
        float dz0 = cam.position[2] - cv->position[2];
        float d2  = dx0*dx0 + dy0*dy0 + dz0*dz0;
        if (d2 < 0.01f) {
            // In FPS mode the cursor is captured -> use the crosshair (screen
            // center). In cursor mode use the actual mouse position.
            float mx, my;
            if (cam.camera_mode) {
                mx = (float)win_w * 0.5f;
                my = (float)win_h * 0.5f;
            } else {
                ImVec2 mp = ImGui::GetMousePos();
                mx = mp.x;
                my = mp.y;
            }
            // Pixel -> Vulkan NDC (matches overlay.vert.glsl: top y=-1).
            float ndc_x = 2.0f * mx / (float)win_w - 1.0f;
            float ndc_y = 2.0f * my / (float)win_h - 1.0f;

            float cam_basis[16];
            float cam_tan[2];
            camera_get_overlay_ray_basis(&cam, (float)win_w / (float)win_h,
                                         cam_basis, cam_tan);

            // NDC -> camera-space ray (matches overlay.frag.glsl).
            float cdx = ndc_x * cam_tan[0];
            float cdy = -ndc_y * cam_tan[1];
            float cdz = 1.0f;
            float clen = sqrtf(cdx*cdx + cdy*cdy + cdz*cdz);
            cdx /= clen; cdy /= clen; cdz /= clen;

            // Camera-space -> world-space (mat3(cam_basis) * cdir, column-major).
            float wx = cam_basis[0]*cdx + cam_basis[4]*cdy + cam_basis[8] *cdz;
            float wy = cam_basis[1]*cdx + cam_basis[5]*cdy + cam_basis[9] *cdz;
            float wz = cam_basis[2]*cdx + cam_basis[6]*cdy + cam_basis[10]*cdz;

            // World -> ref-camera frame (mat3(ref_rot) * w, column-major).
            float ref_rot[16];
            refview_get_rotation_matrix(cv, ref_rot);
            float rx = ref_rot[0]*wx + ref_rot[4]*wy + ref_rot[8] *wz;
            float ry = ref_rot[1]*wx + ref_rot[5]*wy + ref_rot[9] *wz;
            float rz = ref_rot[2]*wx + ref_rot[6]*wy + ref_rot[10]*wz;
            float rlen = sqrtf(rx*rx + ry*ry + rz*rz);
            if (rlen > 1e-8f) { rx /= rlen; ry /= rlen; rz /= rlen; }

            const float PI = 3.14159265358979f;
            float ry_c = ry < -1.0f ? -1.0f : (ry > 1.0f ? 1.0f : ry);
            float u = atan2f(rx, rz) / (2.0f * PI) + 0.5f;
            float v = -asinf(ry_c) / PI + 0.5f;
            ImGui::Text("Cursor UV: %.4f, %.4f", u, v);
        }
    }

    ImGui::Text("Speed: %.1f", cam.move_speed);
    ImGui::Checkbox("Orthographic", &cam.orthographic);
    if (cam.orthographic) {
        ImGui::SliderFloat("Ortho Size", &cam.ortho_size, 0.5f, 5.0f);
    } else {
        float fov_deg = cam.fov_y * (180.0f / 3.14159265358979f);
        if (ImGui::SliderFloat("FOV", &fov_deg, 10.0f, 170.0f, "%.0f°")) {
            cam.fov_y = fov_deg * (3.14159265358979f / 180.0f);
        }
    }
    if (refviews_loaded) {
        ImGui::SliderFloat("Ref View Opacity", &refview_max_alpha, 0.0f, 1.0f);
        ImGui::Checkbox("Use Covisibility", &refviews.use_covisibility);
        if (refviews.use_covisibility) {
            ImGui::SliderInt("Min Inliers", &refviews.min_inliers, 0, 500);
        } else {
            ImGui::SliderFloat("Neighbor Radius", &refviews.neighbor_radius, 0.5f, 10.0f);
        }
        ImGui::Checkbox("Show Node Boxes", &show_node_boxes);
        ImGui::SliderFloat("Node Box Size", &node_half_size, 0.1f, 1.0f);
        ImGui::Checkbox("Show Hotspot Debug", &show_hotspot_debug);
        ImGui::SliderFloat("Transition Speed", &refviews.lerp_speed, 1.0f, 10.0f);
        if (refviews.current_node >= 0) {
            ImGui::Text("Current Node: %d", refviews.current_node);
            ImGui::Text("Neighbors: %u", neighbor_count);
        }
    }
    ImGui::End();

    if (refviews_loaded) {
        ImGui::Begin("Reference Views");
        for (uint32_t i = 0; i < refviews.count; i++) {
            char label[32];
            snprintf(label, sizeof(label), "%u", i);
            bool is_selected = ((int32_t)i == refviews.selected);
            if (ImGui::Selectable(label, is_selected)) {
                RefView* tv = &refviews.views[i];
                float dx = tv->position[0] - cam.position[0];
                float dy = tv->position[1] - cam.position[1];
                float dz = tv->position[2] - cam.position[2];
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                refviews.selected = i;
                refviews.lerping = true;
                refviews.lerp_t = 0.0f;
                refviews.lerp_duration = (dist > 1e-6f) ? dist / refviews.lerp_speed : 0.1f;
                refviews.start_pos[0] = cam.position[0];
                refviews.start_pos[1] = cam.position[1];
                refviews.start_pos[2] = cam.position[2];
                refviews.start_yaw = cam.yaw;
                refviews.start_pitch = cam.pitch;
            }
        }
        ImGui::End();
    }

    if (mesh_path) {
        ImGui::Begin("Mesh Transform");
        MeshTransform& mt = renderer.mesh_transform;
        ImGui::DragFloat3("Translation", mt.translation, 0.01f);
        float rot_deg[3] = {
            mt.rotation_euler[0] * 57.2957795f,
            mt.rotation_euler[1] * 57.2957795f,
            mt.rotation_euler[2] * 57.2957795f,
        };
        if (ImGui::DragFloat3("Rotation (deg)", rot_deg, 0.5f, -360.0f, 360.0f)) {
            mt.rotation_euler[0] = rot_deg[0] * 0.0174532925f;
            mt.rotation_euler[1] = rot_deg[1] * 0.0174532925f;
            mt.rotation_euler[2] = rot_deg[2] * 0.0174532925f;
        }
        ImGui::DragFloat("Scale", &mt.scale, 0.01f, 0.001f, 1000.0f);
        if (ImGui::Button("Reset")) {
            mt.translation[0] = mt.translation[1] = mt.translation[2] = 0.0f;
            mt.rotation_euler[0] = mt.rotation_euler[1] = mt.rotation_euler[2] = 0.0f;
            mt.scale = 1.0f;
        }
        ImGui::End();
    }

    // Draw crosshair in camera mode (highlight when aiming at a node,
    // or show an upward arrow when aiming at a hotspot on the overlay).
    // Suppressed during examine: the object fills the view, so a hover
    // icon would just signal "you can examine what you're already
    // examining". The camera is also locked, so picks are meaningless.
    if (cam.camera_mode && !examine_active) {
        bool crosshair_hover = false;
        bool hotspot_hover = false;

        // Hotspot hover takes precedence over neighbor-node hover.
        // Mirrors the click-time pick logic above.
        if (refviews_loaded && !refviews.lerping && refviews.current_node >= 0) {
            RefView* cv = &refviews.views[refviews.current_node];
            if (cv->hotspot_count > 0) {
                float dx0 = cam.position[0] - cv->position[0];
                float dy0 = cam.position[1] - cv->position[1];
                float dz0 = cam.position[2] - cv->position[2];
                float d2  = dx0*dx0 + dy0*dy0 + dz0*dz0;
                if (d2 < 0.01f) {
                    float forward[3];
                    camera_get_forward(&cam, forward);
                    float R[16];
                    refview_get_rotation_matrix(cv, R);
                    float rx = R[0]*forward[0] + R[4]*forward[1] + R[8] *forward[2];
                    float ry = R[1]*forward[0] + R[5]*forward[1] + R[9] *forward[2];
                    float rz = R[2]*forward[0] + R[6]*forward[1] + R[10]*forward[2];
                    const float PI = 3.14159265358979f;
                    float u = atan2f(rx, rz) / (2.0f * PI) + 0.5f;
                    float ry_c = ry < -1.0f ? -1.0f : (ry > 1.0f ? 1.0f : ry);
                    float v = -asinf(ry_c) / PI + 0.5f;
                    if (hotspot_pick(cv, u, v) >= 0) hotspot_hover = true;
                }
            }
        }

        // Depth-based pick between object AABB and neighbor node boxes;
        // hotspot already wins absolutely above. Closer of (object, node)
        // wins; node hover paints the cyan circle, object hover paints the
        // magnifying glass.
        bool object_hover = false;
        if (!hotspot_hover) {
            float forward[3];
            camera_get_forward(&cam, forward);

            float object_t = 1e30f;
            if (object_path) {
                float obj_model[16];
                mat4_from_transform(renderer.object_transform, obj_model);
                float obmin[3], obmax[3];
                mesh_aabb_world(object.aabb_min, object.aabb_max, obj_model, obmin, obmax);
                float t;
                if (ray_aabb(cam.position, forward, obmin, obmax, &t)) object_t = t;
            }

            float node_t = 1e30f;
            if (refviews_loaded && !refviews.lerping && neighbor_count > 0) {
                for (uint32_t ni = 0; ni < neighbor_count; ni++) {
                    const float* c = &neighbor_positions[ni*3];
                    float hs = node_half_size;
                    float bmin[3] = { c[0]-hs, c[1]-hs, c[2]-hs };
                    float bmax[3] = { c[0]+hs, c[1]+hs, c[2]+hs };
                    float t;
                    if (ray_aabb(cam.position, forward, bmin, bmax, &t) && t < node_t) {
                        node_t = t;
                    }
                }
            }

            if (object_t < 1e30f && object_t <= node_t) {
                object_hover = true;
            } else if (node_t < 1e30f) {
                crosshair_hover = true;
            }
        }

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 center(win_w * 0.5f, win_h * 0.5f);
        if (hotspot_hover) {
            // Myst-style upward pointing arrow.
            ImU32 fill_col    = IM_COL32(255, 230, 80, 240);
            ImU32 outline_col = IM_COL32(0,   0,   0,   220);
            ImVec2 tip(center.x,        center.y - 10.0f);
            ImVec2 lwing(center.x - 8.0f, center.y + 1.0f);
            ImVec2 rwing(center.x + 8.0f, center.y + 1.0f);
            ImVec2 lstem(center.x - 3.0f, center.y + 1.0f);
            ImVec2 rstem(center.x + 3.0f, center.y + 1.0f);
            ImVec2 lbase(center.x - 3.0f, center.y + 9.0f);
            ImVec2 rbase(center.x + 3.0f, center.y + 9.0f);
            // Filled arrow: head triangle + rectangular stem.
            dl->AddTriangleFilled(tip, lwing, rwing, fill_col);
            dl->AddQuadFilled(lstem, rstem, rbase, lbase, fill_col);
            // Outline (head + stem sides + base).
            dl->AddLine(tip,   lwing, outline_col, 1.5f);
            dl->AddLine(tip,   rwing, outline_col, 1.5f);
            dl->AddLine(lwing, lstem, outline_col, 1.5f);
            dl->AddLine(rwing, rstem, outline_col, 1.5f);
            dl->AddLine(lstem, lbase, outline_col, 1.5f);
            dl->AddLine(rstem, rbase, outline_col, 1.5f);
            dl->AddLine(lbase, rbase, outline_col, 1.5f);
        } else if (object_hover) {
            // Magnifying-glass icon: hollow ring + diagonal handle.
            ImU32 col_fill    = IM_COL32(255, 230, 80, 240);
            ImU32 col_outline = IM_COL32(0,   0,   0,   220);
            ImVec2 ring_c(center.x - 2.0f, center.y - 2.0f);
            float  ring_r = 6.0f;
            dl->AddCircle(ring_c, ring_r,        col_fill,    24, 2.0f);
            dl->AddCircle(ring_c, ring_r + 1.0f, col_outline, 24, 1.0f);
            dl->AddCircle(ring_c, ring_r - 1.0f, col_outline, 24, 1.0f);
            // Handle: from ring edge diagonally down-right.
            float k = 0.7071f; // cos/sin 45
            ImVec2 h0(ring_c.x + ring_r * k,        ring_c.y + ring_r * k);
            ImVec2 h1(ring_c.x + (ring_r + 6.0f) * k, ring_c.y + (ring_r + 6.0f) * k);
            dl->AddLine(h0, h1, col_outline, 3.5f);
            dl->AddLine(h0, h1, col_fill,    2.0f);
        } else if (crosshair_hover) {
            dl->AddCircleFilled(center, 5.0f, IM_COL32(0, 200, 255, 240));
            dl->AddCircle(center, 8.0f, IM_COL32(0, 200, 255, 120), 0, 1.5f);
        } else {
            dl->AddCircleFilled(center, 3.0f, IM_COL32(255, 255, 255, 200));
        }
    }

    // Hotspot debug overlay: project authored polygons of the current
    // refview onto the screen by inverting the overlay shader pipeline.
    if (show_hotspot_debug && refviews_loaded && refviews.current_node >= 0) {
        RefView* cv = &refviews.views[refviews.current_node];
        if (cv->hotspot_count > 0) {
            float cam_basis[16];
            float cam_tan[2];
            camera_get_overlay_ray_basis(&cam, (float)win_w / (float)win_h, cam_basis, cam_tan);
            float ref_rot[16];
            refview_get_rotation_matrix(cv, ref_rot);
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const float PI = 3.14159265358979f;

            ImVec2 pts_buf[256];

            for (uint32_t hi = 0; hi < cv->hotspot_count; hi++) {
                const Hotspot* h = &cv->hotspots[hi];
                if (h->type != HOTSPOT_SHAPE_POLYGON) continue;
                if (h->polygon.count < 3) continue;

                // Deterministic per-hotspot color via hash -> hue.
                uint32_t k = (uint32_t)refviews.current_node * 2654435761u + hi * 2246822519u;
                float hue = (float)(k & 0xFFFF) / 65535.0f; // [0,1)
                // HSV(h, 0.85, 1.0) -> RGB
                float hh = hue * 6.0f;
                int hi_ = (int)hh;
                float ff = hh - (float)hi_;
                float p = 1.0f * (1.0f - 0.85f);
                float q = 1.0f * (1.0f - 0.85f * ff);
                float t = 1.0f * (1.0f - 0.85f * (1.0f - ff));
                float r=0, g=0, b=0;
                switch (hi_ % 6) {
                    case 0: r=1.0f; g=t;    b=p;    break;
                    case 1: r=q;    g=1.0f; b=p;    break;
                    case 2: r=p;    g=1.0f; b=t;    break;
                    case 3: r=p;    g=q;    b=1.0f; break;
                    case 4: r=t;    g=p;    b=1.0f; break;
                    case 5: r=1.0f; g=p;    b=q;    break;
                }
                ImU32 line_col = IM_COL32((int)(r*255), (int)(g*255), (int)(b*255), 230);
                ImU32 fill_col = IM_COL32((int)(r*255), (int)(g*255), (int)(b*255), 60);

                uint32_t n = h->polygon.count;
                if (n > 256) n = 256;
                bool any_behind = false;
                for (uint32_t i = 0; i < n; i++) {
                    float u = h->polygon.points[i][0];
                    float v = h->polygon.points[i][1];

                    // UV -> ref-camera direction (matches overlay.frag.glsl).
                    float lon = (u - 0.5f) * 2.0f * PI;
                    float vp  = (v - 0.5f) * PI;
                    float cb  = cosf(vp);
                    float rdx = cb * sinf(lon);
                    float rdy = -sinf(vp);
                    float rdz = cb * cosf(lon);

                    // ref-camera dir -> world dir: transpose(ref_rot) * rd
                    float wx = ref_rot[0]*rdx + ref_rot[1]*rdy + ref_rot[2] *rdz;
                    float wy = ref_rot[4]*rdx + ref_rot[5]*rdy + ref_rot[6] *rdz;
                    float wz = ref_rot[8]*rdx + ref_rot[9]*rdy + ref_rot[10]*rdz;

                    // world dir -> camera dir: transpose(cam_basis) * w
                    float cx = cam_basis[0]*wx + cam_basis[1]*wy + cam_basis[2] *wz;
                    float cy = cam_basis[4]*wx + cam_basis[5]*wy + cam_basis[6] *wz;
                    float cz = cam_basis[8]*wx + cam_basis[9]*wy + cam_basis[10]*wz;

                    if (cz <= 1e-4f) { any_behind = true; break; }

                    // camera dir -> NDC (inverse of overlay.frag construction).
                    // Matches OpenGL NDC (y-up: +1 at top of framebuffer);
                    // the overlay shader now uses camera_dir.y = +v_ndc.y * tan,
                    // so we invert that here without a sign flip.
                    float ndc_x = (cx / cz) / cam_tan[0];
                    float ndc_y = (cy / cz) / cam_tan[1];

                    pts_buf[i].x = (ndc_x * 0.5f + 0.5f) * (float)win_w;
                    pts_buf[i].y = (1.0f - (ndc_y * 0.5f + 0.5f)) * (float)win_h;
                }
                if (any_behind) continue;

                dl->AddConvexPolyFilled(pts_buf, (int)n, fill_col);
                dl->AddPolyline(pts_buf, (int)n, line_col, ImDrawFlags_Closed, 2.0f);
            }
        }
    }

    // simgui_render() (called inside renderer_draw_frame's pass) calls
    // ImGui::Render() itself, so we don't call it here.

    // Find closest refview node to camera (used for overlay + current_node tracking)
    OverlayParams overlay = {};
    OverlayParams* overlay_ptr = NULL;
    if (refviews_loaded) {
        float best_dist2 = 1e30f;
        int best_idx = -1;
        for (uint32_t i = 0; i < refviews.count; i++) {
            if (!refviews.views[i].texture.id) continue;
            float dx = cam.position[0] - refviews.views[i].position[0];
            float dy = cam.position[1] - refviews.views[i].position[1];
            float dz = cam.position[2] - refviews.views[i].position[2];
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < best_dist2) { best_dist2 = d2; best_idx = (int)i; }
        }

        refviews.current_node = best_idx;

        if (best_idx >= 0 && refview_max_alpha > 0.0f) {
            RefView* rv = &refviews.views[best_idx];
            float dist = sqrtf(best_dist2);
            float fade_dist = 0.1f;
            float alpha = 1.0f - dist / fade_dist;
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > refview_max_alpha) alpha = refview_max_alpha;

            if (alpha > 0.0f) {
                overlay.texture = rv->texture;
                overlay.texture_view = rv->texture_view;
                overlay.alpha = alpha;

                camera_get_overlay_ray_basis(&cam, (float)win_w / (float)win_h,
                                             overlay.camera_ray_basis,
                                             overlay.camera_tan_half_fov);

                refview_get_rotation_matrix(rv, overlay.ref_rotation);
                overlay_ptr = &overlay;
            }
        }

        // Collect neighbor nodes for wireframe rendering + click targets
        neighbor_count = refview_get_neighbors(&refviews, neighbor_positions, neighbor_indices, max_neighbors);
    }

    // Build node render params
    NodeRenderParams node_params = {};
    NodeRenderParams* node_ptr = NULL;
    if (refviews_loaded && neighbor_count > 0 && show_node_boxes) {
        node_params.positions = neighbor_positions;
        node_params.count = neighbor_count;
        node_params.half_size = node_half_size;
        node_ptr = &node_params;
    }

    // Build map-camera uniforms when the top-down overlay is active.
    CameraUniforms  map_uniforms = {};
    CameraUniforms* map_uniforms_ptr = NULL;
    if (map_view_active) {
        camera_get_view_matrix(&map_cam, map_uniforms.view);
        camera_get_proj_matrix(&map_cam, aspect, map_uniforms.proj);
        map_uniforms.viewport[0]   = (float)win_w;
        map_uniforms.viewport[1]   = (float)win_h;
        map_uniforms.orthographic  = map_cam.ortho_blend;
        map_uniforms.persp_focal   = (1.0f / tanf(map_cam.fov_y * 0.5f)) * (float)win_h * 0.5f;
        map_uniforms.ortho_focal   = (float)win_h / (2.0f * map_cam.ortho_size);
        map_uniforms_ptr = &map_uniforms;
    }

    // Render
    renderer_draw_frame(&renderer, &scene, &cam_uniforms, overlay_ptr, node_ptr,
                        1.0f /*wireframe_occlusion*/, map_uniforms_ptr);


    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)result;
    AppState* state = (AppState*)appstate;
    if (!state) return;

    // Tear down GPU resources before sg_shutdown (refview images, renderer
    // pipelines, etc. all live in the sokol_gfx pools).
    if (state->scene_loaded) free_scene(&state->scene);
    if (state->mesh_path) mesh_free(&state->mesh);
    if (state->object_path) mesh_free(&state->object);
    if (state->refviews_loaded) {
        refview_release_images(&state->refviews);
        refview_free(&state->refviews);
    }
    if (state->renderer_started) renderer_destroy(&state->renderer);

    if (state->imgui_sdl3_initialized) ImGui_ImplSDL3_Shutdown();
    if (state->simgui_setup_done) simgui_shutdown(); // destroys ImGui context
    if (state->sg_setup_done) sg_shutdown();

    if (state->gl_context) SDL_GL_DestroyContext(state->gl_context);
    if (state->window) SDL_DestroyWindow(state->window);

    sfx_free(&state->sfx_transition);
    audio_shutdown();

    delete state;
}
