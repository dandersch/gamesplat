// GPU culling and depth-key generation for Gaussian splats.

@cs cull_cs
struct CullGaussianData {
    float data[64];
};

struct UIntData {
    uint count;
};

struct CullProjectedSplatData {
    vec4 color_opacity; // rgb color, a opacity
    vec4 center_radius; // xy raster-space center, zw raster-space radius
    vec4 conic_depth;   // xyz inverse covariance/conic, w ndc depth
};

layout(binding = 0) uniform CullUBO {
    mat4 view;
    mat4 proj;
    vec2 viewport;
    int gaussian_count;
    float orthographic;
    float persp_focal;
    float ortho_focal;
    float clip_y_sign;
    float clip_z_01;
};

layout(binding = 1) uniform SplatEffectUBO {
    vec4 effect_center_radius; // xyz = scene center, w = scene radius
    vec4 effect_params;        // x = elapsed, y = duration, z = strength, w = active
    vec4 effect_color;         // rgb = tint, a = tint strength
};

layout(binding = 0) readonly buffer GaussianBuffer {
    CullGaussianData gaussian_data[];
};

layout(binding = 1) buffer OutputSplatIds {
    UIntData output_splat_ids[];
};

layout(binding = 2) buffer OutputDepthKeys {
    UIntData output_depth_keys[];
};

layout(binding = 3) buffer ProjectedSplats {
    CullProjectedSplatData projected_splats[];
};

layout(binding = 4) buffer VisibleCount {
    UIntData visible_count[];
};

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

uint positive_float_key(float v) {
    return floatBitsToUint(max(v, 0.0));
}

uint far_to_near_depth_key(float positive_view_depth) {
    // Bitonic sort is ascending. Flip positive float depth keys so farther
    // splats sort first for premultiplied back-to-front alpha blending.
    return 0xFFFFFFFFu - positive_float_key(positive_view_depth);
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

vec4 fetch_vec4(uint splat_id, int k) {
    int base = k * 4;
    return vec4(
        gaussian_data[splat_id].data[base + 0],
        gaussian_data[splat_id].data[base + 1],
        gaussian_data[splat_id].data[base + 2],
        gaussian_data[splat_id].data[base + 3]
    );
}

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(gaussian_count)) {
        return;
    }

    vec4 t0 = fetch_vec4(idx, 0);  // pos.xyz, opacity
    vec4 t1 = fetch_vec4(idx, 1);  // scale.xyz, pad
    vec4 t2 = fetch_vec4(idx, 2);  // rot (w,x,y,z)
    vec4 t3 = fetch_vec4(idx, 3);  // dc.rgb, pad
    vec3 position = t0.xyz;
    float opacity = t0.w;
    vec3 scale = t1.xyz;
    vec4 rot = t2;
    vec3 dc = t3.xyz;

    float effect_wave = 0.0;
    if (effect_params.w > 0.5) {
        float duration = max(effect_params.y, 0.001);
        float progress = clamp(effect_params.x / duration, 0.0, 1.0);
        float scene_radius = max(effect_center_radius.w, 0.001);
        vec3 effect_center = effect_center_radius.xyz;

        vec3 to_splat = position - effect_center;
        vec3 jitter = hash31(float(idx)) * (scene_radius * 0.015);
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

    vec4 p_view4 = view * vec4(position, 1.0);
    vec3 p_view = p_view4.xyz;

    bool visible = p_view.z <= -0.2;
    if (visible) {
        float inv_z = -1.0 / p_view.z;
        float ndc_x = (proj[0][0] * p_view.x) * inv_z * (1.0 - orthographic)
                    +  proj[0][0] * p_view.x * orthographic;
        float ndc_y = (proj[1][1] * p_view.y) * inv_z * (1.0 - orthographic)
                    +  proj[1][1] * p_view.y * orthographic;
        visible = abs(ndc_x) <= 1.3 && abs(ndc_y) <= 1.3;
    }

    if (!visible) {
        projected_splats[idx].color_opacity = vec4(0.0);
        projected_splats[idx].center_radius = vec4(0.0);
        projected_splats[idx].conic_depth = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    uint compact_idx = atomicAdd(visible_count[0].count, 1u);
    output_splat_ids[compact_idx].count = idx;
    output_depth_keys[compact_idx].count = far_to_near_depth_key(-p_view.z);

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
    mat3 S = mat3(
        scale.x, 0, 0,
        0, scale.y, 0,
        0, 0, scale.z
    );
    mat3 M = R * S;
    mat3 cov3d = M * transpose(M);

    float fx_p = persp_focal;
    float fy_p = persp_focal;
    float fx_o = ortho_focal;
    float fy_o = ortho_focal;

    float J00 = mix(fx_p / p_view.z,                 -fx_o, orthographic);
    float J11 = mix( fy_p / p_view.z,                -fy_o, orthographic);
    float J02 = mix(-fx_p * p_view.x / (p_view.z * p_view.z), 0.0, orthographic);
    float J12 = mix(-fy_p * p_view.y / (p_view.z * p_view.z), 0.0, orthographic);

    mat3 W = mat3(view);
    mat3 WcovW = W * cov3d * transpose(W);
    float a = J00*J00*WcovW[0][0] + 2.0*J00*J02*WcovW[0][2] + J02*J02*WcovW[2][2];
    float b = J00*J11*WcovW[0][1] + J00*J12*WcovW[0][2] + J02*J11*WcovW[1][2] + J02*J12*WcovW[2][2];
    float c = J11*J11*WcovW[1][1] + 2.0*J11*J12*WcovW[1][2] + J12*J12*WcovW[2][2];

    a += 0.3;
    c += 0.3;

    float det = a * c - b * b;
    if (det < 1e-6) det = 1e-6;
    vec3 conic = vec3(c / det, -b / det, a / det);

    vec2 persp_center = vec2(
        fx_p * p_view.x / p_view.z + viewport.x * 0.5,
        viewport.y * 0.5 + fy_p * p_view.y / p_view.z
    );
    vec2 ortho_center = vec2(
        -fx_o * p_view.x + viewport.x * 0.5,
        viewport.y * 0.5 - fy_o * p_view.y
    );
    vec2 center_px = mix(persp_center, ortho_center, orthographic);

    float y_flip = (1.0 - clip_y_sign) * 0.5;
    vec2 raster_center_px = vec2(center_px.x, mix(center_px.y, viewport.y - center_px.y, y_flip));
    vec3 raster_conic = vec3(conic.x, conic.y * clip_y_sign, conic.z);

    float radius_x = ceil(3.0 * sqrt(a));
    float radius_y = ceil(3.0 * sqrt(c));

    float ndc_z = (proj[2][2] * p_view.z + proj[3][2]) / (-p_view.z);
    ndc_z = mix(ndc_z, ndc_z * 0.5 + 0.5, clip_z_01);

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

    float sh_flat[48];
    for (int i = 0; i < 12; ++i) {
        vec4 tt = fetch_vec4(idx, 4 + i);
        sh_flat[i*4 + 0] = tt.x;
        sh_flat[i*4 + 1] = tt.y;
        sh_flat[i*4 + 2] = tt.z;
        sh_flat[i*4 + 3] = tt.w;
    }
    #define SH(k) vec3(sh_flat[(k)*3 + 0], sh_flat[(k)*3 + 1], sh_flat[(k)*3 + 2])

    vec3 result = SH_C0 * dc;
    float x = dir.x, y = dir.y, z = dir.z;
    result += -SH_C1 * y * SH(0);
    result +=  SH_C1 * z * SH(1);
    result += -SH_C1 * x * SH(2);
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, yz = y*z, xz = z*x;
    result += SH_C2_0 * xy            * SH(3);
    result += SH_C2_1 * yz            * SH(4);
    result += SH_C2_2 * (2.0*zz - xx - yy) * SH(5);
    result += SH_C2_3 * xz            * SH(6);
    result += SH_C2_4 * (xx - yy)     * SH(7);
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

    projected_splats[idx].color_opacity = vec4(color, opacity);
    projected_splats[idx].center_radius = vec4(raster_center_px, radius_x, radius_y);
    projected_splats[idx].conic_depth = vec4(raster_conic, ndc_z);
}
@end

@program cull cull_cs

@cs cull_reset_cs
struct ResetUIntData {
    uint count;
};

layout(binding = 0) uniform ResetUBO {
    int sort_count;
};

layout(binding = 0) buffer ResetVisibleCount {
    ResetUIntData visible_count[];
};

layout(binding = 1) buffer ResetOutputSplatIds {
    ResetUIntData output_splat_ids[];
};

layout(binding = 2) buffer ResetDepthKeys {
    ResetUIntData output_depth_keys[];
};

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx == 0u) {
        visible_count[0].count = 0u;
    }
    if (idx < uint(sort_count)) {
        output_splat_ids[idx].count = 0xFFFFFFFFu;
        output_depth_keys[idx].count = 0xFFFFFFFFu;
    }
}
@end

@program cull_reset cull_reset_cs
