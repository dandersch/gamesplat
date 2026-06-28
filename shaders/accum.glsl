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
    mat4 inv_view_proj;
    mat4 prev_view_proj;
    float history_valid;
    float clip_z_01;
    float taa_pad0;
    float taa_pad1;
};

in vec2 uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_xyz;

void main() {
    vec4 current_color = texture(sampler2D(current_color_tex, current_color_smp), uv);
    float depth = texture(sampler2D(current_depth_tex, current_depth_smp), uv).r;
    float z_clip = clip_z_01 != 0.0 ? depth : depth * 2.0 - 1.0;

    vec4 world = inv_view_proj * vec4(uv * 2.0 - 1.0, z_clip, 1.0);
    world /= world.w;

    vec4 prev_clip = prev_view_proj * vec4(world.xyz, 1.0);
    vec2 prev_uv = prev_clip.xy / prev_clip.w * 0.5 + 0.5;

    bool valid_history = history_valid != 0.0 &&
        prev_clip.w > 0.0 &&
        prev_uv.x >= 0.0 && prev_uv.x <= 1.0 &&
        prev_uv.y >= 0.0 && prev_uv.y <= 1.0;

    vec4 history_color = vec4(0.0);
    vec3 history_xyz = vec3(0.0);
    if (valid_history) {
        history_color = texture(sampler2D(history_color_tex, history_color_smp), prev_uv);
        history_xyz = texture(sampler2D(history_xyz_tex, history_xyz_smp), prev_uv).xyz;
        valid_history = history_color.a > 0.0 && length(world.xyz - history_xyz) < 100.0;
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
