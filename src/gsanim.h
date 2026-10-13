#pragma once
#include <cstdint>
#include <vector>

struct GaussianScene;

struct GSAnimKey {
    double time_seconds;
    uint32_t state;
};

struct GSAnim {
    // State-major, original Gaussian row order, channel-major SH; excludes state 0.
    std::vector<int8_t> deltas;
    float scales[48] = {};
    uint32_t state_count = 0;
    uint32_t gaussian_count = 0;
    double duration = 0;
    std::vector<GSAnimKey> keys;
    bool linear = false;
    double playback_time = 0;
    bool playing = true;
    int selected_state = -1; // UI override only; gsanim_sample ignores this.
};

struct GSAnimSample {
    int state_a, state_b;
    float weight;
};

// POC limits: 1M Gaussians, 256 states, 65536 keys, 1 MiB manifest/header,
// 256 MiB deltas, 512 MiB archive. No GPU allocation or filesystem extraction.
// Requires an empty scene. Failure leaves it untouched; free_scene owns success.
bool gaussian_load_gsanim(const char* path, GaussianScene* scene);

// Validated animation and finite seconds expected. Invalid/nonfinite input returns
// {0,0,0}. Exact keys select the new key; negative time wraps; one key is constant.
GSAnimSample gsanim_sample(const GSAnim& animation, double seconds);
