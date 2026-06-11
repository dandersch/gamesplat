// GPU culling and depth-key generation for Gaussian splats.

@cs cull_cs
struct CullGaussianData {
    float data[64];
};

struct UIntData {
    uint value;
};

layout(binding = 0) uniform CullUBO {
    mat4 view;
    mat4 proj;
    int gaussian_count;
    float orthographic;
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

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

uint positive_float_key(float v) {
    return floatBitsToUint(max(v, 0.0));
}

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= uint(gaussian_count)) {
        return;
    }

    vec3 position = vec3(
        gaussian_data[idx].data[0],
        gaussian_data[idx].data[1],
        gaussian_data[idx].data[2]
    );

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

    output_splat_ids[idx].value = visible ? idx : 0xFFFFFFFFu;
    output_depth_keys[idx].value = visible ? positive_float_key(-p_view.z) : 0u;
}
@end

@program cull cull_cs
