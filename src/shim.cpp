#include <SDL3/SDL.h>
#include <SDL3/SDL_loadso.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <sys/stat.h>

/*
    Minimal SDL callback hot-reload shim
    ===================================

    This executable intentionally owns as little application setup as possible.
    SDL, the window, GL context, sokol_gfx, sokol_imgui, ImGui, scene loading,
    renderer setup, etc. are still initialized from the reloadable code module's
    SDL_AppInit(). The shim only:

      - watches build/hotreload/gsplat_code.so for changes,
      - dynamically loads the current code module,
      - resolves SDL_AppInit / SDL_AppEvent / SDL_AppIterate / SDL_AppQuit,
      - forwards SDL events and frame iteration to those callbacks,
      - preserves the callback appstate pointer across reloads.

    Important problems we ran into
    ------------------------------

    1. Loading the same shared-library path may return old code.

       On Linux, dlopen() may key loaded libraries by the path string passed to
       dlopen(), not by the current file contents behind that path. If we call
       SDL_LoadObject("./build/hotreload/gsplat_code.so") after rebuilding that
       same path, the loader can hand back the already-loaded image even though
       the file was replaced. This matches the warning from:

           https://nullprogram.com/blog/2014/12/23/

       Workaround here:

         a. unload the old SDL_SharedObject before loading replacement code, and
         b. never load the stable build output path directly.

       Instead, code_copy_for_loading() copies gsplat_code.so to a unique path
       containing inode/size/mtime, and SDL_LoadObject() loads that unique path.
       This also helps with C++ DSOs that may not fully unmap after dlclose()
       because of GNU-unique symbols or other dynamic-loader details: even if an
       old image remains mapped, a unique filename forces the loader to map a
       new image for the next build.

    2. Rebuilds produce multiple filesystem changes.

       The linker/build may update the .so several times during one build. When
       we reloaded immediately on any stat() change, one rebuild could print
       several "shim: loaded" messages and race partially-written output.

       Workaround here:

         - detect a changed timestamp,
         - remember it as pending,
         - wait GSPLAT_RELOAD_DEBOUNCE_MS before loading,
         - reset the debounce if another distinct timestamp appears.

    3. AppState alone is not enough for single-header library statics.

       Our AppState preserves project-owned state across reloads, but some of
       the third-party libraries we compile into the code module keep their own
       file-scope static state:

         - sokol_gfx.h owns private static _sg
         - sokol_imgui.h owns private static _simgui
         - Dear ImGui owns a DLL-local current-context pointer (GImGui)

       After a true reload, the new code module gets fresh zero-initialized
       copies of those statics. The first observable failure was
       ImGui_ImplSDL3_NewFrame() asserting because the new module's ImGui
       current context pointer was null / not pointing at the existing context.

       Workaround here:

         - vendor/third_party_impl.cpp exports gsplat_sg_state_* and
           gsplat_simgui_state_* helper functions from the same translation unit
           that defines SOKOL_IMPL / SOKOL_IMGUI_IMPL. Those helpers can see the
           otherwise-private _sg and _simgui objects and memcpy them out/in.
         - before unloading old code, snapshot_capture() copies _sg and _simgui
           into shim-owned heap buffers.
         - after loading new code, snapshot_restore() copies those blobs into
           the new module's _sg and _simgui.
         - src/main.cpp stores ImGuiContext* in AppState after initial setup and
           calls ImGui::SetCurrentContext(state->imgui_context) on reload.

       This keeps standalone builds clean: the same third_party_impl.cpp still
       supplies the implementations for a normal executable, and the hot-reload
       hooks are just extra exported symbols when the app is built as a shared
       object.

    Caveats
    -------

    - This is a proof-of-concept, not a general ABI-safe serialization layer.
      The _sg and _simgui structs are private implementation details. Copying
      them is only safe while the old and new code are built from compatible
      headers, compiler settings, backend selection, and struct layouts.

    - Do not change sokol_gfx.h, sokol_imgui.h, ImGui versions, relevant compile
      defines, or the third_party_impl.cpp layout while a hot-reloaded process is
      running. Rebuild/restart if those change.

    - Backend behavior may vary. The GitHub discussion that suggested this
      approach reported OpenGL and Metal working, with WebGPU being trickier.
      Our current native path is OpenGL, which is why this is plausible.

    - Function pointers inside copied third-party state are dangerous if they
      point at old module code. Current sokol state mostly contains backend
      resources, pools, handles, descriptors, allocator/logger callbacks, etc.;
      if we add callbacks that live in reloadable code, revisit this.

    - If the new shared object fails to load or initialize after the old one has
      been unloaded, there is no old code left to continue running. The shim will
      idle and keep trying when the stable .so changes again.

    - AppState layout itself is an ABI contract across reloads. Adding/removing
      fields while the process is running can make an old state block invalid for
      new code. For now, restart after AppState layout changes.

    - The shim removes unique copied libraries after unloading them, but if the
      process crashes, stale gsplat_code_loaded_*.so files may remain under
      build/hotreload/. They are disposable build artifacts.
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
    SDL_SharedObject* handle;
    CodeTimestamp timestamp;
    char loaded_path[512];
    SDL_AppResult (*app_init)(void**, int, char**);
    SDL_AppResult (*app_event)(void*, SDL_Event*);
    SDL_AppResult (*app_iterate)(void*);
    void (*app_quit)(void*, SDL_AppResult);
    size_t (*sg_state_size)(void);
    void (*sg_state_save)(void*, size_t);
    void (*sg_state_load)(const void*, size_t);
    size_t (*simgui_state_size)(void);
    void (*simgui_state_save)(void*, size_t);
    void (*simgui_state_load)(const void*, size_t);
};

struct RuntimeSnapshot {
    void* sg_state;
    size_t sg_state_size;
    void* simgui_state;
    size_t simgui_state_size;
};

static void snapshot_free(RuntimeSnapshot* snapshot) {
    if (!snapshot) return;
    free(snapshot->sg_state);
    free(snapshot->simgui_state);
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

    return true;
}

static void snapshot_restore(const RuntimeSnapshot* snapshot, const CodeApi* code) {
    if (!snapshot || !code) return;
    if (snapshot->sg_state && code->sg_state_load) {
        code->sg_state_load(snapshot->sg_state, snapshot->sg_state_size);
    }
    if (snapshot->simgui_state && code->simgui_state_load) {
        code->simgui_state_load(snapshot->simgui_state, snapshot->simgui_state_size);
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
        fprintf(stderr, "shim: fopen(%s) failed\n", GSPLAT_CODE_PATH);
        return false;
    }

    FILE* dst = fopen(out_path, "wb");
    if (!dst) {
        fprintf(stderr, "shim: fopen(%s) failed\n", out_path);
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
        fprintf(stderr, "shim: copy %s -> %s failed\n", GSPLAT_CODE_PATH, out_path);
        remove(out_path);
    }
    return ok;
}

static bool code_load(CodeApi* out, const CodeTimestamp* timestamp) {
    CodeApi api = {};
    if (!code_copy_for_loading(timestamp, api.loaded_path, sizeof(api.loaded_path))) {
        return false;
    }

    api.handle = SDL_LoadObject(api.loaded_path);
    if (!api.handle) {
        fprintf(stderr, "shim: SDL_LoadObject(%s) failed: %s\n", api.loaded_path, SDL_GetError());
        remove(api.loaded_path);
        return false;
    }

    api.app_init = (SDL_AppResult (*)(void**, int, char**))SDL_LoadFunction(api.handle, "SDL_AppInit");
    api.app_event = (SDL_AppResult (*)(void*, SDL_Event*))SDL_LoadFunction(api.handle, "SDL_AppEvent");
    api.app_iterate = (SDL_AppResult (*)(void*))SDL_LoadFunction(api.handle, "SDL_AppIterate");
    api.app_quit = (void (*)(void*, SDL_AppResult))SDL_LoadFunction(api.handle, "SDL_AppQuit");
    api.sg_state_size = (size_t (*)(void))SDL_LoadFunction(api.handle, "gsplat_sg_state_size");
    api.sg_state_save = (void (*)(void*, size_t))SDL_LoadFunction(api.handle, "gsplat_sg_state_save");
    api.sg_state_load = (void (*)(const void*, size_t))SDL_LoadFunction(api.handle, "gsplat_sg_state_load");
    api.simgui_state_size = (size_t (*)(void))SDL_LoadFunction(api.handle, "gsplat_simgui_state_size");
    api.simgui_state_save = (void (*)(void*, size_t))SDL_LoadFunction(api.handle, "gsplat_simgui_state_save");
    api.simgui_state_load = (void (*)(const void*, size_t))SDL_LoadFunction(api.handle, "gsplat_simgui_state_load");

    if (!api.app_init || !api.app_event || !api.app_iterate || !api.app_quit) {
        fprintf(stderr, "shim: %s is missing one or more SDL_App* exports\n", api.loaded_path);
        SDL_UnloadObject(api.handle);
        remove(api.loaded_path);
        return false;
    }

    api.timestamp = *timestamp;
    *out = api;
    return true;
}

int main(int argc, char** argv) {
    CodeApi code = {};
    CodeTimestamp pending_timestamp = {};
    bool has_pending_timestamp = false;
    uint64_t pending_since = 0;
    void* appstate = NULL;
    SDL_AppResult result = SDL_APP_CONTINUE;

    while (result == SDL_APP_CONTINUE) {
        CodeTimestamp timestamp = {};
        if (code_stat(GSPLAT_CODE_PATH, &timestamp)) {
            bool changed = !code.handle || !code_timestamp_equal(&timestamp, &code.timestamp);
            if (changed && (!has_pending_timestamp || !code_timestamp_equal(&timestamp, &pending_timestamp))) {
                pending_timestamp = timestamp;
                has_pending_timestamp = true;
                pending_since = SDL_GetTicks();
            } else if (!changed) {
                has_pending_timestamp = false;
            }
        }

        if (has_pending_timestamp && SDL_GetTicks() - pending_since >= GSPLAT_RELOAD_DEBOUNCE_MS) {
            RuntimeSnapshot snapshot = {};
            if (code.handle) {
                if (!snapshot_capture(&code, &snapshot)) {
                    fprintf(stderr, "shim: failed to capture runtime state before reload\n");
                    result = SDL_APP_FAILURE;
                    break;
                }
                char old_loaded_path[sizeof(code.loaded_path)];
                snprintf(old_loaded_path, sizeof(old_loaded_path), "%s", code.loaded_path);
                SDL_UnloadObject(code.handle);
                remove(old_loaded_path);
                code = {};
            }

            CodeApi next = {};
            if (code_load(&next, &pending_timestamp)) {
                snapshot_restore(&snapshot, &next);
                SDL_AppResult init_result = next.app_init(&appstate, argc, argv);
                if (init_result == SDL_APP_CONTINUE) {
                    code = next;
                    fprintf(stderr, "shim: loaded %s from %s\n", code.loaded_path, GSPLAT_CODE_PATH);
                    has_pending_timestamp = false;
                } else {
                    SDL_UnloadObject(next.handle);
                    remove(next.loaded_path);
                    result = init_result;
                }
            }
            snapshot_free(&snapshot);
        }

        if (!code.handle) {
            SDL_Delay(100);
            continue;
        }

        SDL_Event event;
        while (result == SDL_APP_CONTINUE && SDL_PollEvent(&event)) {
            result = code.app_event(appstate, &event);
        }

        if (result == SDL_APP_CONTINUE) {
            result = code.app_iterate(appstate);
        }
    }

    if (code.handle) {
        code.app_quit(appstate, result);
        SDL_UnloadObject(code.handle);
        remove(code.loaded_path);
    }

    return result == SDL_APP_FAILURE ? 1 : 0;
}
