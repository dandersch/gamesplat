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
layout(binding = 0) uniform texture2D sample_tex;
layout(binding = 0) uniform sampler   sample_smp;
layout(binding = 1) uniform texture2D history_tex;
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

@fs blit_fs
layout(binding = 0) uniform texture2D display_tex;
layout(binding = 0) uniform sampler   display_smp;

in vec2 uv;
out vec4 out_color;

void main() {
    out_color = texture(sampler2D(display_tex, display_smp), uv);
}
@end

@program accum accum_vs accum_fs
@program blit accum_vs blit_fs
