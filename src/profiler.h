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
    for (int PROFILER_UQ(_profiler_once_) = (PROFILE_BEGIN(name), 0); \
         PROFILER_UQ(_profiler_once_) == 0; \
         PROFILER_UQ(_profiler_once_) += 1, PROFILE_END())

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

#endif

#endif
