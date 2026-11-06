#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <vector>

struct GaussianScene;

struct GSAnimKey {
    double time_seconds;
    uint32_t state;
    bool linear = false; // Outgoing interval in 0.2.
    bool has_interpolation = false;
};

struct GSAnimLayerKey {
    double time_seconds = 0;
    std::array<float, 3> weight{};
    bool linear = false;
};

struct GSAnimLayer {
    std::string name;
    std::vector<GSAnimLayerKey> keys;
};

struct GSAnim {
    int version = 1;
    std::vector<GSAnimLayer> layers;
    // Raw IEEE half bits, layer/row/channel-major (48 coefficients per row).
    std::vector<uint16_t> layer_data;
    // RGB uint16 UNORM, red-fastest grid order.
    std::vector<uint16_t> display_lut;
    uint32_t lut_size = 0;
    float log2_min = 0, log2_max = 0;
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

// POC limits: 1M Gaussians, 256 states/layers, 65536 keys per track,
// 1 MiB manifest/header, 256 MiB delta/layer coefficients, 512 MiB archive.
// No GPU allocation or filesystem extraction.
// Requires an empty scene. Failure leaves it untouched; free_scene owns success.
bool gaussian_load_gsanim(const char* path, GaussianScene* scene);

// Validated animation and finite seconds expected. Invalid/nonfinite input returns
// {0,0,0}. Exact keys select the new key; negative time wraps; one key is constant.
GSAnimSample gsanim_sample(const GSAnim& animation, double seconds);

// Empty for legacy assets or invalid/nonfinite playback input.
std::vector<std::array<float, 3>> gsanim_layer_weights(const GSAnim& animation, double seconds);
