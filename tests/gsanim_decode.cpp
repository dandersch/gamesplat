// CPU-only conformance probe using the same vendor parsers as the app.
#define SJ_IMPL
#include "sj.h"
#include "src/gsanim.h"
#include "src/gaussian.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void release(GaussianScene& s) {
    delete s.animation;
    SDL_free(s.gaussians);
    SDL_free(s.visible_indices); SDL_free(s.visible_depths);
    SDL_free(s.sorted_indices); SDL_free(s.scratch_indices);
    SDL_free(s.scratch_keys); SDL_free(s.scratch_keys2);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    GaussianScene scene{};
    if (!gaussian_load_gsanim(argv[1], &scene)) {
        assert(!scene.animation && !scene.gaussians && !scene.gaussian_count);
        assert(!scene.visible_indices && !scene.visible_depths && !scene.sorted_indices);
        assert(!scene.scratch_indices && !scene.scratch_keys && !scene.scratch_keys2);
        return 1;
    }
    assert(scene.animation && scene.visible_indices && scene.visible_depths && scene.sorted_indices);
    assert(scene.scratch_indices && scene.scratch_keys && scene.scratch_keys2 && !scene.visible_count);
    GSAnim& a = *scene.animation;
    assert(a.gaussian_count == scene.gaussian_count);
    assert(a.playing && a.playback_time == 0 && a.selected_state == -1);
    // A second load must not leak/overwrite a live scene.
    assert(!gaussian_load_gsanim(argv[1], &scene));
    a.selected_state = 1; // explicitly verify the sampling API ignores UI override
    if (a.version == 3 && !(argc == 3 && !strcmp(argv[2], "geometry"))) {
        assert(a.deltas.empty() && a.keys.empty() && !a.state_count);
        assert(gsanim_layer_weights(a, NAN).empty());
        assert(gsanim_layer_weights(a, INFINITY).empty());
        if (argc == 3 && !strcmp(argv[2], "layers")) {
            fwrite(a.layer_data.data(), 2, a.layer_data.size(), stdout);
        } else if (argc == 3 && !strcmp(argv[2], "lut")) {
            fwrite(a.display_lut.data(), 2, a.display_lut.size(), stdout);
            fprintf(stderr, "%u %.9g %.9g\n", a.lut_size, a.log2_min, a.log2_max);
        } else if (argc == 3 && !strcmp(argv[2], "names")) {
            for (const auto& layer : a.layers) {
                uint32_t n = uint32_t(layer.name.size());
                fwrite(&n, 4, 1, stdout);
                fwrite(layer.name.data(), 1, n, stdout);
            }
        } else for (int t = 2; t < argc; ++t) {
            auto weights = gsanim_layer_weights(a, strtod(argv[t], nullptr));
            assert(weights.size() == a.layers.size());
            for (const auto& w : weights) fwrite(w.data(), 4, 3, stdout);
        }
    } else if (argc == 3 && !strcmp(argv[2], "geometry")) {
        for (uint32_t i = 0; i < scene.gaussian_count; ++i) {
            const Gaussian& g = scene.gaussians[i];
            fwrite(g.position, 4, 3, stdout); fwrite(g.scale, 4, 3, stdout);
            fwrite(g.rotation, 4, 4, stdout); fwrite(&g.opacity, 4, 1, stdout);
        }
    } else for (int t = 2; t < argc; ++t) {
        GSAnimSample sample = gsanim_sample(a, strtod(argv[t], nullptr));
        fprintf(stderr, "%d %d %.9g\n", sample.state_a, sample.state_b, sample.weight);
        for (uint32_t i = 0; i < scene.gaussian_count; ++i) {
            const Gaussian& g = scene.gaussians[i];
            float coefficients[48];
            for (int j = 0; j < 48; ++j) {
                float base = j < 3 ? g.color[j] : g.sh_rest[((j-3)%15)*3+(j-3)/15];
                auto state = [&](int s) {
                    return s == 0 ? base : base + float(a.deltas[(size_t(s-1)*a.gaussian_count+i)*48+j]) * a.scales[j];
                };
                coefficients[j] = (1.0f-sample.weight)*state(sample.state_a) + sample.weight*state(sample.state_b);
            }
            if (fwrite(coefficients, 4, 48, stdout) != 48) { release(scene); return 2; }
        }
    }
    release(scene);
    return ferror(stdout) ? 2 : 0;
}
