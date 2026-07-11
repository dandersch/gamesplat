// Experimental Gaussian Point Splatting prototype.
// This validates the compute-splat -> resolve -> stochastic display path with
// one point per projected Gaussian. It intentionally does not implement the
// final opacity-corrected point counts or 64-bit packed depth/color atomics.

@cs gps_clear_cs
struct GpsUIntData {
    uint count;
};

layout(binding = 0) uniform GpsClearUBO {
    int pixel_count;
};

layout(binding = 0) buffer GpsClearDepthKeys {
    GpsUIntData gps_depth_keys[];
};

layout(binding = 1) buffer GpsClearColors {
    GpsUIntData gps_colors[];
};

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(pixel_count)) return;
    gps_depth_keys[idx].count = 0xFFFFFFFFu;
    gps_colors[idx].count = 0u;
}
@end

@program gps_clear gps_clear_cs

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

layout(binding = 0) uniform GpsSplatUBO {
    vec2 viewport;
    int gaussian_count;
    float clip_z_01;
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

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

uint pack_rgb8(vec3 color) {
    vec3 c = clamp(color, vec3(0.0), vec3(1.0));
    uvec3 rgb = uvec3(round(c * 255.0));
    return rgb.r | (rgb.g << 8u) | (rgb.b << 16u) | 0xFF000000u;
}

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(gaussian_count)) return;

    uint splat_id = splat_ids[idx].count;
    if (splat_id == 0xFFFFFFFFu) return;

    GpsProjectedSplatData projected = projected_splats[splat_id];
    if (projected.color_opacity.a <= 0.0) return;

    ivec2 pixel = ivec2(floor(projected.center_radius.xy + vec2(0.5)));
    ivec2 extent = ivec2(viewport);
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= extent.x || pixel.y >= extent.y) return;

    float z = projected.conic_depth.w;
    z = mix(z * 0.5 + 0.5, z, clip_z_01);
    uint depth_key = uint(clamp(z, 0.0, 1.0) * 4294967294.0);
    uint pixel_index = uint(pixel.y * extent.x + pixel.x);
    uint old_key = atomicMin(gps_depth_keys[pixel_index].count, depth_key);
    if (depth_key < old_key) {
        gps_colors[pixel_index].count = pack_rgb8(projected.color_opacity.rgb);
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
    float resolve_pad0;
    float resolve_pad1;
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
    ivec2 extent = ivec2(viewport);
    uint idx = uint(pixel.y * extent.x + pixel.x);
    uint depth_key = gps_depth_keys[idx].count;
    if (depth_key == 0xFFFFFFFFu) {
        out_color = vec4(0.1, 0.1, 0.1, 0.0);
        out_depth = vec4(1.0, 0.0, 0.0, 0.0);
        return;
    }

    out_color = vec4(unpack_rgb8(gps_colors[idx].count), 1.0);
    out_depth = vec4(float(depth_key) / 4294967294.0, 0.0, 0.0, 0.0);
}
@end

@program gps_resolve gps_resolve_vs gps_resolve_fs
