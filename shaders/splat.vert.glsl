#version 450

// SDL_GPU SPIR-V layout for vertex shaders:
//   set 0: vertex samplers, storage textures, storage buffers
//   set 1: vertex uniform buffers
//
// Gaussian data is stored in an RGBA32F texture (16 texels per gaussian,
// 64 floats = original GpuGaussian std430 layout). Width is fixed at
// GAUSSIAN_TEX_WIDTH; height grows with gaussian_count. Layout per gaussian:
//   texel 0 = (pos.x, pos.y, pos.z, opacity)
//   texel 1 = (scale.x, scale.y, scale.z, pad)
//   texel 2 = (rot.w, rot.x, rot.y, rot.z)
//   texel 3 = (color.r, color.g, color.b, pad)            // raw f_dc
//   texels 4..14 = sh_rest (45 floats packed tightly, RGB triples per coeff)
//   texel 15.last_three_components = pad
// This packing is identical to the previous storage-buffer GpuGaussian layout,
// so pack_gpu_gaussians() output can be uploaded byte-for-byte.
layout(set = 0, binding = 0) uniform sampler2D gaussian_tex;

const int GAUSSIAN_TEX_WIDTH    = 4096;
const int GAUSSIAN_TEXELS_PER   = 16;

layout(set = 1, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec2 viewport;
    float orthographic; // ORTHO: 1.0 = orthographic, 0.0 = perspective
    // Pure persp/ortho focal lengths in pixels. Used per-branch below so the
    // mid-transition mix() doesn't re-introduce the blended proj[0][0] value
    // (which caused a visible zoom wobble for inspect targets with
    // ortho_size < 1).
    float persp_focal;
    float ortho_focal;
};

// Per-instance vertex attribute: the sorted gaussian index for this instance.
// Replaces the old `sorted_indices[]` storage buffer.
layout(location = 0) in uint splat_id;

layout(location = 0) out vec3 frag_color;
layout(location = 1) out float frag_opacity;
layout(location = 2) out vec2 frag_center;
layout(location = 3) out vec3 frag_conic;

vec4 fetch_texel(int k) {
    int linear = int(splat_id) * GAUSSIAN_TEXELS_PER + k;
    int x = linear & (GAUSSIAN_TEX_WIDTH - 1);   // width is POT (4096)
    int y = linear >> 12;                        // log2(4096) = 12
    return texelFetch(gaussian_tex, ivec2(x, y), 0);
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

    // 3. Build rotation matrix from quaternion
    // rot = (w, x, y, z) stored as rot.x=w, rot.y=x, rot.z=y, rot.w=z
    // GLSL mat3 constructor takes COLUMNS, so we provide column0, column1, column2
    float qw = rot.x, qx = rot.y, qy = rot.z, qz = rot.w;
    mat3 R = mat3(
        // Column 0
        1.0 - 2.0*(qy*qy + qz*qz),
        2.0*(qx*qy + qw*qz),
        2.0*(qx*qz - qw*qy),
        // Column 1
        2.0*(qx*qy - qw*qz),
        1.0 - 2.0*(qx*qx + qz*qz),
        2.0*(qy*qz + qw*qx),
        // Column 2
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

    // 6. Focal lengths in pixels. fx == fy under our convention (uniform
    //    pixel scale on x and y once aspect is accounted for). Using the
    //    pure persp/ortho focals separately avoids a wobble from the
    //    blended proj[0][0] feeding both mix() branches.
    float fx_p = persp_focal;
    float fy_p = persp_focal;
    float fx_o = ortho_focal;
    float fy_o = ortho_focal;

    // 7. Jacobian of the screen-space projection at t
    // Lerp between perspective and orthographic Jacobians
    float J00 = mix(fx_p / t.z,                     -fx_o, orthographic);
    float J11 = mix(-fy_p / t.z,                     fy_o, orthographic);
    float J02 = mix(-fx_p * t.x / (t.z * t.z),     0.0,   orthographic);
    float J12 = mix( fy_p * t.y / (t.z * t.z),     0.0,   orthographic);

    // 8. View rotation (upper-left 3x3 of view matrix)
    mat3 W = mat3(view);

    // 9. Project 3D covariance to 2D: Σ' = J · W · Σ · Wᵀ · Jᵀ
    // First compute W · Σ · Wᵀ
    mat3 WcovW = W * cov3d * transpose(W);

    // Then apply J (2x3 matrix) to get 2x2 covariance
    // J = | J00   0   J02 |
    //     |  0   J11  J12 |
    // cov2d = J * WcovW * J^T
    // WcovW is symmetric so [i][j] == [j][i]
    float a = J00*J00*WcovW[0][0] + 2.0*J00*J02*WcovW[0][2] + J02*J02*WcovW[2][2];
    float b = J00*J11*WcovW[0][1] + J00*J12*WcovW[0][2] + J02*J11*WcovW[1][2] + J02*J12*WcovW[2][2];
    float c = J11*J11*WcovW[1][1] + 2.0*J11*J12*WcovW[1][2] + J12*J12*WcovW[2][2];

    // 10. Add low-pass filter
    a += 0.3;
    c += 0.3;

    // 11. Compute inverse (conic) for fragment shader
    float det = a * c - b * b;
    if (det < 1e-6) det = 1e-6;  // Guard against degenerate case
    vec3 conic = vec3(c / det, -b / det, a / det);

    // 12. Compute screen-space center (lerp between perspective and orthographic)
    vec2 persp_center = vec2(
        fx_p * t.x / t.z + viewport.x * 0.5,
        viewport.y * 0.5 - fy_p * t.y / t.z
    );
    vec2 ortho_center = vec2(
        -fx_o * t.x + viewport.x * 0.5,
        viewport.y * 0.5 + fy_o * t.y
    );
    vec2 center_px = mix(persp_center, ortho_center, orthographic);

    // 13. Compute quad radius (3 sigma)
    float radius_x = ceil(3.0 * sqrt(a));
    float radius_y = ceil(3.0 * sqrt(c));

    // 14. Position this vertex
    vec2 pos_px = center_px + corner * vec2(radius_x, radius_y);
    
    // Convert pixel position to NDC
    // In SDL_GPU/Vulkan: NDC Y goes from -1 (bottom) to +1 (top)
    // But we computed pos_px with Y=0 at top, so flip again
    vec2 ndc = vec2(
        2.0 * pos_px.x / viewport.x - 1.0,
        1.0 - 2.0 * pos_px.y / viewport.y  // Flip Y for NDC
    );
    // Compute proper depth from projection matrix (proj[2][2]*z + proj[3][2]) / (-z)
    float ndc_z = (proj[2][2] * t.z + proj[3][2]) / (-t.z);
    gl_Position = vec4(ndc, ndc_z, 1.0);

    // 15. Evaluate spherical harmonics (degree 3) for view-dependent color.
    //     dir = normalized vector from camera to gaussian, in world space.
    //     cam_pos_world = -W^T * view[3].xyz   (W = mat3(view))
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

    // Fetch the 12 texels covering sh_rest (floats 16..63 = 48 floats total,
    // last 3 are padding). Flatten into a 48-float array so SH(k) can index
    // three consecutive floats at offset k*3.
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

    // 16. Pass outputs to fragment shader
    frag_color = color;
    frag_opacity = opacity;
    frag_center = center_px;
    frag_conic = conic;
}
