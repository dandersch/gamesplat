// Animated mesh + static --object mesh. Vertex format: vec3 pos + vec2 uv.
// Single material texture sampled in the fragment shader; falls back to a
// flat color when use_texture <= 0.5.
//
// sokol-shdc input. Compiled into shaders/mesh.glsl.h.

@vs mesh_vs
layout(binding = 0) uniform MeshUniforms {
    mat4 mvp;
    vec4 color;
    float use_texture;
};

in vec3 in_position;
in vec2 in_uv;

out vec4  v_color;
out vec2  v_uv;
out float v_use_texture;

void main() {
    gl_Position = mvp * vec4(in_position, 1.0);
    v_color = color;
    v_uv = in_uv;
    v_use_texture = use_texture;
}
@end

@fs mesh_fs
layout(binding = 0) uniform texture2D mesh_tex;
layout(binding = 0) uniform sampler   mesh_smp;

in vec4  v_color;
in vec2  v_uv;
in float v_use_texture;

out vec4 out_color;

void main() {
    if (v_use_texture > 0.5) {
        out_color = texture(sampler2D(mesh_tex, mesh_smp), v_uv);
    } else {
        out_color = v_color;
    }
}
@end

@program mesh mesh_vs mesh_fs
