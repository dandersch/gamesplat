#ifndef GAMESPLAT_PROFILER_H
#define GAMESPLAT_PROFILER_H

#if defined(ENABLE_PROFILER)

#ifndef TRACY_ENABLE
#define TRACY_ENABLE
#endif

#include "tracy/TracyC.h"

#define PROFILER_TOKEN_PASTE(a, b) a##b
#define PROFILER_CONCAT(a, b) PROFILER_TOKEN_PASTE(a, b)
#define PROFILER_UQ(name) PROFILER_CONCAT(name, __LINE__)

struct ProfilerContextStack {
    TracyCZoneCtx zones[64];
    int count;
};

static inline ProfilerContextStack* profiler_context_stack(void) {
    static thread_local ProfilerContextStack stack = {};
    return &stack;
}

static inline void profiler_begin(const struct ___tracy_source_location_data* location) {
    ProfilerContextStack* stack = profiler_context_stack();
    if (stack->count < (int)(sizeof(stack->zones) / sizeof(stack->zones[0]))) {
        stack->zones[stack->count++] = ___tracy_emit_zone_begin(location, 1);
    }
}

static inline void profiler_end(void) {
    ProfilerContextStack* stack = profiler_context_stack();
    if (stack->count > 0) {
        TracyCZoneEnd(stack->zones[--stack->count]);
    }
}

#define PROFILE_BEGIN(name) \
    do { \
        static const struct ___tracy_source_location_data PROFILER_UQ(_profiler_location_) = { name, __func__, __FILE__, (uint32_t)__LINE__, 0 }; \
        profiler_begin(&PROFILER_UQ(_profiler_location_)); \
    } while (0)

#define PROFILE_END() \
    do { \
        profiler_end(); \
    } while (0)

#define PROFILE_FRAME() TracyCFrameMark

#define PROFILE(name) \
    if (static const struct ___tracy_source_location_data _profiler_location = { name, __func__, __FILE__, (uint32_t)__LINE__, 0 }; false) { \
    } else \
        for (int PROFILER_UQ(_profiler_once_) = (profiler_begin(&_profiler_location), 0); \
             PROFILER_UQ(_profiler_once_) == 0; \
             PROFILER_UQ(_profiler_once_) += 1, profiler_end())

#if defined(SOKOL_GLCORE)

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <SDL3/SDL_opengl.h>
#include <new>
#include "tracy/TracyOpenGL.hpp"

struct ProfilerGpuContextStack {
    alignas(tracy::GpuCtxScope) unsigned char zones[64][sizeof(tracy::GpuCtxScope)];
    int count;
};

static inline ProfilerGpuContextStack* profiler_gpu_context_stack(void) {
    static thread_local ProfilerGpuContextStack stack = {};
    return &stack;
}

static inline void profiler_gpu_begin(const tracy::SourceLocationData* location) {
    ProfilerGpuContextStack* stack = profiler_gpu_context_stack();
    if (stack->count < (int)(sizeof(stack->zones) / sizeof(stack->zones[0]))) {
        new (stack->zones[stack->count++]) tracy::GpuCtxScope(location, true);
    }
}

static inline void profiler_gpu_end(void) {
    ProfilerGpuContextStack* stack = profiler_gpu_context_stack();
    if (stack->count > 0) {
        reinterpret_cast<tracy::GpuCtxScope*>(stack->zones[--stack->count])->~GpuCtxScope();
    }
}

#define PROFILE_GPU_CONTEXT() do { TracyGpuContext; } while (0)
#define PROFILE_GPU_COLLECT() do { TracyGpuCollect; } while (0)

#define PROFILE_GPU_BEGIN(name) \
    do { \
        static constexpr tracy::SourceLocationData PROFILER_UQ(_profiler_gpu_location_) = { name, __func__, __FILE__, (uint32_t)__LINE__, 0 }; \
        profiler_gpu_begin(&PROFILER_UQ(_profiler_gpu_location_)); \
    } while (0)

#define PROFILE_GPU_END() \
    do { \
        profiler_gpu_end(); \
    } while (0)

#define PROFILE_GPU(name) \
    if (static constexpr tracy::SourceLocationData _profiler_gpu_location = { name, __func__, __FILE__, (uint32_t)__LINE__, 0 }; false) { \
    } else \
        for (int PROFILER_UQ(_profiler_gpu_once_) = (profiler_gpu_begin(&_profiler_gpu_location), 0); \
             PROFILER_UQ(_profiler_gpu_once_) == 0; \
             PROFILER_UQ(_profiler_gpu_once_) += 1, profiler_gpu_end())

#elif defined(SOKOL_D3D11)

// TODO: Tracy D3D11 GPU profiling needs access to native D3D11 device/context handles.
#define PROFILE_GPU_CONTEXT() do { } while (0)
#define PROFILE_GPU_COLLECT() do { } while (0)
#define PROFILE_GPU_BEGIN(name) do { (void)(name); } while (0)
#define PROFILE_GPU_END() do { } while (0)
#define PROFILE_GPU(name) \
    for (int PROFILER_UQ(_profiler_gpu_once_) = 0; \
         PROFILER_UQ(_profiler_gpu_once_) == 0; \
         PROFILER_UQ(_profiler_gpu_once_) += 1)

#elif defined(SOKOL_METAL)

// TODO: Tracy Metal GPU profiling needs access to native Metal device/command-buffer state.
#define PROFILE_GPU_CONTEXT() do { } while (0)
#define PROFILE_GPU_COLLECT() do { } while (0)
#define PROFILE_GPU_BEGIN(name) do { (void)(name); } while (0)
#define PROFILE_GPU_END() do { } while (0)
#define PROFILE_GPU(name) \
    for (int PROFILER_UQ(_profiler_gpu_once_) = 0; \
         PROFILER_UQ(_profiler_gpu_once_) == 0; \
         PROFILER_UQ(_profiler_gpu_once_) += 1)

#elif defined(SOKOL_VULKAN)

// TODO: Tracy Vulkan GPU profiling needs access to native Vulkan device/queue/command-buffer state.
#define PROFILE_GPU_CONTEXT() do { } while (0)
#define PROFILE_GPU_COLLECT() do { } while (0)
#define PROFILE_GPU_BEGIN(name) do { (void)(name); } while (0)
#define PROFILE_GPU_END() do { } while (0)
#define PROFILE_GPU(name) \
    for (int PROFILER_UQ(_profiler_gpu_once_) = 0; \
         PROFILER_UQ(_profiler_gpu_once_) == 0; \
         PROFILER_UQ(_profiler_gpu_once_) += 1)

#else

// GPU profiling is not available for this sokol backend/configuration yet.
#define PROFILE_GPU_CONTEXT() do { } while (0)
#define PROFILE_GPU_COLLECT() do { } while (0)
#define PROFILE_GPU_BEGIN(name) do { (void)(name); } while (0)
#define PROFILE_GPU_END() do { } while (0)
#define PROFILE_GPU(name) \
    for (int PROFILER_UQ(_profiler_gpu_once_) = 0; \
         PROFILER_UQ(_profiler_gpu_once_) == 0; \
         PROFILER_UQ(_profiler_gpu_once_) += 1)

#endif

#else

#define PROFILER_TOKEN_PASTE(a, b) a##b
#define PROFILER_CONCAT(a, b) PROFILER_TOKEN_PASTE(a, b)
#define PROFILER_UQ(name) PROFILER_CONCAT(name, __LINE__)

#define PROFILE_BEGIN(name) do { (void)(name); } while (0)
#define PROFILE_END() do { } while (0)
#define PROFILE_FRAME() do { } while (0)
#define PROFILE(name) \
    for (int PROFILER_UQ(_profiler_once_) = 0; \
         PROFILER_UQ(_profiler_once_) == 0; \
         PROFILER_UQ(_profiler_once_) += 1)

#define PROFILE_GPU_CONTEXT() do { } while (0)
#define PROFILE_GPU_COLLECT() do { } while (0)
#define PROFILE_GPU_BEGIN(name) do { (void)(name); } while (0)
#define PROFILE_GPU_END() do { } while (0)
#define PROFILE_GPU(name) \
    for (int PROFILER_UQ(_profiler_gpu_once_) = 0; \
         PROFILER_UQ(_profiler_gpu_once_) == 0; \
         PROFILER_UQ(_profiler_gpu_once_) += 1)

#endif

#endif
