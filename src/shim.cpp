#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#define SOKOL_NO_ENTRY
#define SOKOL_APP_IMPL
#if !defined(SOKOL_GLCORE)
#define SOKOL_GLCORE
#endif
#include "sokol_app.h"

#include "log.h"
int log_verbosity_level = LOG_EVERYTHING;

/*
    sokol_app hot-reload shim
    =========================

    The shim owns the non-reloadable sokol_app window/event loop and loads the
    actual application code from build/hotreload/gsplat_code.so.  On startup it
    asks the code module for its sapp_desc via sokol_main(), copies the window
    setup fields, and replaces the callbacks with shim callbacks.  Each shim
    callback mirrors the live sokol_app state into the code module, calls the
    current code callback, then mirrors state back so sapp_quit(), mouse lock,
    frame counters, etc. stay coherent.

    On reload, the shim preserves the private single-header-library state that
    lives in the code module (sokol_gfx, sokol_imgui, sokol_app and the app's
    AppState pointer), unloads the old .so, loads a uniquely named copy of the
    new .so, restores the snapshots, and continues dispatching callbacks.
*/

#if defined(_WIN32)
#define GSPLAT_CODE_PATH "build\\hotreload\\gsplat_code.dll"
#else
#define GSPLAT_CODE_PATH "./build/hotreload/gsplat_code.so"
#endif

#define GSPLAT_RELOAD_DEBOUNCE_MS 300

struct CodeTimestamp {
    dev_t dev;
    ino_t ino;
    off_t size;
    time_t mtime_sec;
    long mtime_nsec;
};

struct CodeApi {
    void* handle;
    CodeTimestamp timestamp;
    char loaded_path[512];
    sapp_desc desc;
    sapp_desc (*sokol_main_fn)(int, char**);
    size_t (*sg_state_size)(void);
    void (*sg_state_save)(void*, size_t);
    void (*sg_state_load)(const void*, size_t);
    size_t (*simgui_state_size)(void);
    void (*simgui_state_save)(void*, size_t);
    void (*simgui_state_load)(const void*, size_t);
    size_t (*sapp_state_size)(void);
    void (*sapp_state_save)(void*, size_t);
    void (*sapp_state_load)(const void*, size_t);
    void* (*app_state_save)(void);
    void (*app_state_load)(void*);
    void (*after_state_restore)(void);
};

struct RuntimeSnapshot {
    void* sg_state;
    size_t sg_state_size;
    void* simgui_state;
    size_t simgui_state_size;
    void* sapp_state;
    size_t sapp_state_size;
    void* app_state;
};

static CodeApi g_code = {};
static int g_argc = 0;
static char** g_argv = NULL;
static CodeTimestamp g_pending_timestamp = {};
static bool g_has_pending_timestamp = false;
static uint64_t g_pending_since = 0;

static uint64_t shim_ticks_ms(void) {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void* shim_load_symbol(void* handle, const char* name) {
    return dlsym(handle, name);
}

static size_t shim_sapp_state_size(void) {
    return sizeof(_sapp);
}

static void shim_sapp_state_save(void* dst, size_t dst_size) {
    if (dst && dst_size == sizeof(_sapp)) {
        memcpy(dst, &_sapp, sizeof(_sapp));
    }
}

static void shim_sapp_state_load(const void* src, size_t src_size) {
    if (src && src_size == sizeof(_sapp)) {
        memcpy(&_sapp, src, sizeof(_sapp));
    }
}

static void snapshot_free(RuntimeSnapshot* snapshot) {
    if (!snapshot) return;
    free(snapshot->sg_state);
    free(snapshot->simgui_state);
    free(snapshot->sapp_state);
    *snapshot = {};
}

static bool snapshot_capture(const CodeApi* code, RuntimeSnapshot* out) {
    snapshot_free(out);
    if (!code || !code->handle) return true;

    if (code->sg_state_size && code->sg_state_save) {
        out->sg_state_size = code->sg_state_size();
        out->sg_state = malloc(out->sg_state_size);
        if (!out->sg_state) return false;
        code->sg_state_save(out->sg_state, out->sg_state_size);
    }

    if (code->simgui_state_size && code->simgui_state_save) {
        out->simgui_state_size = code->simgui_state_size();
        out->simgui_state = malloc(out->simgui_state_size);
        if (!out->simgui_state) {
            snapshot_free(out);
            return false;
        }
        code->simgui_state_save(out->simgui_state, out->simgui_state_size);
    }

    if (code->sapp_state_size && code->sapp_state_save) {
        out->sapp_state_size = code->sapp_state_size();
        out->sapp_state = malloc(out->sapp_state_size);
        if (!out->sapp_state) {
            snapshot_free(out);
            return false;
        }
        code->sapp_state_save(out->sapp_state, out->sapp_state_size);
    }

    if (code->app_state_save) {
        out->app_state = code->app_state_save();
    }

    return true;
}

static void snapshot_restore(const RuntimeSnapshot* snapshot, const CodeApi* code) {
    if (!snapshot || !code) return;
    if (snapshot->sapp_state && code->sapp_state_load) {
        code->sapp_state_load(snapshot->sapp_state, snapshot->sapp_state_size);
    }
    if (snapshot->sg_state && code->sg_state_load) {
        code->sg_state_load(snapshot->sg_state, snapshot->sg_state_size);
    }
    if (snapshot->simgui_state && code->simgui_state_load) {
        code->simgui_state_load(snapshot->simgui_state, snapshot->simgui_state_size);
    }
    if (code->app_state_load) {
        code->app_state_load(snapshot->app_state);
    }
    if (code->after_state_restore) {
        code->after_state_restore();
    }
}

static bool code_stat(const char* path, CodeTimestamp* out) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    out->dev = st.st_dev;
    out->ino = st.st_ino;
    out->size = st.st_size;
#if defined(__APPLE__)
    out->mtime_sec = st.st_mtimespec.tv_sec;
    out->mtime_nsec = st.st_mtimespec.tv_nsec;
#else
    out->mtime_sec = st.st_mtim.tv_sec;
    out->mtime_nsec = st.st_mtim.tv_nsec;
#endif
    return true;
}

static bool code_timestamp_equal(const CodeTimestamp* a, const CodeTimestamp* b) {
    return a->dev == b->dev &&
           a->ino == b->ino &&
           a->size == b->size &&
           a->mtime_sec == b->mtime_sec &&
           a->mtime_nsec == b->mtime_nsec;
}

static bool code_copy_for_loading(const CodeTimestamp* timestamp, char* out_path, size_t out_path_size) {
    snprintf(out_path, out_path_size,
             "./build/hotreload/gsplat_code_loaded_%lld_%lld_%lld_%ld.so",
             (long long)timestamp->ino,
             (long long)timestamp->size,
             (long long)timestamp->mtime_sec,
             timestamp->mtime_nsec);

    FILE* src = fopen(GSPLAT_CODE_PATH, "rb");
    if (!src) {
        LOG(ERROR|APP|IO, "shim: fopen(%s) failed", GSPLAT_CODE_PATH);
        return false;
    }

    FILE* dst = fopen(out_path, "wb");
    if (!dst) {
        LOG(ERROR|APP|IO, "shim: fopen(%s) failed", out_path);
        fclose(src);
        return false;
    }

    uint8_t buf[64 * 1024];
    bool ok = true;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), src);
        if (n > 0 && fwrite(buf, 1, n, dst) != n) {
            ok = false;
            break;
        }
        if (n < sizeof(buf)) {
            if (ferror(src)) ok = false;
            break;
        }
    }

    fclose(dst);
    fclose(src);

    if (!ok) {
        LOG(ERROR|APP|IO, "shim: copy %s -> %s failed", GSPLAT_CODE_PATH, out_path);
        remove(out_path);
    }
    return ok;
}

static bool code_load(CodeApi* out, const CodeTimestamp* timestamp) {
    CodeApi api = {};
    if (!code_copy_for_loading(timestamp, api.loaded_path, sizeof(api.loaded_path))) {
        return false;
    }

    api.handle = dlopen(api.loaded_path, RTLD_NOW | RTLD_LOCAL);
    if (!api.handle) {
        LOG(ERROR|APP|LOAD, "shim: dlopen(%s) failed: %s", api.loaded_path, dlerror());
        remove(api.loaded_path);
        return false;
    }

    api.sokol_main_fn = (sapp_desc (*)(int, char**))shim_load_symbol(api.handle, "sokol_main");
    api.sg_state_size = (size_t (*)(void))shim_load_symbol(api.handle, "gsplat_sg_state_size");
    api.sg_state_save = (void (*)(void*, size_t))shim_load_symbol(api.handle, "gsplat_sg_state_save");
    api.sg_state_load = (void (*)(const void*, size_t))shim_load_symbol(api.handle, "gsplat_sg_state_load");
    api.simgui_state_size = (size_t (*)(void))shim_load_symbol(api.handle, "gsplat_simgui_state_size");
    api.simgui_state_save = (void (*)(void*, size_t))shim_load_symbol(api.handle, "gsplat_simgui_state_save");
    api.simgui_state_load = (void (*)(const void*, size_t))shim_load_symbol(api.handle, "gsplat_simgui_state_load");
    api.sapp_state_size = (size_t (*)(void))shim_load_symbol(api.handle, "gsplat_sapp_state_size");
    api.sapp_state_save = (void (*)(void*, size_t))shim_load_symbol(api.handle, "gsplat_sapp_state_save");
    api.sapp_state_load = (void (*)(const void*, size_t))shim_load_symbol(api.handle, "gsplat_sapp_state_load");
    api.app_state_save = (void* (*)(void))shim_load_symbol(api.handle, "gsplat_app_state_save");
    api.app_state_load = (void (*)(void*))shim_load_symbol(api.handle, "gsplat_app_state_load");
    api.after_state_restore = (void (*)(void))shim_load_symbol(api.handle, "gsplat_hot_reload_after_state_restore");

    if (!api.sokol_main_fn) {
        LOG(ERROR|APP|LOAD, "shim: %s is missing sokol_main", api.loaded_path);
        dlclose(api.handle);
        remove(api.loaded_path);
        return false;
    }

    api.desc = api.sokol_main_fn(g_argc, g_argv);
    if (!api.desc.init_cb || !api.desc.frame_cb || !api.desc.cleanup_cb || !api.desc.event_cb) {
        LOG(ERROR|APP|LOAD, "shim: %s returned an incomplete sapp_desc", api.loaded_path);
        dlclose(api.handle);
        remove(api.loaded_path);
        return false;
    }

    api.timestamp = *timestamp;
    *out = api;
    return true;
}

static void code_unload(CodeApi* code) {
    if (!code || !code->handle) return;
    char old_loaded_path[sizeof(code->loaded_path)];
    snprintf(old_loaded_path, sizeof(old_loaded_path), "%s", code->loaded_path);
    dlclose(code->handle);
    remove(old_loaded_path);
    *code = {};
}

static void sync_shim_sapp_to_code(const CodeApi* code) {
    if (!code || !code->handle || !code->sapp_state_load) return;
    size_t size = shim_sapp_state_size();
    void* state = malloc(size);
    if (!state) return;
    shim_sapp_state_save(state, size);
    code->sapp_state_load(state, size);
    free(state);
}

static void sync_code_sapp_to_shim(const CodeApi* code) {
    if (!code || !code->handle || !code->sapp_state_size || !code->sapp_state_save) return;
    size_t size = code->sapp_state_size();
    if (size != shim_sapp_state_size()) return;
    void* state = malloc(size);
    if (!state) return;
    code->sapp_state_save(state, size);
    shim_sapp_state_load(state, size);
    free(state);
}

static bool perform_reload(const CodeTimestamp* timestamp) {
    RuntimeSnapshot snapshot = {};
    if (g_code.handle) {
        sync_shim_sapp_to_code(&g_code);
        if (!snapshot_capture(&g_code, &snapshot)) {
            LOG(ERROR|APP|RESOURCE, "shim: failed to capture runtime state before reload");
            return false;
        }
        code_unload(&g_code);
    }

    CodeApi next = {};
    if (!code_load(&next, timestamp)) {
        snapshot_free(&snapshot);
        return false;
    }

    if (snapshot.sapp_state) {
        snapshot_restore(&snapshot, &next);
    } else {
        sync_shim_sapp_to_code(&next);
        if (next.desc.init_cb) {
            next.desc.init_cb();
        }
        sync_code_sapp_to_shim(&next);
    }

    snapshot_free(&snapshot);
    g_code = next;
    LOG(INFO|APP|LOAD, "shim: loaded %s from %s", g_code.loaded_path, GSPLAT_CODE_PATH);
    return true;
}

static void check_reload(void) {
    CodeTimestamp timestamp = {};
    if (code_stat(GSPLAT_CODE_PATH, &timestamp)) {
        bool changed = !g_code.handle || !code_timestamp_equal(&timestamp, &g_code.timestamp);
        if (changed && (!g_has_pending_timestamp || !code_timestamp_equal(&timestamp, &g_pending_timestamp))) {
            g_pending_timestamp = timestamp;
            g_has_pending_timestamp = true;
            g_pending_since = shim_ticks_ms();
        } else if (!changed) {
            g_has_pending_timestamp = false;
        }
    }

    if (g_has_pending_timestamp && shim_ticks_ms() - g_pending_since >= GSPLAT_RELOAD_DEBOUNCE_MS) {
        if (perform_reload(&g_pending_timestamp)) {
            g_has_pending_timestamp = false;
        }
    }
}

static void shim_init(void) {
    if (g_code.handle) {
        sync_shim_sapp_to_code(&g_code);
        g_code.desc.init_cb();
        sync_code_sapp_to_shim(&g_code);
    }
}

static void shim_frame(void) {
    check_reload();
    if (g_code.handle) {
        sync_shim_sapp_to_code(&g_code);
        g_code.desc.frame_cb();
        sync_code_sapp_to_shim(&g_code);
    }
}

static void shim_cleanup(void) {
    if (g_code.handle) {
        sync_shim_sapp_to_code(&g_code);
        g_code.desc.cleanup_cb();
        sync_code_sapp_to_shim(&g_code);
        code_unload(&g_code);
    }
}

static void shim_event(const sapp_event* event) {
    if (g_code.handle) {
        sync_shim_sapp_to_code(&g_code);
        g_code.desc.event_cb(event);
        sync_code_sapp_to_shim(&g_code);
    }
}

int main(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;

    CodeTimestamp timestamp = {};
    if (!code_stat(GSPLAT_CODE_PATH, &timestamp) || !code_load(&g_code, &timestamp)) {
        LOG(ERROR|APP|LOAD, "shim: failed to load initial code module %s", GSPLAT_CODE_PATH);
        return 1;
    }

    sapp_desc desc = g_code.desc;
    desc.init_cb = shim_init;
    desc.frame_cb = shim_frame;
    desc.cleanup_cb = shim_cleanup;
    desc.event_cb = shim_event;
    desc.logger.func = NULL;
    desc.user_data = NULL;
    desc.init_userdata_cb = NULL;
    desc.frame_userdata_cb = NULL;
    desc.cleanup_userdata_cb = NULL;
    desc.event_userdata_cb = NULL;

    sapp_run(&desc);
    return 0;
}
