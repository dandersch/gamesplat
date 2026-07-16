#include <SDL3/SDL.h>
#include <cstdio>

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_args.h"
#include "imgui.h"
#include "sokol_imgui.h"
#if defined(ENABLE_PROFILER)
#include "sokol_gfx_imgui.h"
#endif

#include "profiler.h"
#include "maths.h"
#include "log.h"
int log_verbosity_level = LOG_EVERYTHING; /* define globally once */

/* unity build */
#include "camera.cpp"
#include "gaussian.cpp"
#include "gaussian_loader_sog.cpp"
#include "gaussian_loader_ply.cpp"
#include "mesh.cpp"
#include "mesh_loader_obj.cpp"
#include "mesh_loader_gltf.cpp"
#include "renderer.cpp"
#include "examine.cpp"
#include "hotspot.cpp"
#include "colmap.cpp"
#include "refview.cpp"

#if defined(_MSC_VER)
#define GSPLAT_EXPORT extern "C" __declspec(dllexport)
#else
#define GSPLAT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

static int g_argc = 0;
static char** g_argv = NULL;

static float g_app_time = 0.0f;
static float g_splat_effect_start_time = 0.0f;
static float g_splat_effect_duration = 2.25f;
static float g_splat_effect_strength = 0.08f;
static float g_splat_effect_tint_strength = 0.45f;
static bool  g_splat_effect_active = false;
static float g_splat_effect_center[3] = { 0.0f, 0.0f, 0.0f };
static float g_splat_effect_radius = 1.0f;

static const uint32_t MAX_NEIGHBORS = 64;

struct AppState {
    const char* ply_path;
    const char* colmap_dir;
    const char* mesh_path;
    const char* object_path;

    bool           sg_setup_done;
    bool           simgui_setup_done;
    bool           sgimgui_setup_done;
    bool           renderer_started;
    ImGuiContext*  imgui_context;

    Renderer      renderer;
    GaussianScene scene;
    bool          scene_loaded;
    Mesh          mesh;
    Mesh          object;
    RefViewSet    refviews;
    bool          refviews_loaded;

    Camera cam;
    bool   keys[9]; // W A S D Space LCtrl LShift E Q
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

    // Accumulated by app_event and consumed once per app_frame.
    float mouse_dx;
    float mouse_dy;
};

static AppState* g_state = NULL;

GSPLAT_EXPORT void* gsplat_app_state_save(void) {
    return g_state;
}

GSPLAT_EXPORT void gsplat_app_state_load(void* state) {
    g_state = (AppState*)state;
}

GSPLAT_EXPORT void gsplat_hot_reload_after_state_restore(void) {
    if (g_state && g_state->imgui_context) {
        ImGui::SetCurrentContext(g_state->imgui_context);
    }
}

static void app_init(void) {
    AppState* state = new AppState();
    g_state = state;

    sargs_desc args = {};
    args.argc = g_argc;
    args.argv = g_argv;
    sargs_setup(&args);

    state->ply_path    = sargs_value("ply");
    state->colmap_dir  = sargs_value("colmap");
    state->mesh_path   = sargs_value("mesh");
    state->object_path = sargs_value("object");

    // Set defaults. Native and web builds can override these with sokol_args:
    //   ./gsplat ply=res/export_n01.sog colmap=res/colmap mesh=res/foo.glb
    //   index.html?ply=res/export_n01.sog&colmap=res/colmap&mesh=res/foo.glb
    state->ply_path    = state->ply_path[0]    ? state->ply_path    : "res/export_n01.sog";
    //state->object_path = state->object_path ? state->object_path : "res/priest.glb";
    //state->mesh_path   = state->mesh_path   ? state->mesh_path   : "res/cyberpunk_guy.glb";
    state->colmap_dir  = state->colmap_dir[0]  ? state->colmap_dir  : "res/colmap";
    state->mesh_path   = state->mesh_path[0]   ? state->mesh_path   : NULL;
    state->object_path = state->object_path[0] ? state->object_path : NULL;

    sg_desc sgd = {};
    sgd.environment = sglue_environment();
    sgd.logger.func = slog_func;
    sg_setup(&sgd);
    if (!sg_isvalid()) {
        LOG(ERROR|RENDERER|INIT, "sg_setup failed");
        sapp_quit();
        return;
    }
    state->sg_setup_done = true;

    // sokol_imgui creates the ImGui context itself, so we don't call
    // ImGui::CreateContext().
    IMGUI_CHECKVERSION();
    simgui_desc_t sid = {};
    sid.color_format = renderer_default_color_format();
    sid.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    sid.sample_count = 1;
    simgui_setup(&sid);
    state->simgui_setup_done = true;
    state->imgui_context = ImGui::GetCurrentContext();

#if defined(ENABLE_PROFILER)
    sgimgui_desc_t sgimgui_desc = {};
    sgimgui_setup(&sgimgui_desc);
    state->sgimgui_setup_done = true;
#endif

    // Renderer (no more SDL_GPUDevice; sokol_gfx is set up globally).
    state->renderer_started = true;
    if (!renderer_init(&state->renderer)) {
        LOG(ERROR|RENDERER|INIT, "Renderer init failed");
        sapp_quit();
        return;
    }

    // Scene
    if (state->ply_path) {
        PROFILE("load gaussian scene") {
        state->scene_loaded = load_gaussian_scene(state->ply_path, &state->scene);
        }
        if (state->scene_loaded) {
            // compute gaussian scene radius from center
            {
                if (state->scene.gaussian_count == 0) {
                    g_splat_effect_radius = 1.0f;
                    return;
                }

                float max_r2 = 0.0f;
                for (uint32_t i = 0; i < state->scene.gaussian_count; ++i) {
                    const float* p = state->scene.gaussians[i].position;
                    float dx = p[0] - g_splat_effect_center[0];
                    float dy = p[1] - g_splat_effect_center[1];
                    float dz = p[2] - g_splat_effect_center[2];
                    float r2 = dx*dx + dy*dy + dz*dz;
                    if (r2 > max_r2) max_r2 = r2;
                }

                float r = sqrtf(max_r2);
                g_splat_effect_radius = r > 1e-4f ? r : 1.0f;
            }
            PROFILE("upload gaussian scene") {
            renderer_upload_gaussians(&state->renderer, &state->scene);
            }
        }
    }

    // Mesh
    if (state->mesh_path) {
        PROFILE("load mesh") {
        if (mesh_load(state->mesh_path, &state->mesh)) {
            PROFILE("upload mesh") {
            renderer_upload_mesh(&state->renderer, &state->mesh);
            }
        }
        }
    }

    // Static scene object (no animation, identity transform)
    if (state->object_path) {
        PROFILE("load object mesh") {
        if (mesh_load(state->object_path, &state->object)) {
            PROFILE("upload object mesh") {
            renderer_upload_object_mesh(&state->renderer, &state->object);
            }
        }
        }
    }

    // Reference views
    state->refviews.selected = -1;
    if (state->colmap_dir) {
        ColmapPaths colmap_paths = {};
        ColmapImageSet colmap_images = {};
        ColmapCovisibility colmap_covis = {};
        PROFILE("load reference views") {
        state->refviews_loaded = colmap_resolve_paths(&colmap_paths, state->colmap_dir)
                              && colmap_load_images_txt(&colmap_images, colmap_paths.model_dir)
                              && refview_load(&state->refviews, &colmap_images, colmap_paths.image_dir);
        }
        if (state->refviews_loaded) {
            PROFILE("load covisibility") {
            if (colmap_load_covisibility(&colmap_covis, colmap_paths.database_path)) {
                refview_load_covisibility(&state->refviews, &colmap_covis);
            }
            }
            PROFILE("load reference images") {
            refview_load_images(&state->refviews);
            }
            PROFILE("load hotspots") {
            hotspot_load_for_set(&state->refviews);
            }
        }
        colmap_free_covisibility(&colmap_covis);
        colmap_free_image_set(&colmap_images);
    }

    // Camera
    camera_init(&state->cam);
    sapp_lock_mouse(true); // start in camera mode
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;

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

    return;
}

static void app_event(const sapp_event* ev) {
    AppState* state = g_state;
    if (!state || !ev) return;

    simgui_handle_event(ev);

    switch (ev->type) {
    case SAPP_EVENTTYPE_QUIT_REQUESTED:
        sapp_quit();
        return;
    case SAPP_EVENTTYPE_KEY_DOWN:
    case SAPP_EVENTTYPE_KEY_UP: {
        bool down = (ev->type == SAPP_EVENTTYPE_KEY_DOWN);
        switch (ev->key_code) {
            case SAPP_KEYCODE_W: state->keys[0] = down; break;
            case SAPP_KEYCODE_A: state->keys[1] = down; break;
            case SAPP_KEYCODE_S: state->keys[2] = down; break;
            case SAPP_KEYCODE_D: state->keys[3] = down; break;
            case SAPP_KEYCODE_SPACE: state->keys[4] = down; break;
            case SAPP_KEYCODE_LEFT_CONTROL: state->keys[5] = down; break;
            case SAPP_KEYCODE_LEFT_SHIFT: state->keys[6] = down; break;
            case SAPP_KEYCODE_E: state->keys[7] = down; break;
            case SAPP_KEYCODE_Q: state->keys[8] = down; break;
            case SAPP_KEYCODE_ESCAPE: if (down) sapp_quit(); break;
            default: break;
        }
        break;
    }
    case SAPP_EVENTTYPE_MOUSE_DOWN:
        if (ev->mouse_button == SAPP_MOUSEBUTTON_RIGHT && state->examine.state != Examine::OFF) {
            // While examining, right-click exits. Cancels any in-flight
            // lerp by retargeting from the current pose back to `rest`.
            if (state->examine.state == Examine::ACTIVE) {
                state->examine.start = state->renderer.object_transform;
                state->examine.target = state->examine.rest;
                state->examine.t = 0.0f;
                state->examine.state = Examine::LERP_OUT;
            } else if (state->examine.state == Examine::LERP_IN) {
                // Mid-entry: swap direction so the easing stays smooth.
                state->examine.start = state->renderer.object_transform;
                state->examine.target = state->examine.rest;
                state->examine.t = 0.0f;
                state->examine.state = Examine::LERP_OUT;
            }
            // Do NOT toggle cam.camera_mode or map overlay.
            break;
        }
        if (ev->mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
            if (state->refviews_loaded && state->refviews.in_inspect) {
                // Exit inspect: lerp position back to where we clicked
                // the hotspot from, drop ortho, re-enter FPS controls.
                // Yaw/pitch are not lerped (see refview_update); the
                // user can look around during the return.
                state->refviews.inspect_target_pos[0] = state->refviews.inspect_return_pos[0];
                state->refviews.inspect_target_pos[1] = state->refviews.inspect_return_pos[1];
                state->refviews.inspect_target_pos[2] = state->refviews.inspect_return_pos[2];
                float dx = state->refviews.inspect_return_pos[0] - state->cam.position[0];
                float dy = state->refviews.inspect_return_pos[1] - state->cam.position[1];
                float dz = state->refviews.inspect_return_pos[2] - state->cam.position[2];
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                state->refviews.selected = -1;
                state->refviews.inspect_mode = true;
                state->refviews.inspect_return = true;
                state->refviews.lerping = true;
                state->refviews.lerp_t = 0.0f;
                state->refviews.lerp_duration = (dist > 1e-6f) ? dist / state->refviews.lerp_speed : 0.1f;
                state->refviews.start_pos[0] = state->cam.position[0];
                state->refviews.start_pos[1] = state->cam.position[1];
                state->refviews.start_pos[2] = state->cam.position[2];
                state->refviews.start_yaw = state->cam.yaw;
                state->refviews.start_pitch = state->cam.pitch;
                state->refviews.in_inspect = false;
                state->cam.orthographic = false;
                state->cam.camera_mode = true;
                sapp_lock_mouse(true);
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
            } else {
                state->cam.camera_mode = !state->cam.camera_mode;
                sapp_lock_mouse(state->cam.camera_mode);
                ImGuiIO& io = ImGui::GetIO();
                if (state->cam.camera_mode) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
                else                 io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

                // Toggle the top-down map overlay on top of the
                // existing camera-mode toggle.
                state->map_view_active = !state->map_view_active;
            }
        }
        if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT && state->map_view_active &&
            !ImGui::GetIO().WantCaptureMouse) {
            // Begin panning the top-down map.
            state->map_dragging = true;
            break;
        }
        if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT && state->cam.camera_mode &&
            state->examine.state == Examine::OFF && !state->map_view_active) {
            // Ray from screen center (crosshair) into scene
            float forward[3];
            camera_get_forward(&state->cam, forward);

            // 1. Hotspot pick on the currently-overlaid view (if any).
            //    Hotspots take precedence over object/neighbor clicks.
            int  hotspot_view  = -1;
            int32_t hotspot_idx = -1;
            if (state->refviews_loaded && !state->refviews.lerping && state->refviews.current_node >= 0) {
                RefView* cv = &state->refviews.views[state->refviews.current_node];
                if (cv->hotspot_count > 0) {
                    // Gate on overlay-visible distance (matches fade_dist=0.1 used below).
                    float dx0 = state->cam.position[0] - cv->position[0];
                    float dy0 = state->cam.position[1] - cv->position[1];
                    float dz0 = state->cam.position[2] - cv->position[2];
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
                        if (hotspot_idx >= 0) hotspot_view = state->refviews.current_node;
                    }
                }
            }

            if (hotspot_idx >= 0) {
                const Hotspot* h = &state->refviews.views[hotspot_view].hotspots[hotspot_idx];
                if (h->action.type == HOTSPOT_ACTION_WARP) {
                    int32_t warp_target = h->action.warp.target_view;
                    RefView* tv = &state->refviews.views[warp_target];
                    float dx = tv->position[0] - state->cam.position[0];
                    float dy = tv->position[1] - state->cam.position[1];
                    float dz = tv->position[2] - state->cam.position[2];
                    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                    state->refviews.selected = warp_target;
                    state->refviews.inspect_mode = false;
                    state->refviews.lerping = true;
                    state->refviews.lerp_t = 0.0f;
                    state->refviews.lerp_duration = (dist > 1e-6f) ? dist / state->refviews.lerp_speed : 0.1f;
                    state->refviews.start_pos[0] = state->cam.position[0];
                    state->refviews.start_pos[1] = state->cam.position[1];
                    state->refviews.start_pos[2] = state->cam.position[2];
                    state->refviews.start_yaw = state->cam.yaw;
                    state->refviews.start_pitch = state->cam.pitch;
                    break;
                } else if (h->action.type == HOTSPOT_ACTION_INSPECT) {
                    const HotspotActionInspect* it = &h->action.inspect;
                    float dx = it->position[0] - state->cam.position[0];
                    float dy = it->position[1] - state->cam.position[1];
                    float dz = it->position[2] - state->cam.position[2];
                    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                    state->refviews.selected = -1;
                    state->refviews.inspect_mode = true;
                    state->refviews.inspect_return = false;
                    state->refviews.inspect_target_pos[0] = it->position[0];
                    state->refviews.inspect_target_pos[1] = it->position[1];
                    state->refviews.inspect_target_pos[2] = it->position[2];
                    state->refviews.inspect_target_yaw   = it->yaw;
                    state->refviews.inspect_target_pitch = it->pitch;
                    state->refviews.lerping = true;
                    state->refviews.lerp_t = 0.0f;
                    state->refviews.lerp_duration = (dist > 1e-6f) ? dist / state->refviews.lerp_speed : 0.1f;
                    state->refviews.start_pos[0] = state->cam.position[0];
                    state->refviews.start_pos[1] = state->cam.position[1];
                    state->refviews.start_pos[2] = state->cam.position[2];
                    state->refviews.start_yaw = state->cam.yaw;
                    state->refviews.start_pitch = state->cam.pitch;
                    // Remember where to lerp back to on right-click exit.
                    state->refviews.inspect_return_pos[0] = state->cam.position[0];
                    state->refviews.inspect_return_pos[1] = state->cam.position[1];
                    state->refviews.inspect_return_pos[2] = state->cam.position[2];
                    state->refviews.in_inspect = true;
                    // Drive the existing ortho_blend transition.
                    state->cam.orthographic = true;
                    state->cam.ortho_size   = it->ortho_size;
                    // Switch to cursor mode (point & click); right-click
                    // will exit inspect and restore FPS controls.
                    state->cam.camera_mode = false;
                    sapp_lock_mouse(false);
                    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
                    break;
                }
            }

            // 2. Object pick (--object mesh, world-AABB) vs nearest
            //    neighbor-node pick. Closer hit wins; object > node by
            //    depth. Hotspot above already short-circuited if hit.
            float object_t = 1e30f;
            if (state->object_path) {
                float obj_model[16];
                mat4_from_transform(state->renderer.object_transform, obj_model);
                float obmin[3], obmax[3];
                mesh_aabb_world(state->object.aabb_min, state->object.aabb_max, obj_model, obmin, obmax);
                float t;
                if (math_ray_aabb(state->cam.position, forward, obmin, obmax, &t)) object_t = t;
            }

            float node_t = 1e30f;
            int best_hit = -1;
            if (state->refviews_loaded && !state->refviews.lerping) {
                for (uint32_t ni = 0; ni < state->neighbor_count; ni++) {
                    const float* c = &state->neighbor_positions[ni*3];
                    float hs = state->node_half_size;
                    float bmin[3] = { c[0]-hs, c[1]-hs, c[2]-hs };
                    float bmax[3] = { c[0]+hs, c[1]+hs, c[2]+hs };
                    float t;
                    if (math_ray_aabb(state->cam.position, forward, bmin, bmax, &t) && t < node_t) {
                        node_t = t;
                        best_hit = (int)ni;
                    }
                }
            }

            if (object_t < 1e30f && object_t <= node_t) {
                // Start examine: capture rest pose, compute target,
                // snapshot camera basis + distance for orbit/zoom,
                // begin LERP_IN. AABB radius = half-diagonal.
                state->examine.rest  = state->renderer.object_transform;
                state->examine.start = state->renderer.object_transform;
                state->examine.aabb_center_local[0] = (state->object.aabb_min[0] + state->object.aabb_max[0]) * 0.5f;
                state->examine.aabb_center_local[1] = (state->object.aabb_min[1] + state->object.aabb_max[1]) * 0.5f;
                state->examine.aabb_center_local[2] = (state->object.aabb_min[2] + state->object.aabb_max[2]) * 0.5f;
                float ex = state->object.aabb_max[0] - state->object.aabb_min[0];
                float ey = state->object.aabb_max[1] - state->object.aabb_min[1];
                float ez = state->object.aabb_max[2] - state->object.aabb_min[2];
                state->examine.aabb_radius = 0.5f * sqrtf(ex*ex + ey*ey + ez*ez);
                examine_compute_target(state->cam, state->examine.aabb_center_local,
                                       state->examine.aabb_radius, state->examine.rest,
                                       &state->examine.target);

                // Snapshot camera basis at examine entry. Reused each
                // frame while ACTIVE so zoom + pitch axes stay stable.
                camera_get_forward(&state->cam, state->examine.cam_fwd_entry);
                float wu[3] = { 0.0f, 1.0f, 0.0f };
                state->examine.cam_right_entry[0] = wu[1]*state->examine.cam_fwd_entry[2] - wu[2]*state->examine.cam_fwd_entry[1];
                state->examine.cam_right_entry[1] = wu[2]*state->examine.cam_fwd_entry[0] - wu[0]*state->examine.cam_fwd_entry[2];
                state->examine.cam_right_entry[2] = wu[0]*state->examine.cam_fwd_entry[1] - wu[1]*state->examine.cam_fwd_entry[0];
                float rl = sqrtf(state->examine.cam_right_entry[0]*state->examine.cam_right_entry[0]
                               + state->examine.cam_right_entry[1]*state->examine.cam_right_entry[1]
                               + state->examine.cam_right_entry[2]*state->examine.cam_right_entry[2]);
                if (rl > 1e-8f) {
                    state->examine.cam_right_entry[0] /= rl;
                    state->examine.cam_right_entry[1] /= rl;
                    state->examine.cam_right_entry[2] /= rl;
                }

                // Recompute dist with the same formula compute_target
                // used (kept in sync; if you change one, change both).
                float r = state->examine.aabb_radius * state->examine.rest.scale;
                if (r < 1e-4f) r = 1e-4f;
                state->examine.dist_base = r / tanf(state->cam.fov_y * 0.5f) / 0.8f;
                if (state->examine.dist_base < r * 1.5f) state->examine.dist_base = r * 1.5f;

                state->examine.base_rotation_euler[0] = state->examine.target.rotation_euler[0];
                state->examine.base_rotation_euler[1] = state->examine.target.rotation_euler[1];
                state->examine.base_rotation_euler[2] = state->examine.target.rotation_euler[2];
                state->examine.orbit_yaw      = 0.0f;
                state->examine.orbit_pitch    = 0.0f;
                state->examine.distance_scale = 1.0f;

                state->examine.t = 0.0f;
                state->examine.state = Examine::LERP_IN;
            } else if (best_hit >= 0) {
                uint32_t view_idx = state->neighbor_indices[best_hit];
                RefView* tv = &state->refviews.views[view_idx];
                float dx = tv->position[0] - state->cam.position[0];
                float dy = tv->position[1] - state->cam.position[1];
                float dz = tv->position[2] - state->cam.position[2];
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                state->refviews.selected = (int32_t)view_idx;
                state->refviews.lerping = true;
                state->refviews.lerp_t = 0.0f;
                state->refviews.lerp_duration = (dist > 1e-6f) ? dist / state->refviews.lerp_speed : 0.1f;
                state->refviews.start_pos[0] = state->cam.position[0];
                state->refviews.start_pos[1] = state->cam.position[1];
                state->refviews.start_pos[2] = state->cam.position[2];
                state->refviews.start_yaw = state->cam.yaw;
                state->refviews.start_pitch = state->cam.pitch;
            }
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_UP:
        if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT) state->map_dragging = false;
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        if (state->cam.camera_mode) {
            state->mouse_dx += ev->mouse_dx;
            state->mouse_dy += ev->mouse_dy;
        }
        if (state->map_dragging && state->map_view_active) {
            // Pan along the map camera's own right/up basis (which lies
            // in the world XZ plane for a top-down view). We use the
            // camera-local axes rather than world XZ so that dragging
            // still tracks screen-space movement if the map is rotated
            // about the world up axis.
            int wh = sapp_height();
            float fwd[3], right[3], up[3];
            float wup[3] = {0.0f, 1.0f, 0.0f};
            camera_get_forward(&state->map_cam, fwd);
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
            float wpp = (2.0f * state->map_cam.ortho_size) / (float)wh;
            float dr =  ev->mouse_dx * wpp;
            float du = -ev->mouse_dy * wpp;
            state->map_cam.position[0] += dr * right[0] + du * up[0];
            state->map_cam.position[1] += dr * right[1] + du * up[1];
            state->map_cam.position[2] += dr * right[2] + du * up[2];
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        if (!ImGui::GetIO().WantCaptureMouse) {
            if (state->examine.state != Examine::OFF) {
                // Zoom while examining: scale the AABB-fit distance.
                // Clamped so the object can't be shoved into the
                // camera or flown off to infinity.
                float factor = (ev->scroll_y > 0) ? (1.0f / 1.1f) : 1.1f;
                state->examine.distance_scale *= factor;
                if (state->examine.distance_scale < EXAMINE_ZOOM_MIN) state->examine.distance_scale = EXAMINE_ZOOM_MIN;
                if (state->examine.distance_scale > EXAMINE_ZOOM_MAX) state->examine.distance_scale = EXAMINE_ZOOM_MAX;
                break;
            }
            if (state->map_view_active) {
                // Zoom toward the cursor: scale ortho_size, then shift
                // map_cam.position so the world point under the cursor
                // before the zoom remains under the cursor after.
                float factor = (ev->scroll_y > 0) ? (1.0f / 1.2f) : 1.2f;
                float new_size = state->map_cam.ortho_size * factor;
                if (new_size < 0.05f) { factor = 0.05f / state->map_cam.ortho_size; new_size = 0.05f; }
                if (new_size > 50.0f) { factor = 50.0f  / state->map_cam.ortho_size; new_size = 50.0f;  }

                int ww = sapp_width();
                int wh = sapp_height();
                float aspect_m = (float)ww / (float)wh;
                float half_h = state->map_cam.ortho_size;
                float half_w = half_h * aspect_m;

                float mx = ev->mouse_x;
                float my = ev->mouse_y;
                float off_r = (2.0f * mx / (float)ww - 1.0f) * half_w;
                float off_u = (1.0f - 2.0f * my / (float)wh) * half_h;

                float fwd[3], right[3], up[3];
                float wup[3] = {0.0f, 1.0f, 0.0f};
                camera_get_forward(&state->map_cam, fwd);
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
                state->map_cam.position[0] += k * (off_r * right[0] + off_u * up[0]);
                state->map_cam.position[1] += k * (off_r * right[1] + off_u * up[1]);
                state->map_cam.position[2] += k * (off_r * right[2] + off_u * up[2]);

                state->map_cam.ortho_size = new_size;
            } else if (state->cam.camera_mode) {
                // FPS camera: wheel zooms by adjusting FOV.
                // Wheel up -> FOV down (zoom in), wheel down -> FOV up (zoom out).
                state->cam.fov_y *= (ev->scroll_y > 0) ? (1.0f / 1.1f) : 1.1f;
                float min_fov = 10.0f * (3.14159265358979f / 180.0f);
                float max_fov = 120.0f * (3.14159265358979f / 180.0f);
                if (state->cam.fov_y < min_fov) state->cam.fov_y = min_fov;
                if (state->cam.fov_y > max_fov) state->cam.fov_y = max_fov;
            } else {
                state->cam.move_speed *= (ev->scroll_y > 0) ? 1.2f : (1.0f / 1.2f);
                if (state->cam.move_speed < 0.1f) state->cam.move_speed = 0.1f;
                if (state->cam.move_speed > 100.0f) state->cam.move_speed = 100.0f;
            }
        }
        break;
    default:
        break;
    }
    return;
}

static void app_frame(void) {
    AppState* state = g_state;
    if (!state) return;

    PROFILE_FRAME();
    PROFILE_BEGIN("app_frame");

    const uint32_t max_neighbors = MAX_NEIGHBORS;

    float dt = (float)sapp_frame_duration();
    g_app_time += dt;
    if (g_splat_effect_duration <= 0.0f) g_splat_effect_duration = 2.25f;
    if (g_splat_effect_strength <= 0.0f) g_splat_effect_strength = 0.08f;
    if (g_splat_effect_tint_strength <= 0.0f) g_splat_effect_tint_strength = 0.45f;
    if (g_splat_effect_radius <= 0.0f) g_splat_effect_radius = 1.0f;

    float mouse_dx = state->mouse_dx;
    float mouse_dy = state->mouse_dy;
    state->mouse_dx = 0.0f;
    state->mouse_dy = 0.0f;

    bool examine_active = false;
    int win_w = 0, win_h = 0;
    float aspect = 1.0f;
    CameraUniforms cam_uniforms = {};

    PROFILE("frame update") {
    // Mesh path animation: walk between consecutive refview nodes and loop forever.
    if (state->mesh_path && state->refviews_loaded && state->refviews.count >= 2) {
        uint32_t a = state->anim_node % state->refviews.count;
        uint32_t b = (a + 1) % state->refviews.count;
        const float* pa = state->refviews.views[a].position;
        const float* pb = state->refviews.views[b].position;
        float dx = pb[0] - pa[0], dy = pb[1] - pa[1], dz = pb[2] - pa[2];
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        float dur = (dist > 1e-6f) ? (dist / state->anim_speed) : 0.1f;

        state->anim_t += dt / dur;
        // Advance through nodes if we passed multiple segments in one frame.
        int safety = (int)state->refviews.count + 1;
        while (state->anim_t >= 1.0f && safety-- > 0) {
            state->anim_t -= 1.0f;
            state->anim_node = (state->anim_node + 1) % state->refviews.count;
            a = state->anim_node;
            b = (a + 1) % state->refviews.count;
            pa = state->refviews.views[a].position;
            pb = state->refviews.views[b].position;
            dx = pb[0] - pa[0]; dy = pb[1] - pa[1]; dz = pb[2] - pa[2];
            dist = sqrtf(dx*dx + dy*dy + dz*dz);
            dur = (dist > 1e-6f) ? (dist / state->anim_speed) : 0.1f;
        }

        float t = state->anim_t;
        state->renderer.mesh_transform.translation[0] = pa[0] + dx * t;
        state->renderer.mesh_transform.translation[1] = pa[1] + dy * t + state->anim_y_offset;
        state->renderer.mesh_transform.translation[2] = pa[2] + dz * t;

        // Yaw faces direction of travel (matches camera yaw convention: yaw=0 -> +Z).
        float horiz2 = dx*dx + dz*dz;
        if (horiz2 > 1e-10f) {
            float target_yaw = atan2f(dx, dz);
            if (!state->anim_yaw_initialized) {
                state->anim_yaw = target_yaw;
                state->anim_yaw_initialized = true;
            } else {
                float diff = target_yaw - state->anim_yaw;
                const float PI = 3.14159265358979f;
                while (diff >  PI) diff -= 2.0f * PI;
                while (diff < -PI) diff += 2.0f * PI;
                float k = dt * 5.0f;
                if (k > 1.0f) k = 1.0f;
                state->anim_yaw += diff * k;
            }
            state->renderer.mesh_transform.rotation_euler[1] = state->anim_yaw;
        }
    }

    // Update reference view interpolation (locks camera input while active)
    bool camera_locked = refview_update(&state->refviews, &state->cam, dt);

    // Drive the examine lerp; while examining, FPS controls are fully
    // muted (no mouse look, no WASD) to keep the cam stationary for the
    // eventual return-lerp.
    examine_active = examine_locks_input(state->examine);
    examine_tick(&state->examine, &state->renderer.object_transform, dt);

    // While ACTIVE, mouse drag orbits the object around its AABB center
    // and the wheel zooms (handled in the wheel event). Composes a
    // world-up yaw with a fixed-axis pitch on top of the entry rotation.
    if (state->examine.state == Examine::ACTIVE) {
        state->examine.orbit_yaw   -= mouse_dx * EXAMINE_ORBIT_SENSITIVITY;
        state->examine.orbit_pitch += mouse_dy * EXAMINE_ORBIT_SENSITIVITY;
        if (state->examine.orbit_pitch >  EXAMINE_PITCH_LIMIT) state->examine.orbit_pitch =  EXAMINE_PITCH_LIMIT;
        if (state->examine.orbit_pitch < -EXAMINE_PITCH_LIMIT) state->examine.orbit_pitch = -EXAMINE_PITCH_LIMIT;

        // R_orbit = R_yaw(world_up) * R_pitch(cam_right_entry).
        float wu[3] = { 0.0f, 1.0f, 0.0f };
        float Ry[9], Rp[9], R_orbit[9];
        math_mat3_axis_angle(wu,                       state->examine.orbit_yaw,   Ry);
        math_mat3_axis_angle(state->examine.cam_right_entry,  state->examine.orbit_pitch, Rp);
        math_mat3_mul(Ry, Rp, R_orbit);

        // Compose with the base rotation captured at examine entry.
        float R_base[9], R_new[9];
        math_mat3_from_euler_zyx(state->examine.base_rotation_euler, R_base);
        math_mat3_mul(R_orbit, R_base, R_new);

        // Write Euler back into object_transform.
        math_mat3_decompose_zyx(R_new, state->renderer.object_transform.rotation_euler);

        // Place the AABB center at the (zoom-adjusted) pivot along the
        // entry forward, then back the translation off by the rotated
        // local center so the pivot stays nailed there.
        float s = state->renderer.object_transform.scale;
        float cl[3] = {
            state->examine.aabb_center_local[0] * s,
            state->examine.aabb_center_local[1] * s,
            state->examine.aabb_center_local[2] * s,
        };
        float cw[3] = {
            R_new[0]*cl[0] + R_new[3]*cl[1] + R_new[6]*cl[2],
            R_new[1]*cl[0] + R_new[4]*cl[1] + R_new[7]*cl[2],
            R_new[2]*cl[0] + R_new[5]*cl[1] + R_new[8]*cl[2],
        };
        float dist = state->examine.dist_base * state->examine.distance_scale;
        state->renderer.object_transform.translation[0] = state->cam.position[0] + state->examine.cam_fwd_entry[0]*dist - cw[0];
        state->renderer.object_transform.translation[1] = state->cam.position[1] + state->examine.cam_fwd_entry[1]*dist - cw[1];
        state->renderer.object_transform.translation[2] = state->cam.position[2] + state->examine.cam_fwd_entry[2]*dist - cw[2];
    }

    // Update camera (allow mouse look during lerp, but block WASD movement)
    if (examine_active) {
        camera_update(&state->cam, state->keys, 0.0f, 0.0f, 0.0f);
    } else if (camera_locked) {
        camera_update(&state->cam, state->keys, mouse_dx, mouse_dy, 0);
    } else if (state->cam.camera_mode || !ImGui::GetIO().WantCaptureKeyboard) {
        camera_update(&state->cam, state->keys, mouse_dx, mouse_dy, dt);
    } else {
        camera_update(&state->cam, state->keys, mouse_dx, mouse_dy, 0);
    }

    // Get framebuffer size
    win_w = sapp_width();
    win_h = sapp_height();
    aspect = (float)win_w / (float)win_h;

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
    if (!(state->refviews_loaded && state->refviews.lerping && state->refviews.inspect_mode)) {
        float target = state->cam.orthographic ? 1.0f : 0.0f;
        float blend_speed = 1.0f; // 1/speed seconds for full transition
        if (state->cam.ortho_blend < target) {
            state->cam.ortho_blend += blend_speed * dt;
            if (state->cam.ortho_blend > target) state->cam.ortho_blend = target;
        } else if (state->cam.ortho_blend > target) {
            state->cam.ortho_blend -= blend_speed * dt;
            if (state->cam.ortho_blend < target) state->cam.ortho_blend = target;
        }
    }

    // Build camera uniforms
    cam_uniforms = {};
    camera_get_view_matrix(&state->cam, cam_uniforms.view);
    camera_get_proj_matrix(&state->cam, aspect, cam_uniforms.proj);
    cam_uniforms.viewport[0] = (float)win_w;
    cam_uniforms.viewport[1] = (float)win_h;
    cam_uniforms.orthographic = state->cam.ortho_blend;
    // Provide pure persp/ortho focal lengths so the splat shader can use
    // each in its own mix() branch (see comment in camera.h).
    cam_uniforms.persp_focal = (1.0f / tanf(state->cam.fov_y * 0.5f)) * (float)win_h * 0.5f;
    cam_uniforms.ortho_focal = (float)win_h / (2.0f * state->cam.ortho_size);
    if (sg_query_backend() == SG_BACKEND_WGPU) {
        cam_uniforms.clip_y_sign = -1.0f;
        cam_uniforms.clip_z_01 = 1.0f;
    } else {
        cam_uniforms.clip_y_sign = 1.0f;
        cam_uniforms.clip_z_01 = 0.0f;
    }
    }

    if (state->scene_loaded) {
        // GPU culling/sorting runs later inside renderer_draw_frame. Until
        // Sokol exposes an indirect draw count, splat modes draw conservative
        // sentinel-filtered instance ranges.
        state->scene.visible_count = state->scene.gaussian_count;
    }

    PROFILE("imgui") {
    // ImGui frame. simgui_new_frame calls ImGui::NewFrame internally and
    // sets DisplaySize/DeltaTime.
    simgui_new_frame({ win_w, win_h, dt, sapp_dpi_scale() });

    ImGui::Begin("Info");
    ImGui::Text("FPS: %.1f", dt > 0 ? 1.0f / dt : 0.0f);
    if (state->scene_loaded) {
        ImGui::Text("Visible: %u / %u", state->scene.visible_count, state->scene.gaussian_count);
        int render_mode = (int)state->renderer.splat_render_mode;
        const char* render_mode_labels[] = { "Alpha Blend / Sorted", "StochasticSplats", "GPS Prototype" };
        if (ImGui::Combo("Render Mode", &render_mode, render_mode_labels, 3)) {
            state->renderer.splat_render_mode = (SplatRenderMode)render_mode;
            renderer_reset_stochastic_accumulation(&state->renderer);
        }
        const GpsCapabilities& gps_caps = state->renderer.gps_capabilities;
        ImGui::TextDisabled("GPS backend: %s", gps_caps.compute ? "prototype available" : "unavailable");
        if (!gps_caps.supported && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Requires compute, storage images, shader int64, and 64-bit atomic min.\n"
                              "Prototype mode currently requires compute only.\n"
                              "Detected: compute=%d storage_images=%d shader_int64=%d atomic_min_64=%d",
                              gps_caps.compute ? 1 : 0,
                              gps_caps.storage_images ? 1 : 0,
                              gps_caps.shader_int64 ? 1 : 0,
                              gps_caps.atomic_min_64 ? 1 : 0);
        }
        if (state->renderer.splat_render_mode == SplatRenderMode::StochasticSplats) {
            if (ImGui::Checkbox("ST TAA", &state->renderer.stochastic_taa_enabled)) {
                renderer_reset_stochastic_accumulation(&state->renderer);
            }
            if (state->renderer.stochastic_taa_enabled) {
                int taa_current_samples = (int)state->renderer.stochastic_taa_current_samples;
                if (ImGui::SliderInt("ST TAA Current Samples", &taa_current_samples, 1, 16)) {
                    state->renderer.stochastic_taa_current_samples = (uint32_t)taa_current_samples;
                    renderer_reset_stochastic_accumulation(&state->renderer);
                }
                ImGui::Text("ST TAA samples: %u", state->renderer.stochastic_sample_count);
            } else {
                int samples_per_frame = (int)state->renderer.stochastic_samples_per_frame;
                if (ImGui::SliderInt("ST Samples / Frame", &samples_per_frame, 1, 16)) {
                    state->renderer.stochastic_samples_per_frame = (uint32_t)samples_per_frame;
                    renderer_reset_stochastic_accumulation(&state->renderer);
                }
                if (ImGui::Checkbox("ST Accumulation", &state->renderer.stochastic_accumulation_enabled)) {
                    renderer_reset_stochastic_accumulation(&state->renderer);
                }
                if (state->renderer.stochastic_accumulation_enabled) {
                    ImGui::Text("ST accumulated samples: %u", state->renderer.stochastic_sample_count);
                } else {
                    ImGui::Text("ST accumulation off: averaging current-frame samples only");
                }
            }
        }
        if (state->renderer.splat_render_mode == SplatRenderMode::GaussianPointSplatting) {
            int gps_ss = (int)state->renderer.gps_supersample_factor;
            if (ImGui::SliderInt("GPS Supersampling", &gps_ss, 1, 4, "%dx")) {
                state->renderer.gps_supersample_factor = (uint32_t)gps_ss;
                state->renderer.gps_gpu.width = 0;
                state->renderer.gps_gpu.height = 0;
                state->renderer.gps_gpu.supersample_factor = 0;
                renderer_reset_stochastic_accumulation(&state->renderer);
            }
            int gps_work_budget_m = (int)(state->renderer.gps_max_work_items / (1024u * 1024u));
            if (ImGui::SliderInt("GPS Work Budget", &gps_work_budget_m, 1, 250, "%dM")) {
                state->renderer.gps_max_work_items = (uint32_t)gps_work_budget_m * 1024u * 1024u;
                renderer_upload_gaussians(&state->renderer, &state->scene);
                renderer_reset_stochastic_accumulation(&state->renderer);
            }
            ImGui::TextDisabled("Prototype global buffer cap, not a per-Gaussian GPS parameter.");
        }
        if (ImGui::Button("Shockwave Burst")) {
            g_splat_effect_active = true;
            g_splat_effect_start_time = g_app_time;
            renderer_reset_stochastic_accumulation(&state->renderer);
        }
        if (ImGui::Checkbox("Splat diagnostics", &state->renderer.splat_diagnostics_enabled)) {
            state->renderer.splat_diagnostics.valid = false;
        }
        {
            const char* sh_deg_labels[] = { "0 (DC only)", "1", "2", "3 (full)" };
            int sh_deg = state->renderer.sh_degree;
            if (sh_deg < 0) sh_deg = 0;
            if (sh_deg > 3) sh_deg = 3;
            if (ImGui::Combo("SH Degree", &sh_deg, sh_deg_labels, 4)) {
                state->renderer.sh_degree = sh_deg;
                renderer_reset_stochastic_accumulation(&state->renderer);
            }
            float lod = state->renderer.sh_lod_distance;
            if (ImGui::SliderFloat("SH LOD Distance", &lod, 0.0f, 200.0f, "%.1f")) {
                state->renderer.sh_lod_distance = lod;
                renderer_reset_stochastic_accumulation(&state->renderer);
            }
            if (state->renderer.sh_lod_distance <= 0.0f) {
                ImGui::TextDisabled("LOD off: set >0 to drop far splats to DC color");
            }
        }
        if (state->renderer.splat_diagnostics_enabled) {
            const SplatDiagnostics& diag = state->renderer.splat_diagnostics;
            if (diag.valid) {
                ImGui::Text("Projected quad area: %.1f MPix", (double)diag.total_quad_kpix * 1024.0 / 1000000.0);
                ImGui::Text("Avg quad area: %.1f px", state->scene.visible_count > 0
                    ? ((double)diag.total_quad_kpix * 1024.0 / (double)state->scene.visible_count)
                    : 0.0);
                ImGui::Text("Max quad area: %u px", diag.max_quad_px);
                ImGui::Text(">1K px: %u, >16K px: %u", diag.splats_over_1k_px, diag.splats_over_16k_px);
                ImGui::Text("GPS estimated points: %.1f M", (double)diag.total_gps_kpoints * 1024.0 / 1000000.0);
                ImGui::Text("GPS avg points/splat: %.1f", state->scene.visible_count > 0
                    ? ((double)diag.total_gps_kpoints * 1024.0 / (double)state->scene.visible_count)
                    : 0.0);
                ImGui::Text("GPS max points/splat: %u", diag.max_gps_points);
                ImGui::Text("GPS >1K pts: %u, >16K pts: %u",
                    diag.gps_splats_over_1k_points, diag.gps_splats_over_16k_points);
            } else {
                ImGui::Text("Projected quad area: unavailable");
            }
            ImGui::TextDisabled("Diagnostics use synchronous GL readback; disable for profiling.");
        }
        if (ImGui::SliderFloat("Shockwave Strength", &g_splat_effect_strength, 0.0f, 0.25f, "%.3f")) {
            renderer_reset_stochastic_accumulation(&state->renderer);
        }
        if (ImGui::SliderFloat("Shockwave Duration", &g_splat_effect_duration, 0.25f, 5.0f, "%.2fs")) {
            renderer_reset_stochastic_accumulation(&state->renderer);
        }
        if (ImGui::SliderFloat("Shockwave Tint", &g_splat_effect_tint_strength, 0.0f, 1.5f, "%.2f")) {
            renderer_reset_stochastic_accumulation(&state->renderer);
        }
    }
    ImGui::Text("Camera: %.1f, %.1f, %.1f  yaw %.3f  pitch %.3f",
                state->cam.position[0], state->cam.position[1], state->cam.position[2],
                state->cam.yaw, state->cam.pitch);

    // Cursor UV on the active overlay panorama (matches .hotspots shape.points).
    // Only meaningful while the overlay is visible (same gate as overlay alpha).
    if (state->refviews_loaded && state->refviews.current_node >= 0) {
        RefView* cv = &state->refviews.views[state->refviews.current_node];
        float dx0 = state->cam.position[0] - cv->position[0];
        float dy0 = state->cam.position[1] - cv->position[1];
        float dz0 = state->cam.position[2] - cv->position[2];
        float d2  = dx0*dx0 + dy0*dy0 + dz0*dz0;
        if (d2 < 0.01f) {
            // In FPS mode the cursor is captured -> use the crosshair (screen
            // center). In cursor mode use the actual mouse position.
            float mx, my;
            if (state->cam.camera_mode) {
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
            camera_get_overlay_ray_basis(&state->cam, (float)win_w / (float)win_h,
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

    ImGui::Text("Speed: %.1f", state->cam.move_speed);
    ImGui::Checkbox("Orthographic", &state->cam.orthographic);
    if (state->cam.orthographic) {
        ImGui::SliderFloat("Ortho Size", &state->cam.ortho_size, 0.5f, 5.0f);
    } else {
        float fov_deg = state->cam.fov_y * (180.0f / 3.14159265358979f);
        if (ImGui::SliderFloat("FOV", &fov_deg, 10.0f, 170.0f, "%.0f°")) {
            state->cam.fov_y = fov_deg * (3.14159265358979f / 180.0f);
        }
    }
    if (state->refviews_loaded) {
        ImGui::SliderFloat("Ref View Opacity", &state->refview_max_alpha, 0.0f, 1.0f);
        ImGui::Checkbox("Use Covisibility", &state->refviews.use_covisibility);
        if (state->refviews.use_covisibility) {
            ImGui::SliderInt("Min Inliers", &state->refviews.min_inliers, 0, 500);
        } else {
            ImGui::SliderFloat("Neighbor Radius", &state->refviews.neighbor_radius, 0.5f, 10.0f);
        }
        ImGui::Checkbox("Show Node Boxes", &state->show_node_boxes);
        ImGui::SliderFloat("Node Box Size", &state->node_half_size, 0.1f, 1.0f);
        ImGui::Checkbox("Show Hotspot Debug", &state->show_hotspot_debug);
        ImGui::SliderFloat("Transition Speed", &state->refviews.lerp_speed, 1.0f, 10.0f);
        if (state->refviews.current_node >= 0) {
            ImGui::Text("Current Node: %d", state->refviews.current_node);
            ImGui::Text("Neighbors: %u", state->neighbor_count);
        }
    }
    ImGui::End();

    if (state->refviews_loaded) {
        ImGui::Begin("Reference Views");
        for (uint32_t i = 0; i < state->refviews.count; i++) {
            char label[32];
            snprintf(label, sizeof(label), "%u", i);
            bool is_selected = ((int32_t)i == state->refviews.selected);
            if (ImGui::Selectable(label, is_selected)) {
                RefView* tv = &state->refviews.views[i];
                float dx = tv->position[0] - state->cam.position[0];
                float dy = tv->position[1] - state->cam.position[1];
                float dz = tv->position[2] - state->cam.position[2];
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                state->refviews.selected = i;
                state->refviews.lerping = true;
                state->refviews.lerp_t = 0.0f;
                state->refviews.lerp_duration = (dist > 1e-6f) ? dist / state->refviews.lerp_speed : 0.1f;
                state->refviews.start_pos[0] = state->cam.position[0];
                state->refviews.start_pos[1] = state->cam.position[1];
                state->refviews.start_pos[2] = state->cam.position[2];
                state->refviews.start_yaw = state->cam.yaw;
                state->refviews.start_pitch = state->cam.pitch;
            }
        }
        ImGui::End();
    }

    if (state->mesh_path) {
        ImGui::Begin("Mesh Transform");
        MeshTransform& mt = state->renderer.mesh_transform;
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

#if defined(ENABLE_PROFILER)
    ImGui::Begin("sokol-gfx", NULL, ImGuiWindowFlags_MenuBar);
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Windows")) {
            sgimgui_draw_capabilities_menu_item("Capabilities");
            sgimgui_draw_frame_stats_menu_item("Frame Stats");
            sgimgui_draw_buffer_menu_item("Buffers");
            sgimgui_draw_image_menu_item("Images");
            sgimgui_draw_view_menu_item("Views");
            sgimgui_draw_sampler_menu_item("Samplers");
            sgimgui_draw_shader_menu_item("Shaders");
            sgimgui_draw_pipeline_menu_item("Pipelines");
            sgimgui_draw_capture_menu_item("Calls");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    sgimgui_draw_frame_stats_window_content();
    ImGui::End();
    sgimgui_draw();
#endif

    // Draw crosshair in camera mode (highlight when aiming at a node,
    // or show an upward arrow when aiming at a hotspot on the overlay).
    // Suppressed during examine: the object fills the view, so a hover
    // icon would just signal "you can examine what you're already
    // examining". The camera is also locked, so picks are meaningless.
    if (state->cam.camera_mode && !examine_active) {
        bool crosshair_hover = false;
        bool hotspot_hover = false;

        // Hotspot hover takes precedence over neighbor-node hover.
        // Mirrors the click-time pick logic above.
        if (state->refviews_loaded && !state->refviews.lerping && state->refviews.current_node >= 0) {
            RefView* cv = &state->refviews.views[state->refviews.current_node];
            if (cv->hotspot_count > 0) {
                float dx0 = state->cam.position[0] - cv->position[0];
                float dy0 = state->cam.position[1] - cv->position[1];
                float dz0 = state->cam.position[2] - cv->position[2];
                float d2  = dx0*dx0 + dy0*dy0 + dz0*dz0;
                if (d2 < 0.01f) {
                    float forward[3];
                    camera_get_forward(&state->cam, forward);
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
            camera_get_forward(&state->cam, forward);

            float object_t = 1e30f;
            if (state->object_path) {
                float obj_model[16];
                mat4_from_transform(state->renderer.object_transform, obj_model);
                float obmin[3], obmax[3];
                mesh_aabb_world(state->object.aabb_min, state->object.aabb_max, obj_model, obmin, obmax);
                float t;
                if (math_ray_aabb(state->cam.position, forward, obmin, obmax, &t)) object_t = t;
            }

            float node_t = 1e30f;
            if (state->refviews_loaded && !state->refviews.lerping && state->neighbor_count > 0) {
                for (uint32_t ni = 0; ni < state->neighbor_count; ni++) {
                    const float* c = &state->neighbor_positions[ni*3];
                    float hs = state->node_half_size;
                    float bmin[3] = { c[0]-hs, c[1]-hs, c[2]-hs };
                    float bmax[3] = { c[0]+hs, c[1]+hs, c[2]+hs };
                    float t;
                    if (math_ray_aabb(state->cam.position, forward, bmin, bmax, &t) && t < node_t) {
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
    if (state->show_hotspot_debug && state->refviews_loaded && state->refviews.current_node >= 0) {
        RefView* cv = &state->refviews.views[state->refviews.current_node];
        if (cv->hotspot_count > 0) {
            float cam_basis[16];
            float cam_tan[2];
            camera_get_overlay_ray_basis(&state->cam, (float)win_w / (float)win_h, cam_basis, cam_tan);
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
                uint32_t k = (uint32_t)state->refviews.current_node * 2654435761u + hi * 2246822519u;
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
                    // The overlay shader uses camera_dir.y = -v_ndc.y * tan
                    // (see comment in shaders/overlay.glsl), so we mirror that
                    // negation when inverting here.
                    float ndc_x = (cx / cz) / cam_tan[0];
                    float ndc_y = -(cy / cz) / cam_tan[1];

                    pts_buf[i].x = (ndc_x * 0.5f + 0.5f) * (float)win_w;
                    pts_buf[i].y = (1.0f - (ndc_y * 0.5f + 0.5f)) * (float)win_h;
                }
                if (any_behind) continue;

                dl->AddConvexPolyFilled(pts_buf, (int)n, fill_col);
                dl->AddPolyline(pts_buf, (int)n, line_col, ImDrawFlags_Closed, 2.0f);
            }
        }
    }
    }

    // simgui_render() (called inside renderer_draw_frame's pass) calls
    // ImGui::Render() itself, so we don't call it here.

    OverlayParams overlay = {};
    OverlayParams* overlay_ptr = NULL;
    NodeRenderParams node_params = {};
    NodeRenderParams* node_ptr = NULL;
    SplatEffectParams splat_effect = {};
    SplatEffectParams* splat_effect_ptr = NULL;
    CameraUniforms  map_uniforms = {};
    CameraUniforms* map_uniforms_ptr = NULL;

    PROFILE("overlay params") {
    // Find closest refview node to camera (used for overlay + current_node tracking)
    if (state->refviews_loaded) {
        float best_dist2 = 1e30f;
        int best_idx = -1;
        for (uint32_t i = 0; i < state->refviews.count; i++) {
            if (!state->refviews.views[i].texture.id) continue;
            float dx = state->cam.position[0] - state->refviews.views[i].position[0];
            float dy = state->cam.position[1] - state->refviews.views[i].position[1];
            float dz = state->cam.position[2] - state->refviews.views[i].position[2];
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < best_dist2) { best_dist2 = d2; best_idx = (int)i; }
        }

        state->refviews.current_node = best_idx;

        if (best_idx >= 0 && state->refview_max_alpha > 0.0f) {
            RefView* rv = &state->refviews.views[best_idx];
            float dist = sqrtf(best_dist2);
            float fade_dist = 0.1f;
            float alpha = 1.0f - dist / fade_dist;
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > state->refview_max_alpha) alpha = state->refview_max_alpha;

            if (alpha > 0.0f) {
                overlay.texture = rv->texture;
                overlay.texture_view = rv->texture_view;
                overlay.alpha = alpha;

                camera_get_overlay_ray_basis(&state->cam, (float)win_w / (float)win_h,
                                             overlay.camera_ray_basis,
                                             overlay.camera_tan_half_fov);

                refview_get_rotation_matrix(rv, overlay.ref_rotation);
                overlay_ptr = &overlay;
            }
        }

        // Collect neighbor nodes for wireframe rendering + click targets
        state->neighbor_count = refview_get_neighbors(&state->refviews, state->neighbor_positions, state->neighbor_indices, max_neighbors);
    }

    // Build node render params
    if (state->refviews_loaded && state->neighbor_count > 0 && state->show_node_boxes) {
        node_params.positions = state->neighbor_positions;
        node_params.count = state->neighbor_count;
        node_params.half_size = state->node_half_size;
        node_ptr = &node_params;
    }

    // Build map-camera uniforms when the top-down overlay is active.
    if (state->map_view_active) {
        camera_get_view_matrix(&state->map_cam, map_uniforms.view);
        camera_get_proj_matrix(&state->map_cam, aspect, map_uniforms.proj);
        map_uniforms.viewport[0]   = (float)win_w;
        map_uniforms.viewport[1]   = (float)win_h;
        map_uniforms.orthographic  = state->map_cam.ortho_blend;
        map_uniforms.persp_focal   = (1.0f / tanf(state->map_cam.fov_y * 0.5f)) * (float)win_h * 0.5f;
        map_uniforms.ortho_focal   = (float)win_h / (2.0f * state->map_cam.ortho_size);
        map_uniforms_ptr = &map_uniforms;
    }

    if (state->scene_loaded && g_splat_effect_active) {
        float elapsed = g_app_time - g_splat_effect_start_time;
        if (elapsed >= g_splat_effect_duration) {
            g_splat_effect_active = false;
        } else {
            splat_effect.center_radius[0] = g_splat_effect_center[0];
            splat_effect.center_radius[1] = g_splat_effect_center[1];
            splat_effect.center_radius[2] = g_splat_effect_center[2];
            splat_effect.center_radius[3] = g_splat_effect_radius;
            splat_effect.params[0] = elapsed;
            splat_effect.params[1] = g_splat_effect_duration;
            splat_effect.params[2] = g_splat_effect_strength;
            splat_effect.params[3] = 1.0f;
            splat_effect.color[0] = 1.0f;
            splat_effect.color[1] = 0.55f;
            splat_effect.color[2] = 0.18f;
            splat_effect.color[3] = g_splat_effect_tint_strength;
            splat_effect_ptr = &splat_effect;
        }
    }
    }

    // Render
    PROFILE("render submit") {
    renderer_draw_frame(&state->renderer, &state->scene, &cam_uniforms, overlay_ptr, node_ptr,
                        splat_effect_ptr, 1.0f /*wireframe_occlusion*/, map_uniforms_ptr);
    }


    PROFILE_END();
    return;
}

static void app_cleanup(void) {
    AppState* state = g_state;
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

#if defined(ENABLE_PROFILER)
    if (state->sgimgui_setup_done) sgimgui_shutdown();
#endif
    if (state->simgui_setup_done) simgui_shutdown(); // destroys ImGui context
    if (state->sg_setup_done) sg_shutdown();

    delete state;
    g_state = NULL;
    if (sargs_isvalid()) sargs_shutdown();
}

sapp_desc sokol_main(int argc, char* argv[]) {
    g_argc = argc;
    g_argv = argv;

    sapp_desc desc = {};
    desc.init_cb = app_init;
    desc.frame_cb = app_frame;
    desc.cleanup_cb = app_cleanup;
    desc.event_cb = app_event;
    desc.width = 1280;
    desc.height = 720;
    desc.sample_count = 1;
    desc.swap_interval = 0; // no vsync
    desc.window_title = "gsplat";
    desc.logger.func = slog_func;
    desc.html5.canvas_selector = "#canvas";
    return desc;
}
