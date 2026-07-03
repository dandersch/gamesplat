// 3D Gaussian splat rendering. A compute prepass projects each Gaussian once;
// the vertex shader only expands precomputed screen-space splats into quads.
//
// sokol-shdc input. Compiled into shaders/splat.glsl.h.

@vs splat_vs
struct ProjectedSplatData {
    vec4 color_opacity; // rgb color, a opacity
    vec4 center_radius; // xy raster-space center, zw raster-space radius
    vec4 conic_depth;   // xyz inverse covariance/conic, w ndc depth
};

struct SplatIdData {
    uint id;
};

layout(binding = 0) readonly buffer ProjectedSplatBuffer {
    ProjectedSplatData projected_splats[];
};

layout(binding = 1) readonly buffer SplatIdBuffer {
    SplatIdData splat_ids[];
};

layout(binding = 0) uniform SplatDrawUBO {
    vec2 viewport;
    vec2 jitter_px;
    float clip_y_sign;
};

out vec3  frag_color;
out float frag_opacity;
flat out float frag_alpha_power_cutoff;
out vec2  frag_center;
out vec3  frag_conic;
flat out uint frag_splat_id;

void main() {
    int quad_verts[6] = int[6](0, 1, 2, 0, 2, 3);
    vec2 corners[4] = vec2[4](
        vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, 1)
    );
    vec2 corner = corners[quad_verts[gl_VertexIndex % 6]];

    uint splat_id = splat_ids[gl_InstanceIndex].id;
    if (splat_id == 0xFFFFFFFFu) {
        frag_color = vec3(0.0);
        frag_opacity = 0.0;
        frag_alpha_power_cutoff = 0.0;
        frag_center = vec2(0.0);
        frag_conic = vec3(1.0, 0.0, 1.0);
        frag_splat_id = splat_id;
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    ProjectedSplatData projected = projected_splats[splat_id];
    vec2 raster_center_px = projected.center_radius.xy;
    vec2 radius = projected.center_radius.zw;
    vec2 pos_px = raster_center_px + corner * radius + jitter_px;

    vec2 ndc = vec2(
        2.0 * pos_px.x / viewport.x - 1.0,
        (2.0 * pos_px.y / viewport.y - 1.0) * clip_y_sign
    );
    gl_Position = vec4(ndc, projected.conic_depth.w, 1.0);

    frag_color = projected.color_opacity.rgb;
    frag_opacity = projected.color_opacity.a;
    frag_alpha_power_cutoff = log((1.0 / 255.0) / max(projected.color_opacity.a, 1.0e-8));
    frag_center = raster_center_px + jitter_px;
    frag_conic = projected.conic_depth.xyz;
    frag_splat_id = splat_id;
}
@end

@fs splat_fs
in vec3  frag_color;
in float frag_opacity;
flat in float frag_alpha_power_cutoff;
in vec2  frag_center;
in vec3  frag_conic;
flat in uint frag_splat_id;

out vec4 out_color;

void main() {
    vec2 d = gl_FragCoord.xy - frag_center;

    float power = -0.5 * (
        frag_conic.x * d.x * d.x +
        2.0 * frag_conic.y * d.x * d.y +
        frag_conic.z * d.y * d.y
    );

    if (power > 0.0) discard;
    if (power < frag_alpha_power_cutoff) discard;

    float alpha = min(frag_opacity * exp(power), 0.99);

    // Premultiplied alpha output
    out_color = vec4(frag_color * alpha, alpha);
}
@end

@fs splat_stochastic_fs
in vec3  frag_color;
in float frag_opacity;
flat in float frag_alpha_power_cutoff;
in vec2  frag_center;
in vec3  frag_conic;
flat in uint frag_splat_id;

layout(binding = 2) uniform StochasticUBO {
    float frame_seed;
};

out vec4 out_color;

float hash13(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2 d = gl_FragCoord.xy - frag_center;

    float power = -0.5 * (
        frag_conic.x * d.x * d.x +
        2.0 * frag_conic.y * d.x * d.y +
        frag_conic.z * d.y * d.y
    );

    if (power > 0.0) discard;
    if (power < frag_alpha_power_cutoff) discard;

    float alpha = min(frag_opacity * exp(power), 0.99);

    float r = hash13(vec3(gl_FragCoord.xy, float(frag_splat_id) + frame_seed));
    if (r >= alpha) discard;

    out_color = vec4(frag_color, 1.0);
}
@end

@program splat splat_vs splat_fs
@program splat_stochastic splat_vs splat_stochastic_fs
