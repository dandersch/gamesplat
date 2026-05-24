// Equirectangular panorama overlay: fullscreen triangle from gl_VertexIndex,
// samples a panorama texture along the reconstructed view ray.
//
// sokol-shdc input. Compiled into shaders/overlay.glsl.h.

@vs overlay_vs
out vec2 v_ndc;

void main() {
    // Fullscreen triangle (3 verts cover entire screen)
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_ndc = pos * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 0.0, 1.0);
}
@end

@fs overlay_fs
layout(binding = 0) uniform OverlayUniforms {
    mat4 camera_ray_basis;
    vec2 camera_tan_half_fov;
    vec2 camera_pad;
    mat4 ref_rotation;   // 3x3 world-to-refcam rotation in upper-left, Y-flip baked in
    float alpha;
};

// sokol-slang: textures and samplers are separate (Vulkan-style). Combine
// with sampler2D(tex, smp) at the call site.
layout(binding = 0) uniform texture2D panorama_tex;
layout(binding = 0) uniform sampler   panorama_smp;

in vec2 v_ndc;
out vec4 out_color;

const float PI = 3.14159265358979;

void main() {
    // camera_ray_basis (built in camera_get_overlay_ray_basis) is built from
    // a world that is conceptually y-down (see proj-matrix comment in
    // src/camera.cpp). The fullscreen vertex shader writes the framebuffer
    // pixel position directly as v_ndc, so for the fragment at the *top* of
    // the screen v_ndc.y is +1 in OpenGL — but in the basis convention that
    // corresponds to "camera looking up", which is -y in camera space.
    // Negating v_ndc.y here reproduces the original SDL_GPU behavior.
    vec3 camera_dir = normalize(vec3(
        v_ndc.x * camera_tan_half_fov.x,
        -v_ndc.y * camera_tan_half_fov.y,
        1.0
    ));

    vec3 dir = normalize(mat3(camera_ray_basis) * camera_dir);
    vec3 ref_dir = normalize(mat3(ref_rotation) * dir);

    float u = atan(ref_dir.x, ref_dir.z) / (2.0 * PI) + 0.5;
    float v = -asin(clamp(ref_dir.y, -1.0, 1.0)) / PI + 0.5;

    vec4 tex_color = texture(sampler2D(panorama_tex, panorama_smp), vec2(u, v));

    // Premultiplied alpha (matches src=ONE, dst=ONE_MINUS_SRC_ALPHA blend).
    out_color = vec4(tex_color.rgb * alpha, alpha);
}
@end

@program overlay overlay_vs overlay_fs
