// 3D Gaussian splat rendering. Vertex shader fetches packed gaussian data
// from an RGBA32F texture (16 texels per gaussian) and projects a 2D oriented
// quad with screen-space covariance. Per-instance vertex attribute carries
// the sorted gaussian id.
//
// sokol-shdc input. Compiled into shaders/splat.glsl.h.

@vs splat_vs
// Gaussian data is laid out as 16 RGBA32F texels per gaussian (= 64 floats,
// byte-identical to the host-side GpuGaussian struct):
//   texel 0 = (pos.x, pos.y, pos.z, opacity)
//   texel 1 = (scale.x, scale.y, scale.z, pad)
//   texel 2 = (rot.w, rot.x, rot.y, rot.z)
//   texel 3 = (color.r, color.g, color.b, pad)   // raw f_dc
//   texels 4..14 = sh_rest (45 floats packed tightly, RGB triples per coeff)
//   texel 15.last_three_components = pad
// Texture width is fixed (POT) so we can use bit ops to address.
layout(binding = 0) uniform texture2D gaussian_tex;
layout(binding = 0) uniform sampler   gaussian_smp;

const int GAUSSIAN_TEX_WIDTH  = 4096;
const int GAUSSIAN_TEXELS_PER = 16;

layout(binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec2 viewport;
    float orthographic; // 1.0 = orthographic, 0.0 = perspective
    float persp_focal;
    float ortho_focal;
};

layout(binding = 1) uniform SplatEffectUBO {
    vec4 effect_center_radius; // xyz = scene center, w = scene radius
    vec4 effect_params;        // x = elapsed, y = duration, z = strength, w = active
    vec4 effect_color;         // rgb = tint, a = tint strength
};

// Per-instance attribute: the sorted gaussian index for this instance.
in uint splat_id;

out vec3  frag_color;
out float frag_opacity;
out vec2  frag_center;
out vec3  frag_conic;

vec4 fetch_texel(int k) {
    int linear = int(splat_id) * GAUSSIAN_TEXELS_PER + k;
    int x = linear & (GAUSSIAN_TEX_WIDTH - 1);   // width is POT (4096)
    int y = linear >> 12;                        // log2(4096) = 12
    return texelFetch(sampler2D(gaussian_tex, gaussian_smp), ivec2(x, y), 0);
}

float hash11(float p) {
    return fract(sin(p * 127.1) * 43758.5453123);
}

vec3 hash31(float p) {
    return vec3(
        hash11(p + 1.0),
        hash11(p + 2.0),
        hash11(p + 3.0)
    ) * 2.0 - 1.0;
}

void main() {
    // 1. Determine quad corner from vertex ID
    int quad_verts[6] = int[6](0, 1, 2, 0, 2, 3);
    vec2 corners[4] = vec2[4](
        vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, 1)
    );
    vec2 corner = corners[quad_verts[gl_VertexIndex % 6]];

    // 2. Fetch Gaussian header (texels 0..3)
    vec4 t0 = fetch_texel(0);  // pos.xyz, opacity
    vec4 t1 = fetch_texel(1);  // scale.xyz, pad
    vec4 t2 = fetch_texel(2);  // rot (w,x,y,z)
    vec4 t3 = fetch_texel(3);  // dc.rgb, pad
    vec3  position = t0.xyz;
    float opacity  = t0.w;
    vec3  scale    = t1.xyz;
    vec4  rot      = t2;
    vec3  dc       = t3.xyz;

    float effect_wave = 0.0;
    if (effect_params.w > 0.5) {
        float duration = max(effect_params.y, 0.001);
        float progress = clamp(effect_params.x / duration, 0.0, 1.0);
        float scene_radius = max(effect_center_radius.w, 0.001);
        vec3 effect_center = effect_center_radius.xyz;

        vec3 to_splat = position - effect_center;
        vec3 jitter = hash31(float(splat_id)) * (scene_radius * 0.015);
        float dist = length(to_splat);
        float wave_radius = progress * scene_radius * 1.15;
        float band_width = scene_radius * 0.085;
        float band = (dist - wave_radius) / band_width;
        float wave = exp(-band * band);
        float attack = smoothstep(0.0, 0.08, progress);
        float release = 1.0 - smoothstep(0.82, 1.0, progress);
        effect_wave = wave * attack * release;

        vec3 dir = normalize(to_splat + jitter + vec3(0.0, scene_radius * 0.01, 0.0));
        float signed_ripple = sin(progress * 18.849556 + dist * 8.0 / scene_radius);
        position += dir * (effect_wave * effect_params.z * scene_radius);
        position += dir * (effect_wave * signed_ripple * 0.012 * scene_radius);
        scale *= 1.0 + effect_wave * 0.75;
        opacity *= 1.0 - effect_wave * 0.20;
    }

    // 3. Build rotation matrix from quaternion (column-major mat3 ctor).
    float qw = rot.x, qx = rot.y, qy = rot.z, qz = rot.w;
    mat3 R = mat3(
        1.0 - 2.0*(qy*qy + qz*qz),
        2.0*(qx*qy + qw*qz),
        2.0*(qx*qz - qw*qy),

        2.0*(qx*qy - qw*qz),
        1.0 - 2.0*(qx*qx + qz*qz),
        2.0*(qy*qz + qw*qx),

        2.0*(qx*qz + qw*qy),
        2.0*(qy*qz - qw*qx),
        1.0 - 2.0*(qx*qx + qy*qy)
    );

    // 4. Build 3D covariance: Σ = R·S·Sᵀ·Rᵀ = M·Mᵀ where M = R·S
    mat3 S = mat3(
        scale.x, 0, 0,
        0, scale.y, 0,
        0, 0, scale.z
    );
    mat3 M = R * S;
    mat3 cov3d = M * transpose(M);

    // 5. Transform center to view space
    vec4 p_view4 = view * vec4(position, 1.0);
    vec3 t = p_view4.xyz;

    // 6. Focal lengths in pixels.
    float fx_p = persp_focal;
    float fy_p = persp_focal;
    float fx_o = ortho_focal;
    float fy_o = ortho_focal;

    // 7. Jacobian of the screen-space projection at t (persp ↔ ortho lerp).
    float J00 = mix(fx_p / t.z,                     -fx_o, orthographic);
    float J11 = mix( fy_p / t.z,                    -fy_o, orthographic);
    float J02 = mix(-fx_p * t.x / (t.z * t.z),     0.0,   orthographic);
    float J12 = mix(-fy_p * t.y / (t.z * t.z),     0.0,   orthographic);

    // 8. View rotation (upper-left 3x3 of view matrix)
    mat3 W = mat3(view);

    // 9. Project 3D covariance to 2D: Σ' = J · W · Σ · Wᵀ · Jᵀ
    mat3 WcovW = W * cov3d * transpose(W);
    float a = J00*J00*WcovW[0][0] + 2.0*J00*J02*WcovW[0][2] + J02*J02*WcovW[2][2];
    float b = J00*J11*WcovW[0][1] + J00*J12*WcovW[0][2] + J02*J11*WcovW[1][2] + J02*J12*WcovW[2][2];
    float c = J11*J11*WcovW[1][1] + 2.0*J11*J12*WcovW[1][2] + J12*J12*WcovW[2][2];

    // 10. Low-pass filter
    a += 0.3;
    c += 0.3;

    // 11. Inverse (conic) for fragment shader
    float det = a * c - b * b;
    if (det < 1e-6) det = 1e-6;
    vec3 conic = vec3(c / det, -b / det, a / det);

    // 12. Screen-space center
    vec2 persp_center = vec2(
        fx_p * t.x / t.z + viewport.x * 0.5,
        viewport.y * 0.5 + fy_p * t.y / t.z
    );
    vec2 ortho_center = vec2(
        -fx_o * t.x + viewport.x * 0.5,
        viewport.y * 0.5 - fy_o * t.y
    );
    vec2 center_px = mix(persp_center, ortho_center, orthographic);

    // 13. Quad radius (3 sigma)
    float radius_x = ceil(3.0 * sqrt(a));
    float radius_y = ceil(3.0 * sqrt(c));

    // 14. Position this vertex
    vec2 pos_px = center_px + corner * vec2(radius_x, radius_y);

    // Gaussian PLY data is Y-up (the gaussian loader does not negate Y on
    // load, unlike the OBJ/GLTF mesh loaders which flip Y to align meshes
    // with the renderer's Y-down convention — see src/mesh.cpp). So for
    // splats we need the OpenGL-style NDC mapping (+y at top of framebuffer)
    // *without* the extra inversion that meshes get from the projection's
    // Y-flip. The splat shader does not multiply by proj for X/Y (only for
    // ndc_z below), so this mapping is independent of the mesh path.
    vec2 ndc = vec2(
        2.0 * pos_px.x / viewport.x - 1.0,
        2.0 * pos_px.y / viewport.y - 1.0
    );
    float ndc_z = (proj[2][2] * t.z + proj[3][2]) / (-t.z);
    gl_Position = vec4(ndc, ndc_z, 1.0);

    // 15. View-dependent color via degree-3 SH.
    vec3 cam_pos_world = -transpose(W) * view[3].xyz;
    vec3 dir = normalize(position - cam_pos_world);

    const float SH_C0 = 0.28209479177387814;
    const float SH_C1 = 0.4886025119029199;
    const float SH_C2_0 =  1.0925484305920792;
    const float SH_C2_1 = -1.0925484305920792;
    const float SH_C2_2 =  0.31539156525252005;
    const float SH_C2_3 = -1.0925484305920792;
    const float SH_C2_4 =  0.5462742152960396;
    const float SH_C3_0 = -0.5900435899266435;
    const float SH_C3_1 =  2.890611442640554;
    const float SH_C3_2 = -0.4570457994644658;
    const float SH_C3_3 =  0.3731763325901154;
    const float SH_C3_4 = -0.4570457994644658;
    const float SH_C3_5 =  1.445305721320277;
    const float SH_C3_6 = -0.5900435899266435;

    // Fetch the 12 texels covering sh_rest (floats 16..63 = 48 floats; last
    // 3 are padding). Flatten into a 48-float array so SH(k) can index three
    // consecutive floats at offset k*3.
    float sh_flat[48];
    for (int i = 0; i < 12; ++i) {
        vec4 tt = fetch_texel(4 + i);
        sh_flat[i*4 + 0] = tt.x;
        sh_flat[i*4 + 1] = tt.y;
        sh_flat[i*4 + 2] = tt.z;
        sh_flat[i*4 + 3] = tt.w;
    }
    #define SH(k) vec3(sh_flat[(k)*3 + 0], sh_flat[(k)*3 + 1], sh_flat[(k)*3 + 2])

    vec3 result = SH_C0 * dc;

    float x = dir.x, y = dir.y, z = dir.z;
    // Degree 1
    result += -SH_C1 * y * SH(0);
    result +=  SH_C1 * z * SH(1);
    result += -SH_C1 * x * SH(2);
    // Degree 2
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, yz = y*z, xz = x*z;
    result += SH_C2_0 * xy            * SH(3);
    result += SH_C2_1 * yz            * SH(4);
    result += SH_C2_2 * (2.0*zz - xx - yy) * SH(5);
    result += SH_C2_3 * xz            * SH(6);
    result += SH_C2_4 * (xx - yy)     * SH(7);
    // Degree 3
    result += SH_C3_0 * y * (3.0*xx - yy)               * SH(8);
    result += SH_C3_1 * xy * z                          * SH(9);
    result += SH_C3_2 * y * (4.0*zz - xx - yy)          * SH(10);
    result += SH_C3_3 * z * (2.0*zz - 3.0*xx - 3.0*yy)  * SH(11);
    result += SH_C3_4 * x * (4.0*zz - xx - yy)          * SH(12);
    result += SH_C3_5 * z * (xx - yy)                   * SH(13);
    result += SH_C3_6 * x * (xx - 3.0*yy)               * SH(14);

    result += 0.5;
    vec3 color = max(result, vec3(0.0));
    color += effect_color.rgb * (effect_wave * effect_color.a);

    frag_color = color;
    frag_opacity = opacity;
    frag_center = center_px;
    frag_conic = conic;
}
@end

@fs splat_fs
in vec3  frag_color;
in float frag_opacity;
in vec2  frag_center;
in vec3  frag_conic;

out vec4 out_color;

void main() {
    vec2 d = gl_FragCoord.xy - frag_center;

    float power = -0.5 * (
        frag_conic.x * d.x * d.x +
        2.0 * frag_conic.y * d.x * d.y +
        frag_conic.z * d.y * d.y
    );

    if (power > 0.0) discard;

    float alpha = min(frag_opacity * exp(power), 0.99);
    if (alpha < 1.0 / 255.0) discard;

    // Premultiplied alpha output
    out_color = vec4(frag_color * alpha, alpha);
}
@end

@program splat splat_vs splat_fs
