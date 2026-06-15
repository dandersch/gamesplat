// Portable 8-bit-pass radix sort for splat depth keys. Avoids subgroup
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

shared uint bin_totals[256];

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main() {
    uint bin = gl_LocalInvocationID.x;
    uint groups = uint(group_count);

    uint total = 0u;
    for (uint g = 0u; g < groups; ++g) {
        total += histograms[g * 256u + bin].count;
    }
    bin_totals[bin] = total;
    barrier();

    for (uint offset = 1u; offset < 256u; offset <<= 1u) {
        uint add = (bin >= offset) ? bin_totals[bin - offset] : 0u;
        barrier();
        bin_totals[bin] += add;
        barrier();
    }

    uint running = (bin == 0u) ? 0u : bin_totals[bin - 1u];
    for (uint g = 0u; g < groups; ++g) {
        uint idx = g * 256u + bin;
        uint count = histograms[idx].count;
        histograms[idx].count = running;
        running += count;
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

shared uint bin_flags[256 * 8];

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main() {
    uint lid = gl_LocalInvocationID.x;
    uint gid = gl_GlobalInvocationID.x;
    uint group = gl_WorkGroupID.x;

    for (uint i = 0u; i < 8u; ++i) {
        bin_flags[lid * 8u + i] = 0u;
    }
    barrier();

    uint key = 0xFFFFFFFFu;
    uint bin = 255u;
    if (gid < uint(sort_count)) {
        key = keys_in[gid].count;
        bin = (key >> uint(shift)) & 255u;
        atomicOr(bin_flags[bin * 8u + (lid >> 5u)], 1u << (lid & 31u));
    }
    barrier();

    if (gid >= uint(sort_count)) {
        return;
    }

    uint local_rank = 0u;
    uint flag_word = lid >> 5u;
    uint flag_bit = 1u << (lid & 31u);
    for (uint i = 0u; i < 8u; ++i) {
        uint bits = bin_flags[bin * 8u + i];
        local_rank += (i < flag_word) ? uint(bitCount(bits)) : 0u;
        local_rank += (i == flag_word) ? uint(bitCount(bits & (flag_bit - 1u))) : 0u;
    }

    uint out_index = histograms[group * 256u + bin].count + local_rank;
    keys_out[out_index].count = key;
    ids_out[out_index].count = ids_in[gid].count;
}
@end

@program radix_scatter radix_scatter_cs
