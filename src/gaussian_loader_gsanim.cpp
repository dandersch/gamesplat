#include "gsanim.h"
#include "gaussian.h"
#include "miniz.h"
#include "sj.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace gsanim_detail {
constexpr size_t max_text = 1024 * 1024;
constexpr size_t max_deltas = 256 * 1024 * 1024;
constexpr uint32_t max_gaussians = 1000000;

// sj is a token reader, not a JSON grammar validator (it skips commas/colons).
// Validate syntax, UTF-8, nesting and escapes first, including ignored metadata.
struct JsonSyntax {
    const unsigned char* p;
    const unsigned char* end;
    void space() { while (p != end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    bool take(char c) { space(); if (p == end || *p != c) return false; ++p; return true; }
    bool string() {
        if (!take('"')) return false;
        while (p != end) {
            unsigned c = *p++;
            if (c == '"') return true;
            if (c < 32) return false;
            if (c == '\\') {
                if (p == end) return false;
                c = *p++;
                if (c == 'u') {
                    for (int i = 0; i < 4; ++i) {
                        if (p == end || !((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) return false;
                        ++p;
                    }
                } else if (!strchr("\"\\/bfnrt", c)) return false;
            } else if (c >= 128) {
                unsigned count = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
                unsigned value = c & (count == 3 ? 7 : count == 2 ? 15 : 31);
                if (c < 0xc2 || c > 0xf4) return false;
                for (unsigned i = 0; i < count; ++i) {
                    if (p == end || (*p & 0xc0) != 0x80) return false;
                    value = (value << 6) | (*p++ & 63);
                }
                if (value < (count == 3 ? 65536u : count == 2 ? 2048u : 128u) || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
            }
        }
        return false;
    }
    bool value(int depth = 0) {
        space();
        if (p == end || depth > 32) return false;
        if (*p == '"') return string();
        if (*p == '{' || *p == '[') {
            bool object = *p++ == '{';
            char close = object ? '}' : ']';
            if (take(close)) return true;
            do {
                if (object && (!string() || !take(':'))) return false;
                if (!value(depth + 1)) return false;
                if (take(close)) return true;
            } while (take(','));
            return false;
        }
        for (const char* word : {"true", "false", "null"}) {
            size_t n = strlen(word);
            if (size_t(end - p) >= n && !memcmp(p, word, n)) { p += n; return true; }
        }
        if (*p == '-') ++p;
        if (p == end) return false;
        if (*p == '0') ++p;
        else {
            if (*p < '1' || *p > '9') return false;
            while (p != end && *p >= '0' && *p <= '9') ++p;
        }
        if (p != end && *p == '.') {
            ++p; const unsigned char* start = p;
            while (p != end && *p >= '0' && *p <= '9') ++p;
            if (p == start) return false;
        }
        if (p != end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p != end && (*p == '-' || *p == '+')) ++p;
            const unsigned char* start = p;
            while (p != end && *p >= '0' && *p <= '9') ++p;
            if (p == start) return false;
        }
        return true;
    }
};

bool number(sj_Value v, double& out) {
    if (v.type != SJ_NUMBER || v.end - v.start > 64) return false;
    std::string text(v.start, v.end);
    char* end;
    out = strtod(text.c_str(), &end);
    return end == text.c_str() + text.size() && std::isfinite(out);
}
bool integer(sj_Value v, uint32_t& out, uint32_t max) {
    double n;
    if (!number(v, n) || n < 0 || n > max) return false;
    const char* start = v.start + (*v.start == '-' ? 1 : 0);
    for (const char* p = start; p != v.end; ++p) if (*p < '0' || *p > '9') return false;
    out = uint32_t(n);
    return true;
}

// Required identifiers are ASCII, but valid JSON can spell them with \u escapes.
// Syntax has already been checked; sjp_eq compares raw token bytes only.
bool identifier(sj_Value v, const char* expected) {
    if (v.type != SJ_STRING) return false;
    for (const char* p = v.start; p != v.end; ++p) {
        unsigned c = (unsigned char)*p;
        if (c == '\\') {
            if (++p == v.end || *p != 'u' || v.end-p < 5) return false;
            c = 0;
            for (int i = 0; i < 4; ++i) {
                unsigned h = (unsigned char)*++p;
                c = c*16 + (h <= '9' ? h-'0' : (h | 32)-'a'+10);
            }
        }
        if (!*expected || c != (unsigned char)*expected++) return false;
    }
    return *expected == 0;
}

// Decode names before comparing: escaped and literal UTF-8 spellings are equal.
bool decoded_string(sj_Value v, std::string& out) {
    if (v.type != SJ_STRING) return false;
    auto hex = [](const char* p) {
        unsigned n = 0;
        for (int i = 0; i < 4; ++i) {
            unsigned c = (unsigned char)p[i];
            n = n * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
        }
        return n;
    };
    for (const char* p = v.start; p != v.end;) {
        unsigned c = (unsigned char)*p++;
        if (c != '\\') { out += char(c); continue; }
        c = (unsigned char)*p++;
        if (c != 'u') {
            const char* escapes = "\"\\/bfnrt";
            out += "\"\\/\b\f\n\r\t"[strchr(escapes, c) - escapes];
            continue;
        }
        c = hex(p); p += 4;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (v.end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
            unsigned low = hex(p + 2); p += 6;
            if (low < 0xdc00 || low > 0xdfff) return false;
            c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
        } else if (c >= 0xdc00 && c <= 0xdfff) return false;
        if (c < 128) out += char(c);
        else {
            if (c >= 65536) out += char(0xf0 | (c >> 18));
            if (c >= 2048) out += char((c >= 65536 ? 0x80 : 0xe0) | ((c >> 12) & 63));
            out += char((c >= 2048 ? 0x80 : 0xc0) | ((c >> 6) & 63));
            out += char(0x80 | (c & 63));
        }
    }
    return true;
}

bool mode(sj_Value v, bool& linear) {
    linear = identifier(v, "linear");
    return linear || identifier(v, "step");
}

bool layers(sj_Reader& r, sj_Value array, GSAnim& a) {
    if (array.type != SJ_ARRAY) return false;
    sj_Value layer;
    while (sj_iter_array(&r, array, &layer)) {
        if (layer.type != SJ_OBJECT || a.layers.size() >= 256) return false;
        GSAnimLayer entry;
        unsigned fields = 0;
        sj_Value k, v;
        while (sj_iter_object(&r, layer, &k, &v)) {
            if (identifier(k, "name")) {
                if ((fields & 1) || !decoded_string(v, entry.name) || entry.name.empty()) return false;
                fields |= 1;
            } else if (identifier(k, "keys")) {
                if ((fields & 2) || v.type != SJ_ARRAY) return false;
                fields |= 2;
                sj_Value key;
                while (sj_iter_array(&r, v, &key)) {
                    if (key.type != SJ_OBJECT || entry.keys.size() >= 65536) return false;
                    GSAnimLayerKey item;
                    unsigned seen = 0;
                    sj_Value name, val;
                    while (sj_iter_object(&r, key, &name, &val)) {
                        unsigned bit = identifier(name, "time_seconds") ? 1 : identifier(name, "weight") ? 2 : identifier(name, "interpolation") ? 4 : 0;
                        if (!bit) continue;
                        if (seen & bit) return false;
                        seen |= bit;
                        if (bit == 1 && !number(val, item.time_seconds)) return false;
                        if (bit == 4 && !mode(val, item.linear)) return false;
                        if (bit == 2) {
                            if (val.type != SJ_ARRAY) return false;
                            sj_Value component; unsigned c = 0;
                            while (sj_iter_array(&r, val, &component)) {
                                double n;
                                if (c == 3 || !number(component, n) || n < 0 || !std::isfinite(float(n))) return false;
                                item.weight[c++] = float(n);
                            }
                            if (c != 3) return false;
                        }
                    }
                    if (seen != 7) return false;
                    entry.keys.push_back(item);
                }
            }
        }
        if (fields != 3 || entry.keys.empty()) return false;
        for (const auto& previous : a.layers) if (previous.name == entry.name) return false;
        a.layers.push_back(std::move(entry));
    }
    return !a.layers.empty();
}

bool display(sj_Reader& r, sj_Value object, GSAnim& a) {
    if (object.type != SJ_OBJECT) return false;
    const char* names[] = {"type", "encoding", "layout", "size", "log2_min", "log2_max"};
    unsigned seen = 0;
    sj_Value k, v;
    while (sj_iter_object(&r, object, &k, &v)) {
        int field = -1;
        for (int i = 0; i < 6; ++i) if (identifier(k, names[i])) field = i;
        if (field < 0) continue;
        if (seen & (1u << field)) return false;
        seen |= 1u << field;
        if (field < 3 && !identifier(v, field == 0 ? "lut3d_log2" : field == 1 ? "uint16_unorm_rgb" : "r_fastest")) return false;
        if (field == 3 && (!integer(v, a.lut_size, 256) || a.lut_size < 2)) return false;
        if (field >= 4) {
            double n;
            if (!number(v, n) || !std::isfinite(float(n))) return false;
            (field == 4 ? a.log2_min : a.log2_max) = float(n);
        }
    }
    return seen == 63 && a.log2_min < a.log2_max;
}

bool manifest(const std::vector<unsigned char>& bytes, GSAnim& a) {
    JsonSyntax syntax{bytes.data(), bytes.data() + bytes.size()};
    if (!syntax.value()) return false;
    syntax.space();
    if (syntax.p != syntax.end) return false;
    sj_Reader r = sj_reader((const char*)bytes.data(), bytes.size());
    sj_Value root = sj_read(&r), k, v;
    if (root.type != SJ_OBJECT) return false;
    // Resolve version before interpreting version-specific fields, irrespective
    // of JSON member order. New field names remain ignorable metadata in 0.1.
    bool version_seen = false;
    while (sj_iter_object(&r, root, &k, &v)) {
        if (!identifier(k, "version")) continue;
        if (version_seen) return false;
        version_seen = true;
        a.version = identifier(v, "0.1") ? 1 : identifier(v, "0.2") ? 2 : identifier(v, "0.3") ? 3 : 0;
    }
    if (r.error || !version_seen || !a.version) return false;
    r = sj_reader((const char*)bytes.data(), bytes.size());
    root = sj_read(&r);
    const char* names[] = {"format", "version", "gaussian_count", "state_count", "sh_degree", "coordinate_system", "color_encoding", "duration_seconds", "interpolation", "keys", "layers", "display_transform"};
    bool linear_color = false;
    unsigned seen = 0;
    while (sj_iter_object(&r, root, &k, &v)) {
        int field = -1;
        for (int i = 0; i < 12; ++i) if (identifier(k, names[i])) field = i;
        if (field < 0) continue;
        if (a.version != 3 && field >= 10) continue;
        if (a.version == 3 && (field == 3 || field == 8 || field == 9)) continue;
        if (seen & (1u << field)) return false;
        seen |= 1u << field;
        uint32_t degree;
        switch (field) {
        case 0: if (!identifier(v, "gsanim")) return false; break;
        case 1: break; // Already validated above.
        case 2: if (!integer(v, a.gaussian_count, max_gaussians) || !a.gaussian_count) return false; break;
        case 3: if (!integer(v, a.state_count, 256) || a.state_count < 2) return false; break;
        case 4: if (!integer(v, degree, 3) || degree != 3) return false; break;
        case 5: if (!identifier(v, "blender_world_z_up_meters")) return false; break;
        case 6:
            linear_color = identifier(v, "scene_linear_rec709");
            if (!linear_color && !identifier(v, "display_referred_agx_baked")) return false;
            break;
        case 7: if (!number(v, a.duration) || a.duration <= 0) return false; break;
        case 8:
            if (!identifier(v, "step") && !identifier(v, "linear")) return false;
            a.linear = identifier(v, "linear"); break;
        case 9: {
            if (v.type != SJ_ARRAY) return false;
            sj_Value key;
            while (sj_iter_array(&r, v, &key)) {
                if (key.type != SJ_OBJECT || a.keys.size() >= 65536) return false;
                GSAnimKey entry{};
                sj_Value name, val;
                unsigned fields = 0;
                while (sj_iter_object(&r, key, &name, &val)) {
                    if (identifier(name, "time_seconds")) {
                        if ((fields & 1) || !number(val, entry.time_seconds)) return false;
                        fields |= 1;
                    } else if (identifier(name, "state")) {
                        if ((fields & 2) || !integer(val, entry.state, 255)) return false;
                        fields |= 2;
                    } else if (a.version == 2 && identifier(name, "interpolation")) {
                        if (entry.has_interpolation) return false;
                        entry.has_interpolation = true;
                        if (!mode(val, entry.linear)) return false;
                    }
                }
                if (fields != 3) return false;
                a.keys.push_back(entry);
            }
            break;
        }
        case 10: if (!layers(r, v, a)) return false; break;
        case 11: if (!display(r, v, a)) return false; break;
        }
    }
    unsigned required = a.version == 3 ? 0xcf7 : 1023;
    if (r.error || (seen & required) != required || linear_color != (a.version == 3)) return false;
    if (a.version == 3) {
        for (const auto& layer : a.layers) {
            double previous = -1;
            if (layer.keys.front().time_seconds != 0) return false;
            for (const auto& key : layer.keys) {
                if (key.time_seconds <= previous || key.time_seconds >= a.duration) return false;
                previous = key.time_seconds;
            }
        }
        return true;
    }
    if (a.keys.empty() || a.keys.front().time_seconds != 0) return false;
    double previous = -1;
    for (const GSAnimKey& key : a.keys) {
        if (a.version == 2 && !key.has_interpolation) return false;
        if (key.state >= a.state_count || key.time_seconds <= previous || key.time_seconds >= a.duration) return false;
        previous = key.time_seconds;
    }
    return true;
}

float little_float(const unsigned char* p) {
    uint32_t bits = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    float value; memcpy(&value, &bits, 4); return value;
}

bool ply_header(const std::vector<unsigned char>& bytes, uint32_t count, size_t& offset) {
    offset = 0;
    auto line = [&](std::string& out) {
        size_t start = offset;
        while (offset < bytes.size() && offset < max_text && bytes[offset] != '\n') ++offset;
        if (offset == bytes.size() || offset == max_text) return false;
        out.assign((const char*)bytes.data() + start, offset++ - start);
        return out.find('\r') == std::string::npos;
    };
    std::string text;
    if (!line(text) || text != "ply" || !line(text) || text != "format binary_little_endian 1.0") return false;
    std::vector<std::string> expected = {"element vertex " + std::to_string(count)};
    for (const char* name : {"x", "y", "z", "nx", "ny", "nz"}) expected.push_back(std::string("property float ") + name);
    for (int i = 0; i < 3; ++i) expected.push_back("property float f_dc_" + std::to_string(i));
    for (int i = 0; i < 45; ++i) expected.push_back("property float f_rest_" + std::to_string(i));
    expected.push_back("property float opacity");
    for (int i = 0; i < 3; ++i) expected.push_back("property float scale_" + std::to_string(i));
    for (int i = 0; i < 4; ++i) expected.push_back("property float rot_" + std::to_string(i));
    expected.push_back("end_header");
    for (const std::string& wanted : expected) {
        do { if (!line(text)) return false; } while (text == "comment" || text.compare(0, 8, "comment ") == 0);
        if (text != wanted) return false;
    }
    return bytes.size() - offset == size_t(count) * 248;
}

struct Zip {
    mz_zip_archive zip{};
    ~Zip() { if (zip.m_pState) mz_zip_reader_end(&zip); }
};
struct SDLFree { void operator()(void* p) const { SDL_free(p); } };
struct FileClose { void operator()(FILE* p) const { fclose(p); } };
template<typename T> using Buffer = std::unique_ptr<T, SDLFree>;

bool load(const char* path, GaussianScene* scene) {
    if (!path || !scene || scene->gaussians || scene->animation || scene->visible_indices || scene->visible_depths || scene->sorted_indices || scene->scratch_indices || scene->scratch_keys || scene->scratch_keys2) return false;
    // Bound the central-directory allocation as well as decompressed payloads.
    std::unique_ptr<FILE, FileClose> file(fopen(path, "rb"));
    if (!file || fseek(file.get(), 0, SEEK_END)) return false;
    long length = ftell(file.get());
    if (length < 0 || length > 512 * 1024 * 1024 || fseek(file.get(), 0, SEEK_SET)) return false;
    Zip archive;
    if (!mz_zip_reader_init_cfile(&archive.zip, file.get(), length, 0) || mz_zip_reader_get_num_files(&archive.zip) != 4) return false;
    const char* names[] = {"manifest.json", "base.ply", "scales.bin", "deltas.bin", "layers.bin", "display_lut.bin"};
    mz_zip_archive_file_stat stats[6]{};
    unsigned seen = 0;
    for (unsigned i = 0; i < 4; ++i) {
        mz_zip_archive_file_stat s;
        if (!mz_zip_reader_file_stat(&archive.zip, i, &s) || s.m_is_directory || s.m_is_encrypted || !s.m_is_supported || (s.m_method != 0 && s.m_method != 8)) return false;
        int which = -1;
        for (int j = 0; j < 6; ++j) if (!strcmp(s.m_filename, names[j]) && mz_zip_reader_get_filename(&archive.zip, i, nullptr, 0) == strlen(names[j]) + 1) which = j;
        if (which < 0 || (seen & (1u << which))) return false;
        seen |= 1u << which; stats[which] = s;
    }
    auto extract = [&](int i, std::vector<unsigned char>& out) {
        out.resize(size_t(stats[i].m_uncomp_size));
        // miniz verifies the decompressed CRC, including stored entries.
        return mz_zip_reader_extract_to_mem(&archive.zip, stats[i].m_file_index, out.data(), out.size(), 0) != 0;
    };
    if (!stats[0].m_uncomp_size || stats[0].m_uncomp_size > max_text) return false;
    std::unique_ptr<GSAnim> a(new GSAnim);
    std::vector<unsigned char> bytes;
    if (!extract(0, bytes) || !manifest(bytes, *a)) return false;
    if (seen != (a->version == 3 ? 51u : 15u)) return false;
    uint64_t body_size = uint64_t(a->gaussian_count) * 248;
    if (stats[1].m_uncomp_size <= body_size || stats[1].m_uncomp_size > body_size + max_text) return false;
    if (a->version == 3) {
        uint64_t layer_size = uint64_t(a->layers.size()) * a->gaussian_count * 96;
        uint64_t lut_bytes = uint64_t(a->lut_size) * a->lut_size * a->lut_size * 6;
        if (layer_size > max_deltas || stats[4].m_uncomp_size != layer_size || stats[5].m_uncomp_size != lut_bytes) return false;
        for (int entry : {4, 5}) {
            if (!extract(entry, bytes)) return false;
            auto& out = entry == 4 ? a->layer_data : a->display_lut;
            out.resize(bytes.size() / 2);
            for (size_t j = 0; j < out.size(); ++j) {
                out[j] = uint16_t(bytes[j*2]) | uint16_t(bytes[j*2+1]) << 8;
                if (entry == 4 && (out[j] & 0x7c00) == 0x7c00) return false;
            }
        }
    } else {
        uint64_t delta_size = uint64_t(a->state_count - 1) * a->gaussian_count * 48;
        if (delta_size > max_deltas || stats[3].m_uncomp_size != delta_size || stats[2].m_uncomp_size != 192) return false;
        if (!extract(2, bytes)) return false;
        for (int j = 0; j < 48; ++j) {
            a->scales[j] = little_float(bytes.data() + j * 4);
            if (!std::isfinite(a->scales[j]) || a->scales[j] <= 0) return false;
        }
        a->deltas.resize(size_t(delta_size));
        if (!mz_zip_reader_extract_to_mem(&archive.zip, stats[3].m_file_index, a->deltas.data(), a->deltas.size(), 0)) return false;
        for (int8_t q : a->deltas) if (q == -128) return false;
    }
    if (!extract(1, bytes)) return false;
    size_t offset;
    if (!ply_header(bytes, a->gaussian_count, offset)) return false;
    Buffer<Gaussian> gaussians((Gaussian*)SDL_malloc(size_t(a->gaussian_count) * sizeof(Gaussian)));
    if (!gaussians) return false;
    for (uint32_t i = 0; i < a->gaussian_count; ++i) {
        float v[62];
        for (int j = 0; j < 62; ++j) {
            v[j] = little_float(bytes.data() + offset + size_t(i) * 248 + j * 4);
            if (!std::isfinite(v[j])) return false;
            if (a->version == 3 && j >= 6 && j < 54 && v[j] != 0) return false;
        }
        Gaussian& g = gaussians.get()[i];
        for (int j = 0; j < 3; ++j) {
            g.position[j] = v[j]; g.color[j] = v[6 + j]; g.scale[j] = expf(v[55 + j]);
            if (!std::isfinite(g.scale[j]) || g.scale[j] <= 0 || !std::isfinite(g.scale[j] * g.scale[j])) return false;
        }
        g.opacity = 1.0f / (1.0f + expf(-v[54]));
        float norm = sqrtf(v[58]*v[58] + v[59]*v[59] + v[60]*v[60] + v[61]*v[61]);
        if (!std::isfinite(norm) || norm <= 1e-8f) return false;
        for (int j = 0; j < 4; ++j) g.rotation[j] = v[58 + j] / norm;
        for (int k = 0; k < 15; ++k) for (int c = 0; c < 3; ++c) g.sh_rest[k*3+c] = v[9+c*15+k];
        // Finite inputs can still overflow when decoded. Validate every state.
        for (uint32_t s = 1; a->version != 3 && s < a->state_count; ++s) for (int j = 0; j < 48; ++j) {
            float delta = float(a->deltas[(size_t(s-1)*a->gaussian_count+i)*48+j]) * a->scales[j];
            if (!std::isfinite(delta) || !std::isfinite(v[6+j] + delta)) return false;
        }
    }
    Buffer<uint32_t> scratch[6];
    for (auto& p : scratch) {
        p.reset((uint32_t*)SDL_malloc(size_t(a->gaussian_count) * sizeof(uint32_t)));
        if (!p) return false;
    }
    scene->gaussian_count = a->gaussian_count;
    scene->gaussians = gaussians.release();
    scene->visible_indices = scratch[0].release();
    scene->visible_depths = (float*)scratch[1].release();
    scene->sorted_indices = scratch[2].release();
    scene->scratch_indices = scratch[3].release();
    scene->scratch_keys = scratch[4].release();
    scene->scratch_keys2 = scratch[5].release();
    scene->visible_count = 0;
    scene->animation = a.release();
    return true;
}
} // namespace gsanim_detail

bool gaussian_load_gsanim(const char* path, GaussianScene* scene) {
    try { return gsanim_detail::load(path, scene); }
    catch (const std::bad_alloc&) { return false; }
}

GSAnimSample gsanim_sample(const GSAnim& a, double seconds) {
    if (!std::isfinite(seconds) || !std::isfinite(a.duration) || a.duration <= 0 || a.keys.empty()) return {0, 0, 0};
    // fmod avoids overflow of seconds/duration for very large finite times.
    double u = fmod(seconds, a.duration);
    if (u < 0) u += a.duration;
    if (u >= a.duration) u = 0;
    auto next = std::upper_bound(a.keys.begin(), a.keys.end(), u,
        [](double t, const GSAnimKey& k) { return t < k.time_seconds; });
    size_t i = next == a.keys.begin() ? 0 : size_t(next - a.keys.begin() - 1);
    int state = int(a.keys[i].state);
    if (!(a.version == 2 ? a.keys[i].linear : a.linear) || a.keys.size() == 1) return {state, state, 0};
    size_t j = (i + 1) % a.keys.size();
    double end = j ? a.keys[j].time_seconds : a.duration;
    return {state, int(a.keys[j].state), float((u - a.keys[i].time_seconds) / (end - a.keys[i].time_seconds))};
}

std::vector<std::array<float, 3>> gsanim_layer_weights(const GSAnim& a, double seconds) {
    if (a.version != 3 || !std::isfinite(seconds) || !std::isfinite(a.duration) || a.duration <= 0) return {};
    double u = fmod(seconds, a.duration);
    if (u < 0) u += a.duration;
    if (u >= a.duration) u = 0;
    std::vector<std::array<float, 3>> out;
    out.reserve(a.layers.size());
    for (const auto& layer : a.layers) {
        if (layer.keys.empty()) return {};
        auto next = std::upper_bound(layer.keys.begin(), layer.keys.end(), u,
            [](double t, const GSAnimLayerKey& k) { return t < k.time_seconds; });
        size_t i = next == layer.keys.begin() ? 0 : size_t(next - layer.keys.begin() - 1);
        size_t j = (i + 1) % layer.keys.size();
        double end = j ? layer.keys[j].time_seconds : a.duration;
        double w = layer.keys[i].linear && layer.keys.size() > 1 ? (u - layer.keys[i].time_seconds) / (end - layer.keys[i].time_seconds) : 0;
        std::array<float, 3> weight;
        for (int c = 0; c < 3; ++c) weight[c] = float((1-w)*layer.keys[i].weight[c] + w*layer.keys[j].weight[c]);
        out.push_back(weight);
    }
    return out;
}
