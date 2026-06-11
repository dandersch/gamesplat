// Portable GPU sort for splat depth keys. This is intentionally a plain
// storage-buffer bitonic sort: slower than a subgroup radix sort, but it
// translates to both modern GL and WebGPU.

@cs bitonic_sort_cs
struct SortUIntData {
    uint count;
};

layout(binding = 0) uniform BitonicSortUBO {
    int sort_count;
    int stage_k;
    int stage_j;
};

layout(binding = 0) buffer SortKeys {
    SortUIntData sort_keys[];
};

layout(binding = 1) buffer SortIds {
    SortUIntData sort_ids[];
};

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main() {
    uint i = gl_GlobalInvocationID.x;
    uint count = uint(sort_count);
    uint k = uint(stage_k);
    uint j = uint(stage_j);

    if (i >= count) {
        return;
    }

    uint other = i ^ j;
    if (other <= i || other >= count) {
        return;
    }

    bool ascending = (i & k) == 0u;

    uint key_a = sort_keys[i].count;
    uint key_b = sort_keys[other].count;
    uint id_a = sort_ids[i].count;
    uint id_b = sort_ids[other].count;

    bool swap_pair = ascending ? (key_a > key_b) : (key_a < key_b);
    if (swap_pair) {
        sort_keys[i].count = key_b;
        sort_keys[other].count = key_a;
        sort_ids[i].count = id_b;
        sort_ids[other].count = id_a;
    }
}
@end

@program bitonic_sort bitonic_sort_cs
