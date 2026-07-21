// Experimental Gaussian Point Splatting prototype.
// This validates the compute-splat -> resolve -> stochastic display path.
// It still uses a prototype global work budget and does not implement the
// final 64-bit packed depth/color atomic path.

@cs gps_clear_cs
struct GpsUIntData {
    uint count;
};

layout(binding = 0) uniform GpsClearUBO {
    int pixel_count;
    int pixels_per_row;
    float clear_pad0;
    float clear_pad1;
};

layout(binding = 0) buffer GpsClearDepthKeys {
    GpsUIntData gps_depth_keys[];
};

layout(binding = 1) buffer GpsClearColors {
    GpsUIntData gps_colors[];
};

layout(binding = 2) buffer GpsClearWorkCount {
    GpsUIntData work_count[];
};

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main() {
    uint idx = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * uint(pixels_per_row);
    if (idx >= uint(pixel_count)) return;
    if (idx == 0u) {
        work_count[0].count = 0u;
    }
    gps_depth_keys[idx].count = 0xFFFFFFFFu;
    gps_colors[idx].count = 0u;
}
@end

@program gps_clear gps_clear_cs

@cs gps_count_cs
struct GpsCountProjectedSplatData {
    vec4 color_opacity; // rgb color, a opacity
    vec4 center_radius; // xy raster-space center, zw raster-space radius
    vec4 conic_depth;   // xyz inverse covariance/conic, w ndc depth
    vec4 covariance_det; // xyz screen-space covariance (a,b,c), w determinant
};

struct GpsCountSplatIdData {
    uint count;
};

struct GpsCountUIntData {
    uint count;
};

layout(binding = 0) uniform GpsCountUBO {
    int gaussian_count;
    float supersample_factor;
    float count_pad0;
    float count_pad1;
};

layout(binding = 0) readonly buffer GpsCountProjectedSplatBuffer {
    GpsCountProjectedSplatData projected_splats[];
};

layout(binding = 1) readonly buffer GpsCountSplatIdBuffer {
    GpsCountSplatIdData splat_ids[];
};

layout(binding = 2) buffer GpsPointCounts {
    GpsCountUIntData point_counts[];
};

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

float dilog(float x) {
    float s = 1.0 - x;
    float y = (((((((-1.068681974 * x + 3.334685126) * x - 4.173996483) * x +
                    2.567860600) * x - 0.884150470) * x - 0.123550674) * x +
                    1.992765336) * x + 6.74195669e-05);
    y += (s > 0.0) ? (s * log(s)) : 0.0;
    return y;
}

float hash01(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return float(x) * (1.0 / 4294967296.0);
}

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(gaussian_count)) return;

    uint splat_id = splat_ids[idx].count;
    if (splat_id == 0xFFFFFFFFu) {
        point_counts[idx].count = 0u;
        return;
    }

    vec4 color_opacity = projected_splats[splat_id].color_opacity;
    float ss = max(supersample_factor, 1.0);
    vec4 covariance_det = projected_splats[splat_id].covariance_det;
    float cov_a = covariance_det.x + 0.3;
    float cov_b = covariance_det.y;
    float cov_c = covariance_det.z + 0.3;
    float det = cov_a * cov_c - cov_b * cov_b;
    float expected_points = ss * ss * 6.28318530718 * sqrt(max(det, 0.0)) *
        dilog(clamp(color_opacity.a, 0.0, 1.0));
    float points_per_work_item = max(ss * ss, 1.0);
    float scaled_work_items = max(expected_points / points_per_work_item, 0.0);
    uint work_item_count = uint(clamp(floor(scaled_work_items), 0.0, 4294967295.0));
    if (hash01(splat_id + 0x9e3779b9u) < fract(scaled_work_items) && work_item_count < 0xFFFFFFFFu) {
        work_item_count += 1u;
    }
    point_counts[idx].count = work_item_count;
}
@end

@program gps_count gps_count_cs

@cs gps_expand_cs
struct GpsExpandUIntData {
    uint count;
};

struct GpsWorkData {
    uint gaussian_id;
    uint sample_id;
};

layout(binding = 0) uniform GpsExpandUBO {
    int gaussian_count;
    int max_work_items;
    float expand_pad1;
    float expand_pad2;
};

layout(binding = 0) readonly buffer GpsExpandPointCounts {
    GpsExpandUIntData point_counts[];
};

layout(binding = 1) buffer GpsExpandWorkCount {
    GpsExpandUIntData work_count[];
};

layout(binding = 2) buffer GpsPointWork {
    GpsWorkData point_work[];
};

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(gaussian_count)) return;

    uint count = point_counts[idx].count;
    if (count == 0u) return;

    uint base = atomicAdd(work_count[0].count, count);
    for (uint i = 0u; i < count; ++i) {
        if (base + i >= uint(max_work_items)) break;
        point_work[base + i].gaussian_id = idx;
        point_work[base + i].sample_id = i;
    }
}
@end

@program gps_expand gps_expand_cs

@cs gps_splat_cs
struct GpsProjectedSplatData {
    vec4 color_opacity; // rgb color, a opacity
    vec4 center_radius; // xy raster-space center, zw raster-space radius
    vec4 conic_depth;   // xyz inverse covariance/conic, w ndc depth
    vec4 covariance_det; // xyz screen-space covariance (a,b,c), w determinant
};

struct GpsSplatIdData {
    uint count;
};

struct GpsSplatUIntData {
    uint count;
};

struct GpsSplatWorkData {
    uint gaussian_id;
    uint sample_id;
};

layout(binding = 0) uniform GpsSplatUBO {
    vec2 viewport;
    int work_count;
    float clip_z_01;
    float frame_seed;
    int work_items_per_row;
    float supersample_factor;
};

layout(binding = 0) readonly buffer GpsProjectedSplatBuffer {
    GpsProjectedSplatData projected_splats[];
};

layout(binding = 1) readonly buffer GpsSplatIdBuffer {
    GpsSplatIdData splat_ids[];
};

layout(binding = 2) buffer GpsDepthKeys {
    GpsSplatUIntData gps_depth_keys[];
};

layout(binding = 3) buffer GpsColors {
    GpsSplatUIntData gps_colors[];
};

layout(binding = 4) readonly buffer GpsSplatPointWork {
    GpsSplatWorkData point_work[];
};

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

float dilog_sample(float x) {
    float s = 1.0 - x;
    float y = (((((((-1.068681974 * x + 3.334685126) * x - 4.173996483) * x +
                    2.567860600) * x - 0.884150470) * x - 0.123550674) * x +
                    1.992765336) * x + 6.74195669e-05);
    y += (s > 0.0) ? (s * log(s)) : 0.0;
    return y;
}

float inv_dilog(float x) {
    return ((((((((((-0.322192871 * x + 2.562830719) * x - 8.669815892) * x +
                    16.243561492) * x - 18.394710417) * x + 12.891925084) * x -
                    5.501796628) * x + 1.361194673) * x - 0.417517201) * x +
                    1.008071981) * x - 6.39879974e-05);
}

void main() {
    uint work_idx = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * uint(work_items_per_row);
    if (work_idx >= uint(work_count)) return;
    uint idx = point_work[work_idx].gaussian_id;
    uint sample_idx = point_work[work_idx].sample_id;

    uint splat_id = splat_ids[idx].count;
    if (splat_id == 0xFFFFFFFFu) return;

    vec4 color_opacity = projected_splats[splat_id].color_opacity;
    if (color_opacity.a <= 0.0) return;

    vec3 color = color_opacity.rgb;
    vec2 mean = projected_splats[splat_id].center_radius.xy;
    vec4 covariance_det = projected_splats[splat_id].covariance_det;
    float ndc_depth = projected_splats[splat_id].conic_depth.w;
    float cov_a = max(covariance_det.x + 0.3, 1.0e-5);
    float cov_b = covariance_det.y;
    float cov_c = max(covariance_det.z + 0.3, 1.0e-5);
    float l00 = sqrt(cov_a);
    float l10 = cov_b / l00;
    float l11 = sqrt(max(cov_c - l10 * l10, 1.0e-5));

    ivec2 extent = ivec2(viewport);
    float z = ndc_depth;
    z = mix(z * 0.5 + 0.5, z, clip_z_01);
    uint depth_key = uint(clamp(z, 0.0, 1.0) * 4294967294.0);
    vec3 clamped_color = clamp(color, vec3(0.0), vec3(1.0));
    uvec3 rgb = uvec3(round(clamped_color * 255.0));
    uint packed_color = rgb.r | (rgb.g << 8u) | (rgb.b << 16u) | 0xFF000000u;

    uvec2 seed = uvec2(
        splat_id * 747796405u + uint(frame_seed) * 2891336453u,
        splat_id * 277803737u + 0x9E3779B9u
    );

    int points_per_work_item = max(int(round(supersample_factor * supersample_factor)), 1);
    float alpha = clamp(color_opacity.a, 1.0e-6, 1.0);
    float dilog_alpha = dilog_sample(alpha);
    for (int p = 0; p < points_per_work_item; ++p) {
        uvec2 point_seed = seed;
        point_seed.x += (sample_idx * uint(points_per_work_item) + uint(p)) * 374761393u;
        point_seed = 1664525u * point_seed + 1013904223u;
        point_seed.x += 1664525u * point_seed.y;
        point_seed.y += 1664525u * point_seed.x;
        point_seed ^= (point_seed >> 16u);
        point_seed.x += 1664525u * point_seed.y;
        point_seed.y += 1664525u * point_seed.x;
        point_seed ^= (point_seed >> 16u);
        vec2 rands = vec2(point_seed) * 2.32830643654e-10;
        float arg = inv_dilog((1.0 - rands.x) * dilog_alpha) / alpha;
        arg = clamp(arg, 1.0e-7, 1.0);
        float radius = sqrt(-2.0 * log(arg));
        float azimuth = 6.28318530718 * rands.y;
        vec2 local = vec2(cos(azimuth), sin(azimuth)) * radius;
        vec2 point = (mean + vec2(l00 * local.x, l10 * local.x + l11 * local.y)) * max(supersample_factor, 1.0);
        ivec2 pixel = ivec2(floor(point + vec2(0.5)));
        if (pixel.x >= 0 && pixel.y >= 0 && pixel.x < extent.x && pixel.y < extent.y) {
            uint pixel_index = uint(pixel.y * extent.x + pixel.x);
            uint old_key = atomicMin(gps_depth_keys[pixel_index].count, depth_key);
            if (depth_key < old_key) {
                gps_colors[pixel_index].count = packed_color;
            }
        }
    }
}
@end

@program gps_splat gps_splat_cs

@vs gps_resolve_vs
out vec2 uv;

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs gps_resolve_fs
struct GpsResolveUIntData {
    uint count;
};

layout(binding = 0) uniform GpsResolveUBO {
    vec2 viewport;
    vec2 gps_viewport;
    int supersample_factor;
    float resolve_pad0;
    float resolve_pad1;
    float resolve_pad2;
};

layout(binding = 0) readonly buffer GpsResolveDepthKeys {
    GpsResolveUIntData gps_depth_keys[];
};

layout(binding = 1) readonly buffer GpsResolveColors {
    GpsResolveUIntData gps_colors[];
};

in vec2 uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_depth;

vec3 unpack_rgb8(uint packed) {
    return vec3(
        float(packed & 255u),
        float((packed >> 8u) & 255u),
        float((packed >> 16u) & 255u)
    ) / 255.0;
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 gps_extent = ivec2(gps_viewport);
    int ss = clamp(supersample_factor, 1, 4);
    ivec2 base = pixel * ss;

    vec3 premul_color_sum = vec3(0.0);
    float coverage_count = 0.0;
    uint min_depth_key = 0xFFFFFFFFu;

    for (int y = 0; y < 4; ++y) {
        if (y >= ss) break;
        for (int x = 0; x < 4; ++x) {
            if (x >= ss) break;
            ivec2 sp = base + ivec2(x, y);
            if (sp.x >= gps_extent.x || sp.y >= gps_extent.y) continue;
            uint idx = uint(sp.y * gps_extent.x + sp.x);
            uint depth_key = gps_depth_keys[idx].count;
            if (depth_key != 0xFFFFFFFFu) {
                premul_color_sum += unpack_rgb8(gps_colors[idx].count);
                coverage_count += 1.0;
                min_depth_key = min(min_depth_key, depth_key);
            }
        }
    }

    if (coverage_count <= 0.0) {
        out_color = vec4(0.1, 0.1, 0.1, 0.0);
        out_depth = vec4(1.0, 0.0, 0.0, 0.0);
        return;
    }

    float subpixel_count = float(ss * ss);
    float coverage = coverage_count / subpixel_count;
    out_color = vec4(premul_color_sum / subpixel_count, coverage);
    out_depth = vec4(float(min_depth_key) / 4294967294.0, 0.0, 0.0, 0.0);
}
@end

@program gps_resolve gps_resolve_vs gps_resolve_fs
