#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

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

int main(int argc, char* argv[]) {
    const char* ply_path = NULL;
    const char* colmap_dir = NULL;
    const char* mesh_path = NULL;
    const char* object_path = NULL;
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
    char colmap_dir_buf[512];
    if (colmap_dir) {
        char probe[512];
        snprintf(probe, sizeof(probe), "%s/images.txt", colmap_dir);
        bool has_model_here = file_exists(probe);
        if (!has_model_here) {
            snprintf(probe, sizeof(probe), "%s/images.bin", colmap_dir);
            has_model_here = file_exists(probe);
        }
        if (!has_model_here) {
            snprintf(colmap_dir_buf, sizeof(colmap_dir_buf), "%s/sparse/0", colmap_dir);
            colmap_dir = colmap_dir_buf;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Audio is non-fatal: if init fails (no device, etc.) sfx_play becomes a
    // no-op via the g_audio_ready flag and the rest of the app keeps running.
    audio_init();
    Sfx sfx_transition = {};
    sfx_load(&sfx_transition, "res/transition.wav");

    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (!device) {
        fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("gsplat", 1280, 720, SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        fprintf(stderr, "SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
        return 1;
    }

    // ImGui init
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForOther(window);

    SDL_GPUTextureFormat swapchain_format = SDL_GetGPUSwapchainTextureFormat(device, window);
    ImGui_ImplSDLGPU3_InitInfo imgui_init = {};
    imgui_init.Device = device;
    imgui_init.ColorTargetFormat = swapchain_format;
    ImGui_ImplSDLGPU3_Init(&imgui_init);

    // Renderer
    Renderer renderer = {};
    if (!renderer_init(&renderer, device, window)) {
        fprintf(stderr, "Renderer init failed\n");
        return 1;
    }

    // Scene
    GaussianScene scene = {};
    bool scene_loaded = false;
    if (ply_path) {
        scene_loaded = load_ply(ply_path, &scene);
        if (scene_loaded) {
            renderer_upload_gaussians(&renderer, &scene);
        }
    }

    // Mesh
    Mesh mesh = {};
    if (mesh_path) {
        if (mesh_load(mesh_path, &mesh)) {
            renderer_upload_mesh(&renderer, &mesh);
        }
    }

    // Static scene object (no animation, identity transform)
    Mesh object = {};
    if (object_path) {
        if (mesh_load(object_path, &object)) {
            renderer_upload_object_mesh(&renderer, &object);
        }
    }

    // Reference views
    RefViewSet refviews = {};
    refviews.selected = -1;
    bool refviews_loaded = false;
    if (colmap_dir) {
        refviews_loaded = refview_load(&refviews, colmap_dir);
        if (refviews_loaded) {
            refview_load_covisibility(&refviews, colmap_dir);
            refview_load_images(&refviews, device);
            hotspot_load_for_set(&refviews);
        }
    }

    // Camera
    Camera cam;
    camera_init(&cam);
    SDL_SetWindowRelativeMouseMode(window, true); // start in camera mode
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;

    bool keys[9] = {}; // W A S D Space LCtrl LShift E Q
    uint64_t last_time = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    bool running = true;
    float refview_max_alpha = 0.5f;
    float node_half_size = 0.5f;
    bool show_node_boxes = true;
    bool show_hotspot_debug = false;

    // Mesh path animation (walks the mesh from refview node 0..n-1 and loops)
    uint32_t anim_node = 0;
    float    anim_t = 0.0f;
    float    anim_speed = 1.0f; // world units per second
    float    anim_yaw = 0.0f;
    bool     anim_yaw_initialized = false;
    float    anim_y_offset = 1.4f; // refview nodes are at head height; drop feet ~1.6m

    // Neighbor scratch buffers
    const uint32_t max_neighbors = 64;
    float neighbor_positions[64 * 3];
    uint32_t neighbor_indices[64];
    uint32_t neighbor_count = 0;

    // Top-down map overlay: separate camera, never mutates the FPS `cam`.
    // Right-click toggles `map_view_active`; while active a second render
    // pass in renderer_draw_frame draws mesh+splats+wireframe from this
    // camera on top of the FPS frame (transparent background = FPS shows
    // through where the map didn't draw).
    bool   map_view_active = false;
    Camera map_cam;
    camera_init(&map_cam);
    map_cam.position[0]  = 1.1f;
    map_cam.position[1]  = -1.0f;
    map_cam.position[2]  = 0.6f;
    map_cam.yaw          = -6.3f;
    map_cam.pitch        = 1.56f;
    map_cam.orthographic = true;
    map_cam.ortho_blend  = 1.0f;
    map_cam.ortho_size   = 5.0f;

    // Map overlay interaction: left-drag pans, wheel zooms toward cursor.
    bool map_dragging = false;

    while (running) {
        uint64_t now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last_time) / (float)freq;
        last_time = now;

        SDL_Event ev;
        float mouse_dx = 0, mouse_dy = 0;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);

            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
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
                    case SDL_SCANCODE_ESCAPE: if (down) running = false; break;
                    default: break;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
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
                    refviews_loaded && !refviews.lerping) {
                    // Ray from screen center (crosshair) into scene
                    float forward[3];
                    camera_get_forward(&cam, forward);

                    // 1. Hotspot pick on the currently-overlaid view (if any).
                    //    Hotspots take precedence over neighbor-node clicks.
                    int  hotspot_view  = -1;
                    int32_t hotspot_idx = -1;
                    if (refviews.current_node >= 0) {
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

                    // 2. Fallback: test against all neighbor AABBs, pick closest hit
                    float best_t = 1e30f;
                    int best_hit = -1;
                    for (uint32_t ni = 0; ni < neighbor_count; ni++) {
                        const float* center = &neighbor_positions[ni*3];
                        float hs = node_half_size;
                        // Slab method
                        float tmin = -1e30f, tmax = 1e30f;
                        for (int axis = 0; axis < 3; axis++) {
                            float o = cam.position[axis];
                            float d = forward[axis];
                            float bmin = center[axis] - hs;
                            float bmax = center[axis] + hs;
                            if (fabsf(d) < 1e-8f) {
                                if (o < bmin || o > bmax) { tmin = 1e30f; break; }
                            } else {
                                float t1 = (bmin - o) / d;
                                float t2 = (bmax - o) / d;
                                if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
                                if (t1 > tmin) tmin = t1;
                                if (t2 < tmax) tmax = t2;
                                if (tmin > tmax) { tmin = 1e30f; break; }
                            }
                        }
                        if (tmin < best_t && tmax > 0.0f) {
                            best_t = tmin;
                            best_hit = (int)ni;
                        }
                    }

                    if (best_hit >= 0) {
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
        }

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

        // Update camera (allow mouse look during lerp, but block WASD movement)
        if (camera_locked) {
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

        // ImGui frame
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

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
        if (cam.camera_mode) {
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

            if (!hotspot_hover && refviews_loaded && !refviews.lerping && neighbor_count > 0) {
                float forward[3];
                camera_get_forward(&cam, forward);
                for (uint32_t ni = 0; ni < neighbor_count; ni++) {
                    const float* c = &neighbor_positions[ni*3];
                    float hs = node_half_size;
                    float tmin = -1e30f, tmax = 1e30f;
                    for (int axis = 0; axis < 3; axis++) {
                        float o = cam.position[axis];
                        float d = forward[axis];
                        float bmin = c[axis] - hs, bmax = c[axis] + hs;
                        if (fabsf(d) < 1e-8f) {
                            if (o < bmin || o > bmax) { tmin = 1e30f; break; }
                        } else {
                            float t1 = (bmin - o) / d, t2 = (bmax - o) / d;
                            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
                            if (t1 > tmin) tmin = t1;
                            if (t2 < tmax) tmax = t2;
                            if (tmin > tmax) { tmin = 1e30f; break; }
                        }
                    }
                    if (tmax > 0.0f && tmin < 1e30f) { crosshair_hover = true; break; }
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
                        // Note: SDL_GPU (Vulkan) NDC has Y pointing down, but the
                        // perspective pipeline elsewhere uses persp[5] = -f to flip
                        // it to logical Y-up. The overlay shader writes v_ndc directly
                        // (no proj), so screen Y here is also Y-up: ndc_y = +1 is top.
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

        ImGui::Render();

        // Find closest refview node to camera (used for overlay + current_node tracking)
        OverlayParams overlay = {};
        OverlayParams* overlay_ptr = NULL;
        if (refviews_loaded) {
            float best_dist2 = 1e30f;
            int best_idx = -1;
            for (uint32_t i = 0; i < refviews.count; i++) {
                if (!refviews.views[i].texture) continue;
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
    }

    SDL_WaitForGPUIdle(device);

    if (scene_loaded) free_scene(&scene);
    if (mesh_path) mesh_free(&mesh);
    if (object_path) mesh_free(&object);
    if (refviews_loaded) {
        refview_release_images(&refviews, device);
        refview_free(&refviews);
    }
    renderer_destroy(&renderer);

    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(device);

    sfx_free(&sfx_transition);
    audio_shutdown();

    SDL_Quit();

    return 0;
}
