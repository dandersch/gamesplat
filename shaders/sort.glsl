// Portable 4-pass 8-bit radix sort for splat depth keys. Avoids subgroup
// extensions so the shader path remains viable for both modern GL and WebGPU.

@cs radix_hist_cs
struct RadixUIntData {
    uint count;
};

layout(binding = 0) uniform RadixHistUBO {
    int sort_count;
    int shift;
};

layout(binding = 0) readonly buffer RadixHistKeys {
    RadixUIntData hist_keys[];
};

layout(binding = 1) buffer RadixHistograms {
    RadixUIntData histograms[];
};

shared uint local_hist[256];

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main() {
    uint lid = gl_LocalInvocationID.x;
    uint gid = gl_GlobalInvocationID.x;
    uint group = gl_WorkGroupID.x;

    local_hist[lid] = 0u;
    barrier();

    if (gid < uint(sort_count)) {
        uint key = hist_keys[gid].count;
        uint bin = (key >> uint(shift)) & 255u;
        atomicAdd(local_hist[bin], 1u);
    }
    barrier();

    histograms[group * 256u + lid].count = local_hist[lid];
}
@end

@program radix_hist radix_hist_cs

@cs radix_prefix_cs
struct RadixPrefixUIntData {
    uint count;
};

layout(binding = 0) uniform RadixPrefixUBO {
    int group_count;
};

layout(binding = 0) buffer RadixPrefixHistograms {
    RadixPrefixUIntData histograms[];
};

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

void main() {
    if (gl_LocalInvocationID.x != 0u) {
        return;
    }

    uint groups = uint(group_count);
    uint running = 0u;

    for (uint b = 0u; b < 256u; ++b) {
        for (uint g = 0u; g < groups; ++g) {
            uint idx = g * 256u + b;
            uint count = histograms[idx].count;
            histograms[idx].count = running;
            running += count;
        }
    }
}
@end

@program radix_prefix radix_prefix_cs

@cs radix_scatter_cs
struct RadixScatterUIntData {
    uint count;
};

layout(binding = 0) uniform RadixScatterUBO {
    int sort_count;
    int shift;
};

layout(binding = 0) readonly buffer RadixScatterKeysIn {
    RadixScatterUIntData keys_in[];
};

layout(binding = 1) buffer RadixScatterKeysOut {
    RadixScatterUIntData keys_out[];
};

layout(binding = 2) readonly buffer RadixScatterIdsIn {
    RadixScatterUIntData ids_in[];
};

layout(binding = 3) buffer RadixScatterIdsOut {
    RadixScatterUIntData ids_out[];
};

layout(binding = 4) readonly buffer RadixScatterHistograms {
    RadixScatterUIntData histograms[];
};

shared uint local_bins[256];

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main() {
    uint lid = gl_LocalInvocationID.x;
    uint gid = gl_GlobalInvocationID.x;
    uint group = gl_WorkGroupID.x;

    uint key = 0xFFFFFFFFu;
    uint bin = 255u;
    if (gid < uint(sort_count)) {
        key = keys_in[gid].count;
        bin = (key >> uint(shift)) & 255u;
    }
    local_bins[lid] = bin;
    barrier();

    if (gid >= uint(sort_count)) {
        return;
    }

    uint local_rank = 0u;
    for (uint i = 0u; i < lid; ++i) {
        if (local_bins[i] == bin) {
            local_rank++;
        }
    }

    uint out_index = histograms[group * 256u + bin].count + local_rank;
    keys_out[out_index].count = key;
    ids_out[out_index].count = ids_in[gid].count;
}
@end

@program radix_scatter radix_scatter_cs
