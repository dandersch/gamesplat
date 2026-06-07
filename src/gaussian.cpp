#include "gaussian.h"
#include "maths.h"
#include "miniz.h"
#include "sj.h"
#include <webp/decode.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cctype>

// --- PLY Parser ---

static float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

static bool str_ends_with_ci(const char* s, const char* suffix) {
    size_t sl = strlen(s);
    size_t tl = strlen(suffix);
    if (tl > sl) return false;
    s += sl - tl;
    for (size_t i = 0; i < tl; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i])) return false;
    }
    return true;
}

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

static bool alloc_scene_storage(GaussianScene* scene, uint32_t count) {
    scene->gaussian_count = count;
    scene->gaussians = (Gaussian*)calloc(count, sizeof(Gaussian));
    scene->visible_indices  = (uint32_t*)malloc(count * sizeof(uint32_t));
    scene->visible_depths   = (float*)malloc(count * sizeof(float));
    scene->sorted_indices   = (uint32_t*)malloc(count * sizeof(uint32_t));
    scene->scratch_indices  = (uint32_t*)malloc(count * sizeof(uint32_t));
    scene->scratch_keys     = (uint32_t*)malloc(count * sizeof(uint32_t));
    scene->scratch_keys2    = (uint32_t*)malloc(count * sizeof(uint32_t));
    scene->visible_count    = 0;

    if (!scene->gaussians || !scene->visible_indices || !scene->visible_depths ||
        !scene->sorted_indices || !scene->scratch_indices || !scene->scratch_keys ||
        !scene->scratch_keys2) {
        free_scene(scene);
        return false;
    }
    return true;
}

static SogArchiveFile* find_sog_archive_file(SogArchiveFile* files, int count, const char* name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(files[i].name, name) == 0) return &files[i];
    }
    return NULL;
}

static bool parse_sog_float_array(sj_Reader* r, sj_Value arr, float* out, int expected) {
    if (!sjp_expect_array(r, arr)) return false;
    int count = 0;
    sj_Value val;
    while (sj_iter_array(r, arr, &val)) {
        if (count >= expected) {
            sjp_set_error(r, "array too long");
            return false;
        }
        if (!sjp_parse_float(r, val, &out[count])) return false;
        count++;
    }
    if (r->error) return false;
    if (count != expected) {
        sjp_set_error(r, "array length mismatch");
        return false;
    }
    return true;
}

static bool parse_sog_string_array(sj_Reader* r, sj_Value arr, char out[][128], int expected) {
    if (!sjp_expect_array(r, arr)) return false;
    int count = 0;
    sj_Value val;
    while (sj_iter_array(r, arr, &val)) {
        if (count >= expected) {
            sjp_set_error(r, "array too long");
            return false;
        }
        if (!sjp_copy_string(r, val, out[count], 128)) return false;
        count++;
    }
    if (r->error) return false;
    if (count != expected) {
        sjp_set_error(r, "array length mismatch");
        return false;
    }
    return true;
}

static bool parse_sog_means(sj_Reader* r, sj_Value obj, SogMeta* meta) {
    bool has_mins = false, has_maxs = false, has_files = false;
    if (!sjp_expect_object(r, obj)) return false;
    sj_Value key, val;
    while (sj_iter_object(r, obj, &key, &val)) {
        if (key.type != SJ_STRING) { sjp_set_error(r, "expected string"); return false; }
        if (sjp_eq(key, "mins")) {
            if (!parse_sog_float_array(r, val, meta->means_mins, 3)) return false;
            has_mins = true;
        } else if (sjp_eq(key, "maxs")) {
            if (!parse_sog_float_array(r, val, meta->means_maxs, 3)) return false;
            has_maxs = true;
        } else if (sjp_eq(key, "files")) {
            if (!parse_sog_string_array(r, val, meta->means_files, 2)) return false;
            has_files = true;
        }
    }
    if (r->error) return false;
    if (!has_mins || !has_maxs || !has_files) sjp_set_error(r, "SOG means missing required field");
    return !r->error;
}

static bool parse_sog_codebook_file(sj_Reader* r, sj_Value obj, float* codebook, char file[128], const char* label) {
    bool has_codebook = false, has_files = false;
    char files[1][128] = {};
    if (!sjp_expect_object(r, obj)) return false;
    sj_Value key, val;
    while (sj_iter_object(r, obj, &key, &val)) {
        if (key.type != SJ_STRING) { sjp_set_error(r, "expected string"); return false; }
        if (sjp_eq(key, "codebook")) {
            if (!parse_sog_float_array(r, val, codebook, 256)) return false;
            has_codebook = true;
        } else if (sjp_eq(key, "files")) {
            if (!parse_sog_string_array(r, val, files, 1)) return false;
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

static bool parse_sog_quats(sj_Reader* r, sj_Value obj, SogMeta* meta) {
    bool has_files = false;
    char files[1][128] = {};
    if (!sjp_expect_object(r, obj)) return false;
    sj_Value key, val;
    while (sj_iter_object(r, obj, &key, &val)) {
        if (key.type != SJ_STRING) { sjp_set_error(r, "expected string"); return false; }
        if (sjp_eq(key, "files")) {
            if (!parse_sog_string_array(r, val, files, 1)) return false;
            snprintf(meta->quats_file, sizeof(meta->quats_file), "%s", files[0]);
            has_files = true;
        }
    }
    if (r->error) return false;
    if (!has_files) sjp_set_error(r, "SOG quats missing files");
    return !r->error;
}

static bool parse_sog_shN(sj_Reader* r, sj_Value obj, SogMeta* meta) {
    bool has_count = false, has_bands = false, has_codebook = false, has_files = false;
    if (!sjp_expect_object(r, obj)) return false;
    sj_Value key, val;
    while (sj_iter_object(r, obj, &key, &val)) {
        if (key.type != SJ_STRING) { sjp_set_error(r, "expected string"); return false; }
        if (sjp_eq(key, "count")) {
            if (!sjp_parse_int(r, val, &meta->shN_count)) return false;
            has_count = true;
        } else if (sjp_eq(key, "bands")) {
            if (!sjp_parse_int(r, val, &meta->shN_bands)) return false;
            has_bands = true;
        } else if (sjp_eq(key, "codebook")) {
            if (!parse_sog_float_array(r, val, meta->shN_codebook, 256)) return false;
            has_codebook = true;
        } else if (sjp_eq(key, "files")) {
            if (!parse_sog_string_array(r, val, meta->shN_files, 2)) return false;
            has_files = true;
        }
    }
    if (r->error) return false;
    if (!has_count || !has_bands || !has_codebook || !has_files) sjp_set_error(r, "SOG shN missing required field");
    if (!r->error && (meta->shN_count <= 0 || meta->shN_count > 65536)) sjp_set_error(r, "SOG shN count out of range");
    if (!r->error && (meta->shN_bands < 1 || meta->shN_bands > 3)) sjp_set_error(r, "SOG shN bands out of range");
    if (!r->error) meta->has_shN = true;
    return !r->error;
}

static bool parse_sog_meta(const char* buf, size_t len, SogMeta* meta) {
    *meta = {};
    bool has_version = false, has_count = false, has_means = false;
    bool has_scales = false, has_quats = false, has_sh0 = false;

    sj_Reader r = sj_reader(buf, len);
    sj_Value root = sj_read(&r);
    if (!sjp_expect_object(&r, root)) goto fail;

    sj_Value key, val;
    while (sj_iter_object(&r, root, &key, &val)) {
        if (key.type != SJ_STRING) { sjp_set_error(&r, "expected string"); goto fail; }
        if (sjp_eq(key, "version")) {
            if (!sjp_parse_int(&r, val, &meta->version)) goto fail;
            has_version = true;
        } else if (sjp_eq(key, "count")) {
            int count = 0;
            if (!sjp_parse_int(&r, val, &count)) goto fail;
            if (count <= 0) { sjp_set_error(&r, "SOG count out of range"); goto fail; }
            meta->count = (uint32_t)count;
            has_count = true;
        } else if (sjp_eq(key, "antialias")) {
            if (!sjp_parse_bool(&r, val, &meta->antialias)) goto fail;
            meta->has_antialias = true;
        } else if (sjp_eq(key, "means")) {
            if (!parse_sog_means(&r, val, meta)) goto fail;
            has_means = true;
        } else if (sjp_eq(key, "scales")) {
            if (!parse_sog_codebook_file(&r, val, meta->scales_codebook, meta->scales_file, "scales")) goto fail;
            has_scales = true;
        } else if (sjp_eq(key, "quats")) {
            if (!parse_sog_quats(&r, val, meta)) goto fail;
            has_quats = true;
        } else if (sjp_eq(key, "sh0")) {
            if (!parse_sog_codebook_file(&r, val, meta->sh0_codebook, meta->sh0_file, "sh0")) goto fail;
            has_sh0 = true;
        } else if (sjp_eq(key, "shN")) {
            if (!parse_sog_shN(&r, val, meta)) goto fail;
        }
    }
    if (r.error) goto fail;

    if (!has_version || !has_count || !has_means || !has_scales || !has_quats || !has_sh0) {
        sjp_set_error(&r, "SOG meta missing required top-level field");
        goto fail;
    }
    if (meta->version != 2) {
        sjp_set_error(&r, "unsupported SOG version");
        goto fail;
    }
    return true;

fail:
    LOG(ERROR|GAUSSIAN|PARSE, "SOG: meta.json parse error at byte %d: %s", sjp_error_offset(&r), r.error ? r.error : "unknown error");
    return false;
}

static bool validate_sog_meta_files(SogArchiveFile* files, int file_count, const SogMeta* meta) {
    const char* required[] = {
        meta->means_files[0], meta->means_files[1], meta->scales_file,
        meta->quats_file, meta->sh0_file,
    };
    for (int i = 0; i < (int)(sizeof(required) / sizeof(required[0])); i++) {
        SogArchiveFile* f = find_sog_archive_file(files, file_count, required[i]);
        if (!f || !f->data) {
            LOG(ERROR|GAUSSIAN|PARSE, "SOG: archive missing file referenced by meta.json: %s", required[i]);
            return false;
        }
    }
    if (meta->has_shN) {
        for (int i = 0; i < 2; i++) {
            SogArchiveFile* f = find_sog_archive_file(files, file_count, meta->shN_files[i]);
            if (!f || !f->data) {
                LOG(ERROR|GAUSSIAN|PARSE, "SOG: archive missing file referenced by meta.json: %s", meta->shN_files[i]);
                return false;
            }
        }
    }
    return true;
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

static float sog_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static float sog_unlog(float n) {
    return n < 0.0f ? -(expf(-n) - 1.0f) : (expf(n) - 1.0f);
}

static float sog_quat_component(uint8_t c) {
    return ((float)c / 255.0f - 0.5f) * 2.0f / sqrtf(2.0f);
}

static bool validate_sog_base_images(const SogMeta* meta,
                                     const SogImage* means_l,
                                     const SogImage* means_u,
                                     const SogImage* scales,
                                     const SogImage* quats,
                                     const SogImage* sh0) {
    int w = means_l->width;
    int h = means_l->height;
    if (means_u->width != w || means_u->height != h ||
        scales->width != w || scales->height != h ||
        quats->width != w || quats->height != h ||
        sh0->width != w || sh0->height != h) {
        LOG(ERROR|GAUSSIAN|PARSE, "SOG: per-gaussian image dimensions do not match");
        return false;
    }
    if ((uint64_t)meta->count > (uint64_t)w * (uint64_t)h) {
        LOG(ERROR|GAUSSIAN|PARSE, "SOG: gaussian count exceeds image capacity");
        return false;
    }
    return true;
}

static int sog_sh_coeff_count(int bands) {
    switch (bands) {
        case 1: return 3;
        case 2: return 8;
        case 3: return 15;
        default: return 0;
    }
}

static bool decode_sog_shN(SogArchiveFile* files, int file_count, const SogMeta* meta, GaussianScene* scene) {
    if (!meta->has_shN) return true;

    SogImage centroids = {}, labels = {};
    bool ok = false;
    int coeff_count = sog_sh_coeff_count(meta->shN_bands);
    int expected_w = 0;
    int expected_h = 0;
    if (coeff_count <= 0) {
        LOG(ERROR|GAUSSIAN|PARSE, "SOG: unsupported shN band count %d", meta->shN_bands);
        return false;
    }

    if (!load_sog_image(files, file_count, meta->shN_files[0], &centroids)) goto done;
    if (!load_sog_image(files, file_count, meta->shN_files[1], &labels)) goto done;

    if ((uint64_t)meta->count > (uint64_t)labels.width * (uint64_t)labels.height) {
        LOG(ERROR|GAUSSIAN|PARSE, "SOG: shN label image is too small for gaussian count");
        goto done;
    }

    expected_w = 64 * coeff_count;
    expected_h = (meta->shN_count + 63) / 64;
    if (centroids.width < expected_w || centroids.height < expected_h) {
        LOG(ERROR|GAUSSIAN|PARSE, "SOG: shN centroid image is too small (got %dx%d, need at least %dx%d)",
            centroids.width, centroids.height, expected_w, expected_h);
        goto done;
    }

    for (uint32_t i = 0; i < meta->count; i++) {
        const uint8_t* label_px = labels.pixels + (size_t)i * 4;
        uint32_t label = (uint32_t)label_px[0] | ((uint32_t)label_px[1] << 8);
        if (label >= (uint32_t)meta->shN_count) {
            LOG(ERROR|GAUSSIAN|PARSE, "SOG: shN label %u out of range at gaussian %u", label, i);
            goto done;
        }

        Gaussian* g = &scene->gaussians[i];
        for (int coeff = 0; coeff < coeff_count; coeff++) {
            int u = (int)(label % 64) * coeff_count + coeff;
            int v = (int)(label / 64);
            const uint8_t* centroid_px = centroids.pixels + ((size_t)v * centroids.width + (size_t)u) * 4;
            g->sh_rest[coeff * 3 + 0] = meta->shN_codebook[centroid_px[0]];
            g->sh_rest[coeff * 3 + 1] = meta->shN_codebook[centroid_px[1]];
            g->sh_rest[coeff * 3 + 2] = meta->shN_codebook[centroid_px[2]];
        }
    }

    ok = true;

done:
    free_sog_image(&centroids);
    free_sog_image(&labels);
    return ok;
}

static bool decode_sog_sh0_scene(SogArchiveFile* files, int file_count, const SogMeta* meta, GaussianScene* scene) {
    SogImage means_l = {}, means_u = {}, scales = {}, quats = {}, sh0 = {};
    bool ok = false;

    if (!load_sog_image(files, file_count, meta->means_files[0], &means_l)) goto done;
    if (!load_sog_image(files, file_count, meta->means_files[1], &means_u)) goto done;
    if (!load_sog_image(files, file_count, meta->scales_file, &scales)) goto done;
    if (!load_sog_image(files, file_count, meta->quats_file, &quats)) goto done;
    if (!load_sog_image(files, file_count, meta->sh0_file, &sh0)) goto done;

    if (!validate_sog_base_images(meta, &means_l, &means_u, &scales, &quats, &sh0)) goto done;
    if (!alloc_scene_storage(scene, meta->count)) goto done;

    for (uint32_t i = 0; i < meta->count; i++) {
        const uint8_t* ml = means_l.pixels + (size_t)i * 4;
        const uint8_t* mu = means_u.pixels + (size_t)i * 4;
        const uint8_t* sc = scales.pixels  + (size_t)i * 4;
        const uint8_t* qt = quats.pixels   + (size_t)i * 4;
        const uint8_t* s0 = sh0.pixels     + (size_t)i * 4;
        Gaussian* g = &scene->gaussians[i];

        for (int axis = 0; axis < 3; axis++) {
            uint32_t q = ((uint32_t)mu[axis] << 8) | (uint32_t)ml[axis];
            float n = sog_lerp(meta->means_mins[axis], meta->means_maxs[axis], (float)q / 65535.0f);
            g->position[axis] = sog_unlog(n);
            // SOG v2 stores k-means labels for the original 3DGS log-scale
            // values. The renderer expects linear-space Gaussian axis stddevs,
            // matching the PLY loader's exp(scale_*) decode.
            g->scale[axis] = expf(meta->scales_codebook[sc[axis]]);
            g->color[axis] = meta->sh0_codebook[s0[axis]];
        }

        float a = sog_quat_component(qt[0]);
        float b = sog_quat_component(qt[1]);
        float c = sog_quat_component(qt[2]);
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

    if (!decode_sog_shN(files, file_count, meta, scene)) {
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
    return ok;
}

static bool load_sog(const char* path, GaussianScene* scene) {
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
    if (!parse_sog_meta((const char*)meta_file->data, meta_file->size, &meta)) {
        free_sog_archive_files(files, file_count);
        free(files);
        return false;
    }
    if (!validate_sog_meta_files(files, file_count, &meta)) {
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

    if (!decode_sog_sh0_scene(files, file_count, &meta, scene)) {
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

struct PlyProperty {
    char name[64];
    int  byte_size; // 4 for float/int, etc.
    int  offset;    // byte offset within vertex
};

bool load_ply(const char* path, GaussianScene* scene) {
    FILE* f = fopen(path, "rb");
    if (!f) { LOG(ERROR|GAUSSIAN|IO, "Failed to open %s", path); return false; }

    // Parse header
    char line[512];
    uint32_t vertex_count = 0;
    PlyProperty props[128];
    int prop_count = 0;
    int current_offset = 0;
    bool in_vertex = false;
    bool header_done = false;

    while (fgets(line, sizeof(line), f)) {
        // Remove trailing newline
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;

        if (strcmp(line, "end_header") == 0) { header_done = true; break; }

        if (strncmp(line, "format ", 7) == 0) {
            if (strstr(line, "binary_little_endian") == NULL) {
                LOG(ERROR|GAUSSIAN|PARSE, "Only binary_little_endian PLY supported");
                fclose(f); return false;
            }
        }

        if (strncmp(line, "element vertex ", 15) == 0) {
            vertex_count = (uint32_t)atoi(line + 15);
            in_vertex = true;
            continue;
        }
        if (strncmp(line, "element ", 8) == 0) {
            in_vertex = false;
            continue;
        }

        if (in_vertex && strncmp(line, "property ", 9) == 0) {
            // "property float x" or "property uchar red"
            char type[32], name[64];
            if (sscanf(line, "property %31s %63s", type, name) == 2) {
                int sz = 0;
                if (strcmp(type, "float") == 0 || strcmp(type, "float32") == 0) sz = 4;
                else if (strcmp(type, "double") == 0 || strcmp(type, "float64") == 0) sz = 8;
                else if (strcmp(type, "uchar") == 0 || strcmp(type, "uint8") == 0) sz = 1;
                else if (strcmp(type, "int") == 0 || strcmp(type, "int32") == 0) sz = 4;
                else if (strcmp(type, "uint") == 0 || strcmp(type, "uint32") == 0) sz = 4;
                else if (strcmp(type, "short") == 0 || strcmp(type, "int16") == 0) sz = 2;
                else if (strcmp(type, "ushort") == 0 || strcmp(type, "uint16") == 0) sz = 2;
                else sz = 4; // fallback

                if (prop_count < 128) {
                    snprintf(props[prop_count].name, 64, "%s", name);
                    props[prop_count].byte_size = sz;
                    props[prop_count].offset = current_offset;
                    prop_count++;
                }
                current_offset += sz;
            }
        }
    }

    if (!header_done || vertex_count == 0) {
        LOG(ERROR|GAUSSIAN|PARSE, "Invalid PLY header");
        fclose(f); return false;
    }

    int stride = current_offset;
    LOG(INFO|GAUSSIAN|PARSE, "PLY: %u vertices, stride %d bytes, %d properties", vertex_count, stride, prop_count);

    // Find property offsets
    auto find_prop = [&](const char* name) -> int {
        for (int i = 0; i < prop_count; i++)
            if (strcmp(props[i].name, name) == 0) return props[i].offset;
        return -1;
    };

    int off_x = find_prop("x"), off_y = find_prop("y"), off_z = find_prop("z");
    int off_s0 = find_prop("scale_0"), off_s1 = find_prop("scale_1"), off_s2 = find_prop("scale_2");
    int off_r0 = find_prop("rot_0"), off_r1 = find_prop("rot_1"), off_r2 = find_prop("rot_2"), off_r3 = find_prop("rot_3");
    int off_op = find_prop("opacity");
    int off_dc0 = find_prop("f_dc_0"), off_dc1 = find_prop("f_dc_1"), off_dc2 = find_prop("f_dc_2");

    // f_rest_0..f_rest_44 (3DGS PLY layout: 15 coeffs per channel, R then G then B).
    // We support up to SH degree 3 (45 rest coeffs). Lower-degree PLYs leave
    // the missing slots at 0 (no contribution).
    int off_rest[45];
    int rest_count = 0;
    for (int k = 0; k < 45; k++) {
        char name[32];
        snprintf(name, sizeof(name), "f_rest_%d", k);
        off_rest[k] = find_prop(name);
        if (off_rest[k] >= 0) rest_count++;
    }

    if (off_x < 0 || off_y < 0 || off_z < 0) {
        LOG(ERROR|GAUSSIAN|PARSE, "Missing position properties");
        fclose(f); return false;
    }

    // Read all vertex data
    uint8_t* raw = (uint8_t*)malloc((size_t)vertex_count * stride);
    if (!raw) { fclose(f); return false; }
    size_t read = fread(raw, stride, vertex_count, f);
    fclose(f);
    if (read != vertex_count) {
        LOG(ERROR|GAUSSIAN|IO, "Short read: got %zu of %u vertices", read, vertex_count);
        free(raw); return false;
    }

    // Allocate scene
    scene->gaussian_count = vertex_count;
    scene->gaussians = (Gaussian*)malloc(vertex_count * sizeof(Gaussian));

    for (uint32_t i = 0; i < vertex_count; i++) {
        uint8_t* v = raw + (size_t)i * stride;
        Gaussian* g = &scene->gaussians[i];

        // Position
        memcpy(&g->position[0], v + off_x, 4);
        memcpy(&g->position[1], v + off_y, 4);
        memcpy(&g->position[2], v + off_z, 4);

        // Scale (apply exp)
        if (off_s0 >= 0) {
            float s0, s1, s2;
            memcpy(&s0, v + off_s0, 4); memcpy(&s1, v + off_s1, 4); memcpy(&s2, v + off_s2, 4);
            g->scale[0] = expf(s0); g->scale[1] = expf(s1); g->scale[2] = expf(s2);
        } else {
            g->scale[0] = g->scale[1] = g->scale[2] = 0.01f;
        }

        // Rotation (normalize quaternion) - PLY order: rot_0=w, rot_1=x, rot_2=y, rot_3=z
        if (off_r0 >= 0) {
            float rw, rx, ry, rz;
            memcpy(&rw, v + off_r0, 4); memcpy(&rx, v + off_r1, 4);
            memcpy(&ry, v + off_r2, 4); memcpy(&rz, v + off_r3, 4);
            float len = sqrtf(rw*rw + rx*rx + ry*ry + rz*rz);
            if (len > 1e-8f) { rw /= len; rx /= len; ry /= len; rz /= len; }
            g->rotation[0] = rw; g->rotation[1] = rx; g->rotation[2] = ry; g->rotation[3] = rz;
        } else {
            g->rotation[0] = 1; g->rotation[1] = g->rotation[2] = g->rotation[3] = 0;
        }

        // Opacity (apply sigmoid)
        if (off_op >= 0) {
            float op;
            memcpy(&op, v + off_op, 4);
            g->opacity = sigmoid(op);
        } else {
            g->opacity = 1.0f;
        }

        // Color: store raw f_dc_0..2; the shader applies SH_C0, adds higher
        // SH bands, biases by +0.5 and clamps. (Storing raw values lets the
        // higher-degree contributions push the color in either direction
        // before the final clamp, which is what produces the saturated
        // view-dependent shading.)
        if (off_dc0 >= 0) {
            memcpy(&g->color[0], v + off_dc0, 4);
            memcpy(&g->color[1], v + off_dc1, 4);
            memcpy(&g->color[2], v + off_dc2, 4);
        } else {
            // Encode mid-grey: SH_C0 * dc + 0.5 = 0.5 → dc = 0
            g->color[0] = g->color[1] = g->color[2] = 0.0f;
        }

        // f_rest: PLY stores all R coeffs (k=0..14), then G (15..29), then B (30..44).
        // Repack into per-coefficient RGB triples for shader-friendly access:
        //   sh_rest[k*3 + 0] = R, sh_rest[k*3 + 1] = G, sh_rest[k*3 + 2] = B
        for (int k = 0; k < 15; k++) {
            float r = 0.0f, gg = 0.0f, b = 0.0f;
            if (off_rest[k]      >= 0) memcpy(&r,  v + off_rest[k],      4);
            if (off_rest[15 + k] >= 0) memcpy(&gg, v + off_rest[15 + k], 4);
            if (off_rest[30 + k] >= 0) memcpy(&b,  v + off_rest[30 + k], 4);
            g->sh_rest[k * 3 + 0] = r;
            g->sh_rest[k * 3 + 1] = gg;
            g->sh_rest[k * 3 + 2] = b;
        }
    }

    LOG(INFO|GAUSSIAN|PARSE, "PLY: found %d/45 f_rest_* coefficients (SH degree %s)",
        rest_count,
        rest_count >= 45 ? "3" : rest_count >= 24 ? "2" : rest_count >= 9 ? "1" : "0");

    free(raw);

    // Allocate scratch buffers
    scene->visible_indices  = (uint32_t*)malloc(vertex_count * sizeof(uint32_t));
    scene->visible_depths   = (float*)malloc(vertex_count * sizeof(float));
    scene->sorted_indices   = (uint32_t*)malloc(vertex_count * sizeof(uint32_t));
    scene->scratch_indices  = (uint32_t*)malloc(vertex_count * sizeof(uint32_t));
    scene->scratch_keys     = (uint32_t*)malloc(vertex_count * sizeof(uint32_t));
    scene->scratch_keys2    = (uint32_t*)malloc(vertex_count * sizeof(uint32_t));
    scene->visible_count    = 0;

    return true;
}

bool load_gaussian_scene(const char* path, GaussianScene* scene) {
    if (str_ends_with_ci(path, ".ply")) {
        return load_ply(path, scene);
    }
    if (str_ends_with_ci(path, ".sog")) {
        return load_sog(path, scene);
    }

    LOG(ERROR|GAUSSIAN|LOAD, "Unsupported gaussian scene format: %s", path);
    return false;
}

void free_scene(GaussianScene* scene) {
    free(scene->gaussians);
    free(scene->visible_indices);
    free(scene->visible_depths);
    free(scene->sorted_indices);
    free(scene->scratch_indices);
    free(scene->scratch_keys);
    free(scene->scratch_keys2);
    memset(scene, 0, sizeof(GaussianScene));
}

GpuGaussian* pack_gpu_gaussians(const GaussianScene* scene) {
    GpuGaussian* gpu = (GpuGaussian*)calloc(scene->gaussian_count, sizeof(GpuGaussian));
    for (uint32_t i = 0; i < scene->gaussian_count; i++) {
        const Gaussian* g = &scene->gaussians[i];
        float* d = gpu[i].data;
        // [0..3] pos.xyz, opacity
        d[0] = g->position[0]; d[1] = g->position[1]; d[2] = g->position[2];
        d[3] = g->opacity;
        // [4..7] scale.xyz, pad
        d[4] = g->scale[0]; d[5] = g->scale[1]; d[6] = g->scale[2];
        // [8..11] rotation w,x,y,z
        d[8] = g->rotation[0]; d[9] = g->rotation[1];
        d[10] = g->rotation[2]; d[11] = g->rotation[3];
        // [12..15] color.rgb (raw DC), pad
        d[12] = g->color[0]; d[13] = g->color[1]; d[14] = g->color[2];
        // [16..60] sh_rest (45 floats)
        for (int k = 0; k < GAUSSIAN_SH_REST_FLOATS; k++) {
            d[16 + k] = g->sh_rest[k];
        }
    }
    return gpu;
}

// --- Culling ---

void cull_gaussians(GaussianScene* scene, const float* view, const float* proj, float ortho_blend) {
    scene->visible_count = 0;

    for (uint32_t i = 0; i < scene->gaussian_count; i++) {
        float p_view[3];
        math_mat4_transform_point(view, scene->gaussians[i].position, p_view);

        // Near-plane cull: camera looks -Z, visible objects have z < 0
        if (p_view[2] > -0.2f) continue;

        // Frustum cull: project to NDC and check with margin
        // Lerp between perspective division and no division
        float inv_z = -1.0f / p_view[2];
        float ndc_x = (proj[0] * p_view[0]) * inv_z * (1.0f - ortho_blend) + proj[0] * p_view[0] * ortho_blend;
        float ndc_y = (proj[5] * p_view[1]) * inv_z * (1.0f - ortho_blend) + proj[5] * p_view[1] * ortho_blend;

        float abs_ndc_x = ndc_x < 0 ? -ndc_x : ndc_x;
        float abs_ndc_y = ndc_y < 0 ? -ndc_y : ndc_y;
        if (abs_ndc_x > 1.3f) continue;
        if (abs_ndc_y > 1.3f) continue;

        scene->visible_indices[scene->visible_count] = i;
        scene->visible_depths[scene->visible_count] = -p_view[2]; // positive, larger = farther
        scene->visible_count++;
    }
}

// --- Radix Sort ---

static uint32_t float_to_sortable(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    // If sign bit set, flip all bits; else flip only sign bit
    if (bits & 0x80000000u)
        return ~bits;
    else
        return bits ^ 0x80000000u;
}

void sort_gaussians(SortContext* ctx) {
    if (ctx->count == 0) return;

    // Convert depths to sortable keys
    uint32_t* keys_a = ctx->scratch_keys;
    uint32_t* keys_b = ctx->scratch_keys2;

    uint32_t* idx_a = ctx->sorted_indices;
    uint32_t* idx_b = ctx->scratch_indices;

    // Initialize
    for (uint32_t i = 0; i < ctx->count; i++) {
        keys_a[i] = float_to_sortable(ctx->depths[i]);
        idx_a[i] = ctx->input_indices[i];
    }

    // 4-pass 8-bit radix sort (LSB first)
    for (int pass = 0; pass < 4; pass++) {
        int shift = pass * 8;

        // Count
        uint32_t count[256];
        memset(count, 0, sizeof(count));
        for (uint32_t i = 0; i < ctx->count; i++) {
            uint8_t bucket = (keys_a[i] >> shift) & 0xFF;
            count[bucket]++;
        }

        // Prefix sum
        uint32_t total = 0;
        for (int b = 0; b < 256; b++) {
            uint32_t c = count[b];
            count[b] = total;
            total += c;
        }

        // Scatter
        for (uint32_t i = 0; i < ctx->count; i++) {
            uint8_t bucket = (keys_a[i] >> shift) & 0xFF;
            uint32_t dst = count[bucket]++;
            keys_b[dst] = keys_a[i];
            idx_b[dst] = idx_a[i];
        }

        // Swap
        uint32_t* tmp;
        tmp = keys_a; keys_a = keys_b; keys_b = tmp;
        tmp = idx_a; idx_a = idx_b; idx_b = tmp;
    }

    // After 4 passes, result is in keys_a/idx_a (ascending order)
    // We need back-to-front = descending depth, so reverse
    if (idx_a != ctx->sorted_indices) {
        // Result ended up in scratch; reverse-copy to sorted_indices
        for (uint32_t i = 0; i < ctx->count; i++) {
            ctx->sorted_indices[i] = idx_a[ctx->count - 1 - i];
        }
    } else {
        // Result is already in sorted_indices; reverse in place
        for (uint32_t i = 0; i < ctx->count / 2; i++) {
            uint32_t tmp_val = ctx->sorted_indices[i];
            ctx->sorted_indices[i] = ctx->sorted_indices[ctx->count - 1 - i];
            ctx->sorted_indices[ctx->count - 1 - i] = tmp_val;
        }
    }
}
