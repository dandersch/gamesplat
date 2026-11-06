// Native integration/measurement harness: runs the real app and renderer.
// Build with tests/render_gsanim.sh. Readbacks happen only after measurement.
#define sokol_main gsplat_interactive_main
#include "src/main.cpp"
#undef sokol_main
#include <cassert>
#include <X11/Xresource.h>

// Fixed-pixel captures must not inherit desktop Xft scaling: Sokol rounds
// physical -> logical -> physical sizes (640 becomes 641 at 150% DPI).
extern "C" Bool __real_XrmGetResource(XrmDatabase, const char*, const char*, char**, XrmValue*);
extern "C" Bool __wrap_XrmGetResource(XrmDatabase db, const char* name, const char* cls, char** type, XrmValue* value) {
    if (!strcmp(name, "Xft.dpi")) {
        static char dpi[] = "96", string_type[] = "String";
        *type = string_type; value->addr = dpi; value->size = sizeof(dpi);
        return True;
    }
    return __real_XrmGetResource(db, name, cls, type, value);
}

static double now_seconds() { return double(SDL_GetPerformanceCounter())/SDL_GetPerformanceFrequency(); }
struct FrameMeasurement {
    GLuint query;
    double wall_ms, cpu_ms, animation_time;
    int state, selected_state;
    bool playing;
};
static std::vector<FrameMeasurement> measurements;
static double started, previous, startup_ms, warmup, interval;
static int frame_number, capture_frames;
static int requested_width = 1920, requested_height = 1080;
static GLint initial_free_kb;
static bool memory_supported;

static void measure_init() {
    memory_supported = gl_has_extension("GL_NVX_gpu_memory_info");
    if (memory_supported) glGetIntegerv(0x9049, &initial_free_kb);
    double begin = now_seconds();
    app_init();
    if (!g_state->scene_loaded) { fprintf(stderr, "Scene load failed\n"); exit(1); }
    glFinish();
    startup_ms = (now_seconds()-begin)*1000;
    warmup = sargs_exists("warmup") ? atof(sargs_value("warmup")) : 3;
    interval = sargs_exists("interval") ? atof(sargs_value("interval")) : 10;
    capture_frames = sargs_exists("frames") ? atoi(sargs_value("frames")) : 0;
    if (sargs_exists("taa")) g_state->renderer.stochastic_taa_enabled = !strcmp(sargs_value("taa"), "on");
    if (!sargs_exists("ui")) ImGui::GetStyle().Alpha = 0;
    ImGui::GetIO().IniFilename = nullptr;
    g_state->baked_views.selected = float(atof(sargs_value("view")));
    g_state->baked_views.transition = sargs_exists("transition");
    // Interactive viewing leaves the fly camera in control. Captures keep the
    // Blender reference cameras unless fly=1 is set.
    if (!g_state->baked_views.poses.empty() && !sargs_exists("fly"))
        g_state->baked_views.follow_reference = true;
    if (sargs_exists("fly_yaw")) g_state->cam.yaw += float(atof(sargs_value("fly_yaw")));
    if (sargs_exists("fly_forward")) {
        bool keys[9] = {};
        keys[0] = true;
        camera_update(&g_state->cam, keys, 0, 0, float(atof(sargs_value("fly_forward"))));
    }
    sapp_lock_mouse(false);
    started = previous = now_seconds();
    fprintf(stderr, "GPU=%s resolution=%dx%d vsync=off warmup=%.3f interval=%.3f\n",
        glGetString(GL_RENDERER), sapp_width(), sapp_height(), warmup, interval);
}

static double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[size_t((values.size()-1)*p)];
}

static void measure_frame() {
    double begin = now_seconds(), elapsed = begin-started;
    if (sapp_width() != requested_width || sapp_height() != requested_height) {
        if (elapsed > 10) { fprintf(stderr, "Window manager did not honor requested resolution\n"); exit(1); }
        return;
    }
    if (!frame_number) {
        started = previous = begin; elapsed = 0;
        fprintf(stderr, "Measured framebuffer=%dx%d\n", sapp_width(), sapp_height());
    }
    bool record = !capture_frames && elapsed >= warmup;
    GLuint query = 0;
    if (record) { glGenQueries(1, &query); glBeginQuery(GL_TIME_ELAPSED, query); }
    if (sargs_exists("fixed_time") && g_state->scene.animation) {
        g_state->scene.animation->playing = false;
        g_state->scene.animation->playback_time = atof(sargs_value("fixed_time"));
    }
    if (capture_frames && sargs_exists("exercise_changes")) {
        bool last = frame_number == capture_frames-1;
        g_state->baked_views.selected = last ? float(atof(sargs_value("view"))) : float(frame_number%3);
        if (g_state->scene.animation && g_state->scene.animation->state_count)
            g_state->scene.animation->selected_state = last ? -1 : frame_number % int(g_state->scene.animation->state_count);
    }
    if (sargs_exists("exercise_modes")) {
        auto& r = g_state->renderer;
        SplatRenderMode mode = frame_number < 3 ? SplatRenderMode(frame_number) :
            !strcmp(sargs_value("render_mode"), "gps") ? SplatRenderMode::GaussianPointSplatting :
            !strcmp(sargs_value("render_mode"), "stochastic") ? SplatRenderMode::StochasticSplats : SplatRenderMode::AlphaBlendSorted;
        if (r.splat_render_mode != mode) {
            r.splat_render_mode = mode;
            renderer_reset_stochastic_accumulation(&r);
        }
        if (g_state->scene.animation && frame_number < 4)
            g_state->scene.animation->playback_time += frame_number * .5;
    }
    const double prior_time = g_state->renderer.animation_time;
    app_frame();
    if (g_state->scene.animation && g_state->scene.animation->version == 3 &&
        prior_time != g_state->scene.animation->playback_time &&
        g_state->renderer.splat_render_mode != SplatRenderMode::AlphaBlendSorted)
        assert(g_state->renderer.stochastic_sample_count <= g_state->renderer.stochastic_samples_per_frame);
    double submitted = now_seconds();
    if (record) {
        glEndQuery(GL_TIME_ELAPSED);
        const GSAnim* a = g_state->scene.animation;
        measurements.push_back({query, (begin-previous)*1000, (submitted-begin)*1000,
            a ? a->playback_time : 0, g_state->renderer.animation_state_a,
            a ? a->selected_state : -1, a && a->playing});
    }
    previous = begin;
    ++frame_number;
    if (capture_frames ? frame_number < capture_frames : elapsed < warmup+interval) return;
    glFinish();
    assert(glGetError() == GL_NO_ERROR);
    assert(!glIsEnabled(GL_FRAMEBUFFER_SRGB)); // these values are already display encoded
    if (sargs_exists("render_mode")) {
        const char* mode = sargs_value("render_mode");
        assert(g_state->renderer.splat_render_mode == (!strcmp(mode, "gps") ? SplatRenderMode::GaussianPointSplatting :
            !strcmp(mode, "stochastic") ? SplatRenderMode::StochasticSplats : SplatRenderMode::AlphaBlendSorted));
    }
    const char* capture = sargs_value("capture");
    if (capture[0]) {
        int w = sapp_width(), h = sapp_height();
        std::vector<unsigned char> pixels(size_t(w)*h*3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        FILE* f = fopen(capture, "wb"); assert(f);
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h-1; y >= 0; --y) assert(fwrite(pixels.data()+size_t(y)*w*3, 1, size_t(w)*3, f) == size_t(w)*3);
        fclose(f);
    }
    const char* linear_capture = sargs_value("linear_capture");
    if (linear_capture[0]) {
        int w = sapp_width(), h = sapp_height();
        std::vector<float> pixels(size_t(w)*h*4);
        const auto& r = g_state->renderer;
        sg_image target = r.appearance_image;
        if (r.splat_render_mode != SplatRenderMode::AlphaBlendSorted) {
            target = r.stochastic_accumulation_enabled ||
                (r.splat_render_mode == SplatRenderMode::StochasticSplats && r.stochastic_taa_enabled)
                ? r.stochastic_accum_images[(r.stochastic_accum_write_index + 1u) & 1u]
                : r.splat_render_mode == SplatRenderMode::GaussianPointSplatting
                    ? r.gps_gpu.resolved_color_image : r.stochastic_sample_image;
        }
        sg_gl_image_info image = sg_gl_query_image_info(target);
        glBindTexture(GL_TEXTURE_2D, image.tex[image.active_slot]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
        FILE* f = fopen(linear_capture, "wb"); assert(f);
        // Raw float32 RGBA, top row first; read with NumPy fromfile/reshape.
        for (int y = h-1; y >= 0; --y)
            assert(fwrite(pixels.data()+size_t(y)*w*4, sizeof(float), size_t(w)*4, f) == size_t(w)*4);
        fclose(f);
    }
    std::vector<double> wall, cpu, gpu;
    const char* csv_path = sargs_value("csv");
    FILE* csv = csv_path[0] ? fopen(csv_path, "w") : nullptr;
    if (csv) fprintf(csv, "wall_ms,cpu_submit_ms,gpu_elapsed_ms,animation_time,state,selected_state,playing\n");
    for (const auto& m : measurements) {
        GLuint64 ns = 0; glGetQueryObjectui64v(m.query, GL_QUERY_RESULT, &ns);
        double ms = double(ns)/1e6;
        wall.push_back(m.wall_ms); cpu.push_back(m.cpu_ms); gpu.push_back(ms);
        if (csv) fprintf(csv, "%.6f,%.6f,%.6f,%.9f,%d,%d,%d\n", m.wall_ms, m.cpu_ms, ms,
            m.animation_time, m.state, m.selected_state, m.playing ? 1 : 0);
        glDeleteQueries(1, &m.query);
    }
    if (csv) fclose(csv);
    GLint free_kb = 0;
    if (memory_supported) glGetIntegerv(0x9049, &free_kb);
    fprintf(stdout, "RESULT {\"frames\":%zu,\"wall_median_ms\":%.6f,\"wall_p95_ms\":%.6f,"
        "\"cpu_median_ms\":%.6f,\"cpu_p95_ms\":%.6f,\"gpu_median_ms\":%.6f,\"gpu_p95_ms\":%.6f,"
        "\"startup_ms\":%.3f,\"vram_free_delta_mib\":%.3f,\"scene_buffers_mib\":%.3f}\n",
        wall.size(), percentile(wall,.5), percentile(wall,.95), percentile(cpu,.5), percentile(cpu,.95),
        percentile(gpu,.5), percentile(gpu,.95), startup_ms, memory_supported ? (initial_free_kb-free_kb)/1024.0 : -1,
        (renderer_scene_buffer_bytes(g_state->scene.gaussian_count, g_state->renderer.gps_max_work_items)+g_state->renderer.animation_bytes)/1048576.0);
    sapp_quit();
}

extern "C" sapp_desc sokol_main(int argc, char** argv) {
    sapp_desc desc = gsplat_interactive_main(argc, argv);
    desc.init_cb = measure_init;
    desc.frame_cb = measure_frame;
    desc.width = 1920; desc.height = 1080;
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "width=", 6)) desc.width = atoi(argv[i]+6);
        if (!strncmp(argv[i], "height=", 7)) desc.height = atoi(argv[i]+7);
    }
    requested_width = desc.width; requested_height = desc.height;
    desc.window_title = "GSAnim validation";
    return desc;
}
