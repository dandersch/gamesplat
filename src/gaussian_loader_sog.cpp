#include "gaussian_loader.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include "webp/decode.h"
#include "webp/types.h"
#include "sj.h"
#include "miniz.h"
#include "gaussian.h"
#include "maths.h"

// .sog parser & loader

struct SogArchiveFile {
    char        name[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];
    void*       data;
    size_t      size;
};

struct SogMeta {
    int      version;
    uint32_t count;
    bool     antialias;
    bool     has_antialias;

    float means_mins[3];
    float means_maxs[3];
    char  means_files[2][128];

    float scales_codebook[256];
    char  scales_file[128];

    char  quats_file[128];

    float sh0_codebook[256];
    char  sh0_file[128];

    bool  has_shN;
    int   shN_count;
    int   shN_bands;
    float shN_codebook[256];
    char  shN_files[2][128];
};

struct SogImage {
    uint8_t* pixels;
    int      width;
    int      height;
};

static void free_sog_archive_files(SogArchiveFile* files, int count) {
    for (int i = 0; i < count; i++) {
        free(files[i].data);
        files[i].data = NULL;
        files[i].size = 0;
    }
}

static void free_sog_image(SogImage* img) {
    if (img->pixels) WebPFree(img->pixels);
    *img = {};
}

static SogArchiveFile* find_sog_archive_file(SogArchiveFile* files, int count, const char* name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(files[i].name, name) == 0) return &files[i];
    }
    return NULL;
}

static bool parse_sog_codebook_file(sj_Reader* r, sj_Value obj, float* codebook, char file[128], const char* label) {
    bool has_codebook = false, has_files = false;
    char files[1][128] = {};
    if (!sjp_expect_object(r, obj)) return false;
    sj_Value key, val;
    while (sj_iter_object(r, obj, &key, &val)) {
        if (key.type != SJ_STRING) { sjp_set_error(r, "expected string"); return false; }
        if (sjp_eq(key, "codebook")) {
            if (!sjp_parse_float_array(r, val, codebook, 256)) return false;
            has_codebook = true;
        } else if (sjp_eq(key, "files")) {
            if (!sjp_parse_string_array(r, val, &files[0][0], 128, 1)) return false;
            snprintf(file, 128, "%s", files[0]);
            has_files = true;
        }
    }
    if (r->error) return false;
    if (!has_codebook || !has_files) {
        static char msg[96];
        snprintf(msg, sizeof(msg), "SOG %s missing required field", label);
        sjp_set_error(r, msg);
    }
    return !r->error;
}

static bool load_sog_image(SogArchiveFile* files, int file_count, const char* name, SogImage* out) {
    *out = {};
    SogArchiveFile* file = find_sog_archive_file(files, file_count, name);
    if (!file || !file->data) {
        LOG(ERROR|GAUSSIAN|LOAD, "SOG: missing image file: %s", name);
        return false;
    }

    // Web builds link libwebp into the WASM module and use this same decode
    // path. An alternative would be an async browser ImageBitmap/canvas bridge,
    // but SOG images are ZIP entries in WASM memory, so staying in C keeps the
    // loader synchronous and avoids browser color-management surprises.
    int w = 0, h = 0;
    if (!WebPGetInfo((const uint8_t*)file->data, file->size, &w, &h) || w <= 0 || h <= 0) {
        LOG(ERROR|GAUSSIAN|PARSE, "SOG: invalid WebP image: %s", name);
        return false;
    }
    uint8_t* rgba = WebPDecodeRGBA((const uint8_t*)file->data, file->size, &w, &h);
    if (!rgba) {
        LOG(ERROR|GAUSSIAN|PARSE, "SOG: failed to decode WebP image: %s", name);
        return false;
    }
    out->pixels = rgba;
    out->width = w;
    out->height = h;
    return true;
}

bool gaussian_load_sog(const char* path, GaussianScene* scene) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) {
        LOG(ERROR|GAUSSIAN|IO, "SOG: failed to open zip archive: %s", path);
        return false;
    }

    mz_uint zip_file_count = mz_zip_reader_get_num_files(&zip);
    SogArchiveFile* files = (SogArchiveFile*)calloc(zip_file_count, sizeof(SogArchiveFile));
    if (!files) {
        mz_zip_reader_end(&zip);
        return false;
    }
    int file_count = 0;

    bool ok = true;
    for (mz_uint i = 0; i < zip_file_count; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            LOG(ERROR|GAUSSIAN|IO, "SOG: failed to stat archive entry %u", i);
            ok = false;
            break;
        }
        if (stat.m_is_directory) {
            continue;
        }

        snprintf(files[file_count].name, sizeof(files[file_count].name), "%s", stat.m_filename);
        files[file_count].data = mz_zip_reader_extract_to_heap(&zip, i, &files[file_count].size, 0);
        if (!files[file_count].data) {
            LOG(ERROR|GAUSSIAN|IO, "SOG: failed to extract file: %s", files[file_count].name);
            ok = false;
            break;
        }
        file_count++;
    }

    mz_zip_reader_end(&zip);
    if (!ok) {
        free_sog_archive_files(files, file_count);
        free(files);
        return false;
    }

    LOG(INFO|GAUSSIAN|LOAD, "SOG: extracted bundled archive files from %s", path);
    for (int i = 0; i < file_count; i++) {
        if (files[i].data) LOG(INFO|GAUSSIAN|LOAD, "SOG:   %s (%zu bytes)", files[i].name, files[i].size);
    }

    SogMeta meta;
    SogArchiveFile* meta_file = find_sog_archive_file(files, file_count, "meta.json");
    if (!meta_file || !meta_file->data) {
        LOG(ERROR|GAUSSIAN|PARSE, "SOG: archive missing required file: meta.json");
        free_sog_archive_files(files, file_count);
        free(files);
        return false;
    }
    bool parsed_meta = false;
    // parse sog meta
    {
    meta = {};
    bool has_version = false, has_count = false, has_means = false;
    bool has_scales = false, has_quats = false, has_sh0 = false;

    sj_Reader r = sj_reader((const char*)meta_file->data, meta_file->size);
    sj_Value root = sj_read(&r);
    if (!sjp_expect_object(&r, root)) goto parse_sog_meta_fail;

    sj_Value key, val;
    while (sj_iter_object(&r, root, &key, &val)) {
        if (key.type != SJ_STRING) { sjp_set_error(&r, "expected string"); goto parse_sog_meta_fail; }
        if (sjp_eq(key, "version")) {
            if (!sjp_parse_int(&r, val, &meta.version)) goto parse_sog_meta_fail;
            has_version = true;
        } else if (sjp_eq(key, "count")) {
            int count = 0;
            if (!sjp_parse_int(&r, val, &count)) goto parse_sog_meta_fail;
            if (count <= 0) { sjp_set_error(&r, "SOG count out of range"); goto parse_sog_meta_fail; }
            meta.count = (uint32_t)count;
            has_count = true;
        } else if (sjp_eq(key, "antialias")) {
            if (!sjp_parse_bool(&r, val, &meta.antialias)) goto parse_sog_meta_fail;
            meta.has_antialias = true;
        } else if (sjp_eq(key, "means")) {
            // parse sog means
            {
                bool has_mins = false, has_maxs = false, has_files = false;
                if (!sjp_expect_object(&r, val)) goto parse_sog_meta_fail;
                sj_Value means_key, means_val;
                while (sj_iter_object(&r, val, &means_key, &means_val)) {
                    if (means_key.type != SJ_STRING) { sjp_set_error(&r, "expected string"); goto parse_sog_meta_fail; }
                    if (sjp_eq(means_key, "mins")) {
                        if (!sjp_parse_float_array(&r, means_val, meta.means_mins, 3)) goto parse_sog_meta_fail;
                        has_mins = true;
                    } else if (sjp_eq(means_key, "maxs")) {
                        if (!sjp_parse_float_array(&r, means_val, meta.means_maxs, 3)) goto parse_sog_meta_fail;
                        has_maxs = true;
                    } else if (sjp_eq(means_key, "files")) {
                        if (!sjp_parse_string_array(&r, means_val, &meta.means_files[0][0], 128, 2)) goto parse_sog_meta_fail;
                        has_files = true;
                    }
                }
                if (r.error) goto parse_sog_meta_fail;
                if (!has_mins || !has_maxs || !has_files) { sjp_set_error(&r, "SOG means missing required field"); goto parse_sog_meta_fail; }
            }
            has_means = true;
        } else if (sjp_eq(key, "scales")) {
            if (!parse_sog_codebook_file(&r, val, meta.scales_codebook, meta.scales_file, "scales")) goto parse_sog_meta_fail;
            has_scales = true;
        } else if (sjp_eq(key, "quats")) {
            // parse sog quats
            {
                bool has_files = false;
                char files[1][128] = {};
                if (!sjp_expect_object(&r, val)) goto parse_sog_meta_fail;
                sj_Value quats_key, quats_val;
                while (sj_iter_object(&r, val, &quats_key, &quats_val)) {
                    if (quats_key.type != SJ_STRING) { sjp_set_error(&r, "expected string"); goto parse_sog_meta_fail; }
                    if (sjp_eq(quats_key, "files")) {
                        if (!sjp_parse_string_array(&r, quats_val, &files[0][0], 128, 1)) goto parse_sog_meta_fail;
                        snprintf(meta.quats_file, sizeof(meta.quats_file), "%s", files[0]);
                        has_files = true;
                    }
                }
                if (r.error) goto parse_sog_meta_fail;
                if (!has_files) { sjp_set_error(&r, "SOG quats missing files"); goto parse_sog_meta_fail; }
            }
            has_quats = true;
        } else if (sjp_eq(key, "sh0")) {
            if (!parse_sog_codebook_file(&r, val, meta.sh0_codebook, meta.sh0_file, "sh0")) goto parse_sog_meta_fail;
            has_sh0 = true;
        } else if (sjp_eq(key, "shN")) {
            // parse sog shN
            {
                bool has_shN_count = false, has_bands = false, has_codebook = false, has_files = false;
                if (!sjp_expect_object(&r, val)) goto parse_sog_meta_fail;
                sj_Value shN_key, shN_val;
                while (sj_iter_object(&r, val, &shN_key, &shN_val)) {
                    if (shN_key.type != SJ_STRING) { sjp_set_error(&r, "expected string"); goto parse_sog_meta_fail; }
                    if (sjp_eq(shN_key, "count")) {
                        if (!sjp_parse_int(&r, shN_val, &meta.shN_count)) goto parse_sog_meta_fail;
                        has_shN_count = true;
                    } else if (sjp_eq(shN_key, "bands")) {
                        if (!sjp_parse_int(&r, shN_val, &meta.shN_bands)) goto parse_sog_meta_fail;
                        has_bands = true;
                    } else if (sjp_eq(shN_key, "codebook")) {
                        if (!sjp_parse_float_array(&r, shN_val, meta.shN_codebook, 256)) goto parse_sog_meta_fail;
                        has_codebook = true;
                    } else if (sjp_eq(shN_key, "files")) {
                        if (!sjp_parse_string_array(&r, shN_val, &meta.shN_files[0][0], 128, 2)) goto parse_sog_meta_fail;
                        has_files = true;
                    }
                }
                if (r.error) goto parse_sog_meta_fail;
                if (!has_shN_count || !has_bands || !has_codebook || !has_files) { sjp_set_error(&r, "SOG shN missing required field"); goto parse_sog_meta_fail; }
                if (meta.shN_count <= 0 || meta.shN_count > 65536) { sjp_set_error(&r, "SOG shN count out of range"); goto parse_sog_meta_fail; }
                if (meta.shN_bands < 1 || meta.shN_bands > 3) { sjp_set_error(&r, "SOG shN bands out of range"); goto parse_sog_meta_fail; }
                meta.has_shN = true;
            }
        }
    }
    if (r.error) goto parse_sog_meta_fail;

    if (!has_version || !has_count || !has_means || !has_scales || !has_quats || !has_sh0) {
        sjp_set_error(&r, "SOG meta missing required top-level field");
        goto parse_sog_meta_fail;
    }
    if (meta.version != 2) {
        sjp_set_error(&r, "unsupported SOG version");
        goto parse_sog_meta_fail;
    }
    parsed_meta = true;
    goto parse_sog_meta_done;

parse_sog_meta_fail:
        LOG(ERROR|GAUSSIAN|PARSE, "SOG: meta.json parse error at byte %d: %s", sjp_error_offset(&r), r.error ? r.error : "unknown error");
parse_sog_meta_done:
        ;
    }
    if (!parsed_meta) {
        free_sog_archive_files(files, file_count);
        free(files);
        return false;
    }
    bool meta_files_valid = true;
    // validate sog meta files
    {
        const char* required[] = {
            meta.means_files[0], meta.means_files[1], meta.scales_file,
            meta.quats_file, meta.sh0_file,
        };
        for (int i = 0; i < (int)(sizeof(required) / sizeof(required[0])); i++) {
            SogArchiveFile* f = find_sog_archive_file(files, file_count, required[i]);
            if (!f || !f->data) {
                LOG(ERROR|GAUSSIAN|PARSE, "SOG: archive missing file referenced by meta.json: %s", required[i]);
                meta_files_valid = false;
                break;
            }
        }
        if (meta_files_valid && meta.has_shN) {
            for (int i = 0; i < 2; i++) {
                SogArchiveFile* f = find_sog_archive_file(files, file_count, meta.shN_files[i]);
                if (!f || !f->data) {
                    LOG(ERROR|GAUSSIAN|PARSE, "SOG: archive missing file referenced by meta.json: %s", meta.shN_files[i]);
                    meta_files_valid = false;
                    break;
                }
            }
        }
    }
    if (!meta_files_valid) {
        free_sog_archive_files(files, file_count);
        free(files);
        return false;
    }

    if (meta.has_shN) {
        LOG(INFO|GAUSSIAN|PARSE, "SOG: meta version %d, %u gaussians, shN present (bands=%d, count=%d)",
            meta.version, meta.count, meta.shN_bands, meta.shN_count);
    } else {
        LOG(INFO|GAUSSIAN|PARSE, "SOG: meta version %d, %u gaussians, shN absent", meta.version, meta.count);
    }

    bool decoded_scene = false;
    // decode sog sh0 scene
    {
        const SogMeta* sog_meta = &meta;
    SogImage means_l = {}, means_u = {}, scales = {}, quats = {}, sh0 = {};
    bool ok = false;
    bool base_images_valid = true;
    bool decoded_shN = true;
    bool scene_storage_allocated = true;

    if (!load_sog_image(files, file_count, sog_meta->means_files[0], &means_l)) goto done;
    if (!load_sog_image(files, file_count, sog_meta->means_files[1], &means_u)) goto done;
    if (!load_sog_image(files, file_count, sog_meta->scales_file, &scales)) goto done;
    if (!load_sog_image(files, file_count, sog_meta->quats_file, &quats)) goto done;
    if (!load_sog_image(files, file_count, sog_meta->sh0_file, &sh0)) goto done;

    // validate sog base images
    {
        int w = means_l.width;
        int h = means_l.height;
        if (means_u.width != w || means_u.height != h ||
            scales.width != w || scales.height != h ||
            quats.width != w || quats.height != h ||
            sh0.width != w || sh0.height != h) {
            LOG(ERROR|GAUSSIAN|PARSE, "SOG: per-gaussian image dimensions do not match");
            base_images_valid = false;
        }
        if (base_images_valid && (uint64_t)sog_meta->count > (uint64_t)w * (uint64_t)h) {
            LOG(ERROR|GAUSSIAN|PARSE, "SOG: gaussian count exceeds image capacity");
            base_images_valid = false;
        }
    }
    if (!base_images_valid) goto done;
    // alloc scene storage
    {
        scene->gaussian_count = sog_meta->count;
        scene->gaussians = (Gaussian*)calloc(sog_meta->count, sizeof(Gaussian));
        scene->visible_indices  = (uint32_t*)SDL_malloc(sog_meta->count * sizeof(uint32_t));
        scene->visible_depths   = (float*)SDL_malloc(sog_meta->count * sizeof(float));
        scene->sorted_indices   = (uint32_t*)SDL_malloc(sog_meta->count * sizeof(uint32_t));
        scene->scratch_indices  = (uint32_t*)SDL_malloc(sog_meta->count * sizeof(uint32_t));
        scene->scratch_keys     = (uint32_t*)SDL_malloc(sog_meta->count * sizeof(uint32_t));
        scene->scratch_keys2    = (uint32_t*)SDL_malloc(sog_meta->count * sizeof(uint32_t));
        scene->visible_count    = 0;

        if (!scene->gaussians || !scene->visible_indices || !scene->visible_depths ||
            !scene->sorted_indices || !scene->scratch_indices || !scene->scratch_keys ||
            !scene->scratch_keys2) {
            free_scene(scene);
            scene_storage_allocated = false;
        }
    }
    if (!scene_storage_allocated) goto done;

    for (uint32_t i = 0; i < sog_meta->count; i++) {
        const uint8_t* ml = means_l.pixels + (size_t)i * 4;
        const uint8_t* mu = means_u.pixels + (size_t)i * 4;
        const uint8_t* sc = scales.pixels  + (size_t)i * 4;
        const uint8_t* qt = quats.pixels   + (size_t)i * 4;
        const uint8_t* s0 = sh0.pixels     + (size_t)i * 4;
        Gaussian* g = &scene->gaussians[i];

        for (int axis = 0; axis < 3; axis++) {
            uint32_t q = ((uint32_t)mu[axis] << 8) | (uint32_t)ml[axis];
            // sog lerp
            float n = math_lerp(sog_meta->means_mins[axis], sog_meta->means_maxs[axis], (float)q / 65535.0f);
            // sog unlog
            {
                g->position[axis] = n < 0.0f ? -(expf(-n) - 1.0f) : (expf(n) - 1.0f);
            }
            // SOG v2 stores k-means labels for the original 3DGS log-scale
            // values. The renderer expects linear-space Gaussian axis stddevs,
            // matching the PLY loader's exp(scale_*) decode.
            g->scale[axis] = expf(sog_meta->scales_codebook[sc[axis]]);
            g->color[axis] = sog_meta->sh0_codebook[s0[axis]];
        }

        float a, b, c;
        // sog quat component
        {
            a = ((float)qt[0] / 255.0f - 0.5f) * 2.0f / sqrtf(2.0f);
            b = ((float)qt[1] / 255.0f - 0.5f) * 2.0f / sqrtf(2.0f);
            c = ((float)qt[2] / 255.0f - 0.5f) * 2.0f / sqrtf(2.0f);
        }
        int mode = (int)qt[3] - 252;
        if (mode < 0 || mode > 3) {
            LOG(ERROR|GAUSSIAN|PARSE, "SOG: invalid quaternion mode %u at gaussian %u", qt[3], i);
            free_scene(scene);
            goto done;
        }
        float omitted = sqrtf(fmaxf(0.0f, 1.0f - (a*a + b*b + c*c)));
        float qx, qy, qz, qw;
        switch (mode) {
            case 0: qx = omitted; qy = a;       qz = b;       qw = c;       break;
            case 1: qx = a;       qy = omitted; qz = b;       qw = c;       break;
            case 2: qx = a;       qy = b;       qz = omitted; qw = c;       break;
            default:qx = a;       qy = b;       qz = c;       qw = omitted; break;
        }
        // SOG's smallest-three decode yields the same component order that
        // splat-transform writes back to PLY as rot_0..rot_3. Keep that order
        // here so SOG scenes match their SOG->PLY roundtrips in this renderer.
        g->rotation[0] = qx;
        g->rotation[1] = qy;
        g->rotation[2] = qz;
        g->rotation[3] = qw;
        g->opacity = (float)s0[3] / 255.0f;
    }

    // decode sog shN
    {
        if (sog_meta->has_shN) {
            SogImage centroids = {}, labels = {};
            bool shN_ok = false;
            int coeff_count = 0;
            int expected_w = 0;
            int expected_h = 0;

            // sog sh coeff count
            {
                switch (sog_meta->shN_bands) {
                    case 1: coeff_count = 3; break;
                    case 2: coeff_count = 8; break;
                    case 3: coeff_count = 15; break;
                    default: coeff_count = 0; break;
                }
            }
            if (coeff_count <= 0) {
                LOG(ERROR|GAUSSIAN|PARSE, "SOG: unsupported shN band count %d", sog_meta->shN_bands);
                decoded_shN = false;
            } else {
                if (!load_sog_image(files, file_count, sog_meta->shN_files[0], &centroids)) goto decode_sog_shN_done;
                if (!load_sog_image(files, file_count, sog_meta->shN_files[1], &labels)) goto decode_sog_shN_done;

                if ((uint64_t)sog_meta->count > (uint64_t)labels.width * (uint64_t)labels.height) {
                    LOG(ERROR|GAUSSIAN|PARSE, "SOG: shN label image is too small for gaussian count");
                    goto decode_sog_shN_done;
                }

                expected_w = 64 * coeff_count;
                expected_h = (sog_meta->shN_count + 63) / 64;
                if (centroids.width < expected_w || centroids.height < expected_h) {
                    LOG(ERROR|GAUSSIAN|PARSE, "SOG: shN centroid image is too small (got %dx%d, need at least %dx%d)",
                        centroids.width, centroids.height, expected_w, expected_h);
                    goto decode_sog_shN_done;
                }

                for (uint32_t i = 0; i < sog_meta->count; i++) {
                    const uint8_t* label_px = labels.pixels + (size_t)i * 4;
                    uint32_t label = (uint32_t)label_px[0] | ((uint32_t)label_px[1] << 8);
                    if (label >= (uint32_t)sog_meta->shN_count) {
                        LOG(ERROR|GAUSSIAN|PARSE, "SOG: shN label %u out of range at gaussian %u", label, i);
                        goto decode_sog_shN_done;
                    }

                    Gaussian* g = &scene->gaussians[i];
                    for (int coeff = 0; coeff < coeff_count; coeff++) {
                        int u = (int)(label % 64) * coeff_count + coeff;
                        int v = (int)(label / 64);
                        const uint8_t* centroid_px = centroids.pixels + ((size_t)v * centroids.width + (size_t)u) * 4;
                        g->sh_rest[coeff * 3 + 0] = sog_meta->shN_codebook[centroid_px[0]];
                        g->sh_rest[coeff * 3 + 1] = sog_meta->shN_codebook[centroid_px[1]];
                        g->sh_rest[coeff * 3 + 2] = sog_meta->shN_codebook[centroid_px[2]];
                    }
                }

                shN_ok = true;

decode_sog_shN_done:
                free_sog_image(&centroids);
                free_sog_image(&labels);
                decoded_shN = shN_ok;
            }
        }
    }
    if (!decoded_shN) {
        free_scene(scene);
        goto done;
    }

    ok = true;

done:
    free_sog_image(&means_l);
    free_sog_image(&means_u);
    free_sog_image(&scales);
    free_sog_image(&quats);
    free_sog_image(&sh0);
    decoded_scene = ok;
    }
    if (!decoded_scene) {
        free_sog_archive_files(files, file_count);
        free(files);
        return false;
    }

    LOG(INFO|GAUSSIAN|LOAD, "SOG: decoded %u gaussians (SH degree %d)",
        meta.count, meta.has_shN ? meta.shN_bands : 0);

    free_sog_archive_files(files, file_count);
    free(files);
    return true;
}
