// RGBA8 temporal accumulation and display for stochastic splats.
// Uses a fullscreen triangle and ping-pong color attachments.

@vs accum_vs
out vec2 uv;

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs accum_fs
@image_sample_type sample_tex unfilterable_float
layout(binding = 0) uniform texture2D sample_tex;
@sampler_type sample_smp nonfiltering
layout(binding = 0) uniform sampler   sample_smp;
@image_sample_type history_tex unfilterable_float
layout(binding = 1) uniform texture2D history_tex;
@sampler_type history_smp nonfiltering
layout(binding = 1) uniform sampler   history_smp;

layout(binding = 0) uniform AccumUBO {
    float sample_count;
    float accum_pad0;
    float accum_pad1;
    float accum_pad2;
};

in vec2 uv;
out vec4 out_color;

void main() {
    vec4 sample_color = texture(sampler2D(sample_tex, sample_smp), uv);
    vec4 history_color = texture(sampler2D(history_tex, history_smp), uv);
    float n = max(sample_count, 1.0);
    out_color = history_color + (sample_color - history_color) / n;
}
@end

@fs taa_accum_fs
@image_sample_type current_color_tex unfilterable_float
layout(binding = 0) uniform texture2D current_color_tex;
@sampler_type current_color_smp nonfiltering
layout(binding = 0) uniform sampler   current_color_smp;
@image_sample_type current_depth_tex unfilterable_float
layout(binding = 1) uniform texture2D current_depth_tex;
@sampler_type current_depth_smp nonfiltering
layout(binding = 1) uniform sampler   current_depth_smp;
@image_sample_type history_color_tex unfilterable_float
layout(binding = 2) uniform texture2D history_color_tex;
@sampler_type history_color_smp nonfiltering
layout(binding = 2) uniform sampler   history_color_smp;
@image_sample_type history_xyz_tex unfilterable_float
layout(binding = 3) uniform texture2D history_xyz_tex;
@sampler_type history_xyz_smp nonfiltering
layout(binding = 3) uniform sampler   history_xyz_smp;

layout(binding = 0) uniform TaaAccumUBO {
    mat4 inv_view;
    mat4 prev_view;
    vec4 viewport_orthographic; // xy viewport, z current orthographic, w previous orthographic
    vec4 focal_clip;            // x current persp focal, y current ortho focal, z clip_y_sign, w clip_z_01
    vec4 prev_focal_clip;       // x previous persp focal, y previous ortho focal, z previous clip_y_sign, w unused
    vec4 proj_z;                // x proj[2][2], y proj[3][2], zw unused
    float history_valid;
    float view_changed;
    float taa_pad1;
    float taa_pad2;
};

in vec2 uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_xyz;

vec3 reconstruct_view_pos(vec2 in_uv, float depth) {
    float z_ndc = depth * 2.0 - 1.0;
    float view_z = -proj_z.y / (z_ndc + proj_z.x);

    vec2 px = in_uv * viewport_orthographic.xy;
    float unflip = (1.0 - focal_clip.z) * 0.5;
    px.y = mix(px.y, viewport_orthographic.y - px.y, unflip);

    vec2 centered = px - viewport_orthographic.xy * 0.5;
    float t = viewport_orthographic.z;
    float denom = mix(focal_clip.x / view_z, -focal_clip.y, t);
    return vec3(centered / denom, view_z);
}

vec2 project_prev_uv(vec3 world_pos) {
    vec3 p = (prev_view * vec4(world_pos, 1.0)).xyz;
    if (p.z >= -0.001) {
        return vec2(-1.0);
    }
    float t = viewport_orthographic.w;
    vec2 persp_px = vec2(
        prev_focal_clip.x * p.x / p.z + viewport_orthographic.x * 0.5,
        viewport_orthographic.y * 0.5 + prev_focal_clip.x * p.y / p.z
    );
    vec2 ortho_px = vec2(
        -prev_focal_clip.y * p.x + viewport_orthographic.x * 0.5,
        viewport_orthographic.y * 0.5 - prev_focal_clip.y * p.y
    );
    vec2 px = mix(persp_px, ortho_px, t);
    float y_flip = (1.0 - prev_focal_clip.z) * 0.5;
    px.y = mix(px.y, viewport_orthographic.y - px.y, y_flip);
    return px / viewport_orthographic.xy;
}

void main() {
    vec4 current_color = texture(sampler2D(current_color_tex, current_color_smp), uv);
    float depth = texture(sampler2D(current_depth_tex, current_depth_smp), uv).r;

    vec3 current_view = reconstruct_view_pos(uv, depth);
    vec4 world = inv_view * vec4(current_view, 1.0);
    vec2 prev_uv = project_prev_uv(world.xyz);

    bool valid_history = history_valid != 0.0 &&
        prev_uv.x >= 0.0 && prev_uv.x <= 1.0 &&
        prev_uv.y >= 0.0 && prev_uv.y <= 1.0;

    vec4 history_color = vec4(0.0);
    vec3 history_xyz = vec3(0.0);
    if (valid_history) {
        history_color = texture(sampler2D(history_color_tex, history_color_smp), prev_uv);
        history_xyz = texture(sampler2D(history_xyz_tex, history_xyz_smp), prev_uv).xyz;
        valid_history = history_color.a > 0.0 && (view_changed == 0.0 || length(world.xyz - history_xyz) < 100.0);
    }

    if (valid_history) {
        float n = min(history_color.a, 128.0);
        out_color.rgb = (history_color.rgb * n + current_color.rgb) / (n + 1.0);
        out_color.a = min(n + 1.0, 128.0);
        out_xyz.xyz = (history_xyz * n + world.xyz) / (n + 1.0);
        out_xyz.a = 1.0;
    } else {
        out_color = vec4(current_color.rgb, 1.0);
        out_xyz = vec4(world.xyz, 1.0);
    }
}
@end

@fs blit_fs
@image_sample_type display_tex unfilterable_float
layout(binding = 0) uniform texture2D display_tex;
@sampler_type display_smp nonfiltering
layout(binding = 0) uniform sampler   display_smp;

in vec2 uv;
out vec4 out_color;

void main() {
    out_color = texture(sampler2D(display_tex, display_smp), uv);
}
@end

@program accum accum_vs accum_fs
@program taa_accum accum_vs taa_accum_fs
@program blit accum_vs blit_fs
