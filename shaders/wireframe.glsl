// Wireframe cubes drawn around node centers (top-down map overlay etc.).
// LINELIST primitive, no depth write, single UBO with mvp + color.
//
// sokol-shdc input. Compiled into shaders/wireframe.glsl.h.

@vs wireframe_vs
layout(binding = 0) uniform WireframeUniforms {
    mat4 mvp;
    vec4 color;
};

in vec3 in_position;

out vec4 v_color;

void main() {
    gl_Position = mvp * vec4(in_position, 1.0);
    v_color = color;
}
@end

@fs wireframe_fs
in vec4 v_color;
out vec4 out_color;

void main() {
    out_color = v_color;
}
@end

@program wireframe wireframe_vs wireframe_fs
