#include "renderer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

static uint8_t* load_file(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Failed to open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = (uint8_t*)malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);
    *out_size = (size_t)sz;
    return data;
}

bool renderer_init(Renderer* r, SDL_GPUDevice* device, SDL_Window* window) {
    r->device = device;
    r->window = window;
    r->gaussian_texture = NULL;
    r->gaussian_sampler = NULL;
    r->gaussian_tex_w = 0;
    r->gaussian_tex_h = 0;
    r->index_buffer = NULL;
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        r->transfer_bufs[i] = NULL;
        r->frame_fences[i] = NULL;
    }
    r->current_frame = 0;
    r->splat_pipeline = NULL;
    r->mesh_pipeline = NULL;
    r->mesh_gpu = {};
    r->object_gpu = {};
    r->mesh_sampler = NULL;
    r->mesh_transform = {};
    r->mesh_transform.scale = 1.0f;
    r->object_transform = {};
    r->object_transform.scale = 1.0f;
    r->depth_texture = NULL;
    r->depth_w = 0;
    r->depth_h = 0;
    r->gaussian_count = 0;
    r->swapchain_format = SDL_GetGPUSwapchainTextureFormat(device, window);
    fprintf(stderr, "Swapchain format: %d\n", (int)r->swapchain_format);

    // Load shaders
    size_t vert_size, frag_size;
    uint8_t* vert_code = load_file("shaders/splat.vert.spv", &vert_size);
    uint8_t* frag_code = load_file("shaders/splat.frag.spv", &frag_size);
    if (!vert_code || !frag_code) return false;

    SDL_GPUShaderCreateInfo vert_info = {};
    vert_info.code = vert_code;
    vert_info.code_size = vert_size;
    vert_info.entrypoint = "main";
    vert_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vert_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vert_info.num_samplers = 1;          // gaussian_tex (texelFetch source)
    vert_info.num_storage_buffers = 0;   // sorted indices now a vertex attribute
    vert_info.num_uniform_buffers = 1;

    SDL_GPUShader* vert_shader = SDL_CreateGPUShader(device, &vert_info);
    free(vert_code);
    if (!vert_shader) { fprintf(stderr, "FAIL vertex shader: %s\n", SDL_GetError()); free(frag_code); return false; }

    SDL_GPUShaderCreateInfo frag_info = {};
    frag_info.code = frag_code;
    frag_info.code_size = frag_size;
    frag_info.entrypoint = "main";
    frag_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    frag_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    frag_info.num_storage_buffers = 0;
    frag_info.num_uniform_buffers = 0;

    SDL_GPUShader* frag_shader = SDL_CreateGPUShader(device, &frag_info);
    free(frag_code);
    if (!frag_shader) { fprintf(stderr, "FAIL fragment shader: %s\n", SDL_GetError()); SDL_ReleaseGPUShader(device, vert_shader); return false; }

    // Pipeline
    SDL_GPUColorTargetDescription color_target = {};
    color_target.format = r->swapchain_format;
    color_target.blend_state.enable_blend = true;
    color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    color_target.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

    // Splat pipeline takes a single per-instance vertex attribute: the sorted
    // gaussian index (uint). gl_VertexIndex (0..5) supplies the quad corner.
    SDL_GPUVertexBufferDescription splat_vb_desc = {};
    splat_vb_desc.slot = 0;
    splat_vb_desc.pitch = sizeof(uint32_t);
    splat_vb_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;

    SDL_GPUVertexAttribute splat_attr = {};
    splat_attr.location = 0;
    splat_attr.buffer_slot = 0;
    splat_attr.format = SDL_GPU_VERTEXELEMENTFORMAT_UINT;
    splat_attr.offset = 0;

    SDL_GPUGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.vertex_shader = vert_shader;
    pipeline_info.fragment_shader = frag_shader;
    pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline_info.vertex_input_state.num_vertex_buffers = 1;
    pipeline_info.vertex_input_state.vertex_buffer_descriptions = &splat_vb_desc;
    pipeline_info.vertex_input_state.num_vertex_attributes = 1;
    pipeline_info.vertex_input_state.vertex_attributes = &splat_attr;
    pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    // Splats only test depth (against the mesh, so they don't poke through it
    // from behind), they do not write depth or stencil.
    pipeline_info.depth_stencil_state.enable_depth_test = true;
    pipeline_info.depth_stencil_state.enable_depth_write = false;
    pipeline_info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    pipeline_info.target_info.num_color_targets = 1;
    pipeline_info.target_info.color_target_descriptions = &color_target;
    pipeline_info.target_info.has_depth_stencil_target = true;
    pipeline_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;

    r->splat_pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeline_info);
    SDL_ReleaseGPUShader(device, vert_shader);
    SDL_ReleaseGPUShader(device, frag_shader);

    if (!r->splat_pipeline) {
        fprintf(stderr, "FAIL pipeline: %s\n", SDL_GetError());
        return false;
    }

    // --- Overlay pipeline ---
    size_t ov_vert_size, ov_frag_size;
    uint8_t* ov_vert_code = load_file("shaders/overlay.vert.spv", &ov_vert_size);
    uint8_t* ov_frag_code = load_file("shaders/overlay.frag.spv", &ov_frag_size);
    if (!ov_vert_code || !ov_frag_code) {
        fprintf(stderr, "Failed to load overlay shaders\n");
        free(ov_vert_code); free(ov_frag_code);
        return false;
    }

    SDL_GPUShaderCreateInfo ov_vert_info = {};
    ov_vert_info.code = ov_vert_code;
    ov_vert_info.code_size = ov_vert_size;
    ov_vert_info.entrypoint = "main";
    ov_vert_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ov_vert_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;

    SDL_GPUShader* ov_vert = SDL_CreateGPUShader(device, &ov_vert_info);
    free(ov_vert_code);

    SDL_GPUShaderCreateInfo ov_frag_info = {};
    ov_frag_info.code = ov_frag_code;
    ov_frag_info.code_size = ov_frag_size;
    ov_frag_info.entrypoint = "main";
    ov_frag_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ov_frag_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    ov_frag_info.num_samplers = 1;
    ov_frag_info.num_uniform_buffers = 1;

    SDL_GPUShader* ov_frag = SDL_CreateGPUShader(device, &ov_frag_info);
    free(ov_frag_code);

    if (!ov_vert || !ov_frag) {
        fprintf(stderr, "FAIL overlay shaders: %s\n", SDL_GetError());
        if (ov_vert) SDL_ReleaseGPUShader(device, ov_vert);
        if (ov_frag) SDL_ReleaseGPUShader(device, ov_frag);
        return false;
    }

    SDL_GPUColorTargetDescription ov_color_target = {};
    ov_color_target.format = r->swapchain_format;
    ov_color_target.blend_state.enable_blend = true;
    ov_color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ov_color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ov_color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    ov_color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ov_color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ov_color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    ov_color_target.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;

    SDL_GPUGraphicsPipelineCreateInfo ov_pipeline_info = {};
    ov_pipeline_info.vertex_shader = ov_vert;
    ov_pipeline_info.fragment_shader = ov_frag;
    ov_pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ov_pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ov_pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    // Stencil: only draw where stencil != 1 (i.e. NOT inside the mesh
    // silhouette). Inside the mesh region, the mesh + splat composite stays.
    ov_pipeline_info.depth_stencil_state.enable_stencil_test = true;
    ov_pipeline_info.depth_stencil_state.compare_mask = 0xFF;
    ov_pipeline_info.depth_stencil_state.write_mask = 0x00;
    ov_pipeline_info.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_NOT_EQUAL;
    ov_pipeline_info.depth_stencil_state.front_stencil_state.pass_op = SDL_GPU_STENCILOP_KEEP;
    ov_pipeline_info.depth_stencil_state.front_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
    ov_pipeline_info.depth_stencil_state.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
    ov_pipeline_info.depth_stencil_state.back_stencil_state = ov_pipeline_info.depth_stencil_state.front_stencil_state;
    ov_pipeline_info.target_info.num_color_targets = 1;
    ov_pipeline_info.target_info.color_target_descriptions = &ov_color_target;
    ov_pipeline_info.target_info.has_depth_stencil_target = true;
    ov_pipeline_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;

    r->overlay_pipeline = SDL_CreateGPUGraphicsPipeline(device, &ov_pipeline_info);
    SDL_ReleaseGPUShader(device, ov_vert);
    SDL_ReleaseGPUShader(device, ov_frag);

    if (!r->overlay_pipeline) {
        fprintf(stderr, "FAIL overlay pipeline: %s\n", SDL_GetError());
        return false;
    }

    // --- Darken pipeline ---
    // Fullscreen translucent black quad used to dim the FPS view behind the
    // top-down map overlay. Reuses overlay.vert (fullscreen triangle).
    size_t dk_frag_size;
    uint8_t* dk_vert_code = load_file("shaders/overlay.vert.spv", &ov_vert_size);
    uint8_t* dk_frag_code = load_file("shaders/darken.frag.spv", &dk_frag_size);
    if (!dk_vert_code || !dk_frag_code) {
        fprintf(stderr, "Failed to load darken shaders\n");
        free(dk_vert_code); free(dk_frag_code);
        return false;
    }

    SDL_GPUShaderCreateInfo dk_vert_info = {};
    dk_vert_info.code = dk_vert_code;
    dk_vert_info.code_size = ov_vert_size;
    dk_vert_info.entrypoint = "main";
    dk_vert_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    dk_vert_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;

    SDL_GPUShader* dk_vert = SDL_CreateGPUShader(device, &dk_vert_info);
    free(dk_vert_code);

    SDL_GPUShaderCreateInfo dk_frag_info = {};
    dk_frag_info.code = dk_frag_code;
    dk_frag_info.code_size = dk_frag_size;
    dk_frag_info.entrypoint = "main";
    dk_frag_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    dk_frag_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    dk_frag_info.num_uniform_buffers = 1; // DarkenUniforms (animated fog time)

    SDL_GPUShader* dk_frag = SDL_CreateGPUShader(device, &dk_frag_info);
    free(dk_frag_code);

    if (!dk_vert || !dk_frag) {
        fprintf(stderr, "FAIL darken shaders: %s\n", SDL_GetError());
        if (dk_vert) SDL_ReleaseGPUShader(device, dk_vert);
        if (dk_frag) SDL_ReleaseGPUShader(device, dk_frag);
        return false;
    }

    // Same premultiplied-alpha blend as the panorama overlay so the dark
    // fragment darkens the existing FPS pixels rather than replacing them.
    SDL_GPUColorTargetDescription dk_color_target = {};
    dk_color_target.format = r->swapchain_format;
    dk_color_target.blend_state.enable_blend = true;
    dk_color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    dk_color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    dk_color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    dk_color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    dk_color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    dk_color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    dk_color_target.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;

    SDL_GPUGraphicsPipelineCreateInfo dk_pipeline_info = {};
    dk_pipeline_info.vertex_shader = dk_vert;
    dk_pipeline_info.fragment_shader = dk_frag;
    dk_pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    dk_pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    dk_pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    // No depth/stencil testing: we just want to darken the entire framebuffer
    // before the map's mesh+splats are drawn on top.
    dk_pipeline_info.target_info.num_color_targets = 1;
    dk_pipeline_info.target_info.color_target_descriptions = &dk_color_target;
    dk_pipeline_info.target_info.has_depth_stencil_target = true;
    dk_pipeline_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;

    r->darken_pipeline = SDL_CreateGPUGraphicsPipeline(device, &dk_pipeline_info);
    SDL_ReleaseGPUShader(device, dk_vert);
    SDL_ReleaseGPUShader(device, dk_frag);

    if (!r->darken_pipeline) {
        fprintf(stderr, "FAIL darken pipeline: %s\n", SDL_GetError());
        return false;
    }

    // Overlay sampler (linear filtering for smooth panorama sampling)
    SDL_GPUSamplerCreateInfo sampler_info = {};
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    r->overlay_sampler = SDL_CreateGPUSampler(device, &sampler_info);

    // --- Wireframe pipeline ---
    size_t wf_vert_size, wf_frag_size;
    uint8_t* wf_vert_code = load_file("shaders/wireframe.vert.spv", &wf_vert_size);
    uint8_t* wf_frag_code = load_file("shaders/wireframe.frag.spv", &wf_frag_size);
    if (!wf_vert_code || !wf_frag_code) {
        fprintf(stderr, "Failed to load wireframe shaders\n");
        free(wf_vert_code); free(wf_frag_code);
        return false;
    }

    SDL_GPUShaderCreateInfo wf_vert_info = {};
    wf_vert_info.code = wf_vert_code;
    wf_vert_info.code_size = wf_vert_size;
    wf_vert_info.entrypoint = "main";
    wf_vert_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    wf_vert_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    wf_vert_info.num_uniform_buffers = 1;

    SDL_GPUShader* wf_vert = SDL_CreateGPUShader(device, &wf_vert_info);
    free(wf_vert_code);

    SDL_GPUShaderCreateInfo wf_frag_info = {};
    wf_frag_info.code = wf_frag_code;
    wf_frag_info.code_size = wf_frag_size;
    wf_frag_info.entrypoint = "main";
    wf_frag_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    wf_frag_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;

    SDL_GPUShader* wf_frag = SDL_CreateGPUShader(device, &wf_frag_info);
    free(wf_frag_code);

    if (!wf_vert || !wf_frag) {
        fprintf(stderr, "FAIL wireframe shaders: %s\n", SDL_GetError());
        if (wf_vert) SDL_ReleaseGPUShader(device, wf_vert);
        if (wf_frag) SDL_ReleaseGPUShader(device, wf_frag);
        return false;
    }

    SDL_GPUColorTargetDescription wf_color_target = {};
    wf_color_target.format = r->swapchain_format;
    wf_color_target.blend_state.enable_blend = true;
    wf_color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    wf_color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    wf_color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    wf_color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    wf_color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    wf_color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    wf_color_target.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;

    SDL_GPUVertexBufferDescription wf_vb_desc = {};
    wf_vb_desc.slot = 0;
    wf_vb_desc.pitch = 3 * sizeof(float);
    wf_vb_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute wf_attr = {};
    wf_attr.location = 0;
    wf_attr.buffer_slot = 0;
    wf_attr.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    wf_attr.offset = 0;

    SDL_GPUGraphicsPipelineCreateInfo wf_pipeline_info = {};
    wf_pipeline_info.vertex_shader = wf_vert;
    wf_pipeline_info.fragment_shader = wf_frag;
    wf_pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;
    wf_pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    wf_pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    wf_pipeline_info.vertex_input_state.num_vertex_buffers = 1;
    wf_pipeline_info.vertex_input_state.vertex_buffer_descriptions = &wf_vb_desc;
    wf_pipeline_info.vertex_input_state.num_vertex_attributes = 1;
    wf_pipeline_info.vertex_input_state.vertex_attributes = &wf_attr;
    wf_pipeline_info.target_info.num_color_targets = 1;
    wf_pipeline_info.target_info.color_target_descriptions = &wf_color_target;
    wf_pipeline_info.target_info.has_depth_stencil_target = true;
    wf_pipeline_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;

    r->wireframe_pipeline = SDL_CreateGPUGraphicsPipeline(device, &wf_pipeline_info);

    if (!r->wireframe_pipeline) {
        fprintf(stderr, "FAIL wireframe pipeline: %s\n", SDL_GetError());
        SDL_ReleaseGPUShader(device, wf_vert);
        SDL_ReleaseGPUShader(device, wf_frag);
        return false;
    }

    SDL_ReleaseGPUShader(device, wf_vert);
    SDL_ReleaseGPUShader(device, wf_frag);

    // --- Mesh pipeline (dedicated shaders with UV + texture support) ---
    size_t mesh_vert_size, mesh_frag_size;
    uint8_t* mesh_vert_code = load_file("shaders/mesh.vert.spv", &mesh_vert_size);
    uint8_t* mesh_frag_code = load_file("shaders/mesh.frag.spv", &mesh_frag_size);
    if (!mesh_vert_code || !mesh_frag_code) {
        fprintf(stderr, "Failed to load mesh shaders\n");
        free(mesh_vert_code); free(mesh_frag_code);
        return false;
    }

    SDL_GPUShaderCreateInfo mesh_vert_info = {};
    mesh_vert_info.code = mesh_vert_code;
    mesh_vert_info.code_size = mesh_vert_size;
    mesh_vert_info.entrypoint = "main";
    mesh_vert_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    mesh_vert_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    mesh_vert_info.num_uniform_buffers = 1;

    SDL_GPUShader* mesh_vs = SDL_CreateGPUShader(device, &mesh_vert_info);
    free(mesh_vert_code);

    SDL_GPUShaderCreateInfo mesh_frag_info = {};
    mesh_frag_info.code = mesh_frag_code;
    mesh_frag_info.code_size = mesh_frag_size;
    mesh_frag_info.entrypoint = "main";
    mesh_frag_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    mesh_frag_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    mesh_frag_info.num_samplers = 1;

    SDL_GPUShader* mesh_fs = SDL_CreateGPUShader(device, &mesh_frag_info);
    free(mesh_frag_code);

    if (!mesh_vs || !mesh_fs) {
        fprintf(stderr, "FAIL mesh shaders: %s\n", SDL_GetError());
        if (mesh_vs) SDL_ReleaseGPUShader(device, mesh_vs);
        if (mesh_fs) SDL_ReleaseGPUShader(device, mesh_fs);
        return false;
    }

    SDL_GPUColorTargetDescription mesh_color_target = {};
    mesh_color_target.format = r->swapchain_format;
    mesh_color_target.blend_state.enable_blend = false;
    mesh_color_target.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

    SDL_GPUVertexBufferDescription mesh_vb_desc = {};
    mesh_vb_desc.slot = 0;
    mesh_vb_desc.pitch = 5 * sizeof(float); // vec3 pos + vec2 uv
    mesh_vb_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute mesh_attrs[2] = {};
    mesh_attrs[0].location = 0;
    mesh_attrs[0].buffer_slot = 0;
    mesh_attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    mesh_attrs[0].offset = 0;
    mesh_attrs[1].location = 1;
    mesh_attrs[1].buffer_slot = 0;
    mesh_attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    mesh_attrs[1].offset = 3 * sizeof(float);

    SDL_GPUGraphicsPipelineCreateInfo mesh_pipeline_info = {};
    mesh_pipeline_info.vertex_shader = mesh_vs;
    mesh_pipeline_info.fragment_shader = mesh_fs;
    mesh_pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    mesh_pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    mesh_pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
    mesh_pipeline_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    mesh_pipeline_info.vertex_input_state.num_vertex_buffers = 1;
    mesh_pipeline_info.vertex_input_state.vertex_buffer_descriptions = &mesh_vb_desc;
    mesh_pipeline_info.vertex_input_state.num_vertex_attributes = 2;
    mesh_pipeline_info.vertex_input_state.vertex_attributes = mesh_attrs;
    mesh_pipeline_info.depth_stencil_state.enable_depth_test = true;
    mesh_pipeline_info.depth_stencil_state.enable_depth_write = true;
    mesh_pipeline_info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    // Stencil: every mesh pixel writes 1, marking "this is the mesh region".
    // The overlay pass will draw only where stencil != 1.
    mesh_pipeline_info.depth_stencil_state.enable_stencil_test = true;
    mesh_pipeline_info.depth_stencil_state.write_mask = 0xFF;
    mesh_pipeline_info.depth_stencil_state.compare_mask = 0xFF;
    mesh_pipeline_info.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    mesh_pipeline_info.depth_stencil_state.front_stencil_state.pass_op = SDL_GPU_STENCILOP_REPLACE;
    mesh_pipeline_info.depth_stencil_state.front_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
    mesh_pipeline_info.depth_stencil_state.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
    mesh_pipeline_info.depth_stencil_state.back_stencil_state = mesh_pipeline_info.depth_stencil_state.front_stencil_state;
    mesh_pipeline_info.target_info.num_color_targets = 1;
    mesh_pipeline_info.target_info.color_target_descriptions = &mesh_color_target;
    mesh_pipeline_info.target_info.has_depth_stencil_target = true;
    mesh_pipeline_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;

    r->mesh_pipeline = SDL_CreateGPUGraphicsPipeline(device, &mesh_pipeline_info);
    SDL_ReleaseGPUShader(device, mesh_vs);
    SDL_ReleaseGPUShader(device, mesh_fs);

    if (!r->mesh_pipeline) {
        fprintf(stderr, "FAIL mesh pipeline: %s\n", SDL_GetError());
        return false;
    }

    // Upload cube geometry (unit cube centered at origin, ±0.5)
    float cube_verts[8 * 3] = {
        -0.5f, -0.5f, -0.5f,  // 0
         0.5f, -0.5f, -0.5f,  // 1
         0.5f,  0.5f, -0.5f,  // 2
        -0.5f,  0.5f, -0.5f,  // 3
        -0.5f, -0.5f,  0.5f,  // 4
         0.5f, -0.5f,  0.5f,  // 5
         0.5f,  0.5f,  0.5f,  // 6
        -0.5f,  0.5f,  0.5f,  // 7
    };
    uint16_t cube_indices[24] = {
        0,1, 1,2, 2,3, 3,0,  // back face
        4,5, 5,6, 6,7, 7,4,  // front face
        0,4, 1,5, 2,6, 3,7,  // connecting edges
    };

    uint32_t vb_size = sizeof(cube_verts);
    uint32_t ib_size = sizeof(cube_indices);

    SDL_GPUBufferCreateInfo cube_vb_info = {};
    cube_vb_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    cube_vb_info.size = vb_size;
    r->cube_vertex_buffer = SDL_CreateGPUBuffer(device, &cube_vb_info);

    SDL_GPUBufferCreateInfo cube_ib_info = {};
    cube_ib_info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    cube_ib_info.size = ib_size;
    r->cube_index_buffer = SDL_CreateGPUBuffer(device, &cube_ib_info);

    SDL_GPUTransferBufferCreateInfo cube_xfer_info = {};
    cube_xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    cube_xfer_info.size = vb_size + ib_size;
    SDL_GPUTransferBuffer* cube_xfer = SDL_CreateGPUTransferBuffer(device, &cube_xfer_info);

    void* cube_map = SDL_MapGPUTransferBuffer(device, cube_xfer, false);
    memcpy(cube_map, cube_verts, vb_size);
    memcpy((uint8_t*)cube_map + vb_size, cube_indices, ib_size);
    SDL_UnmapGPUTransferBuffer(device, cube_xfer);

    SDL_GPUCommandBuffer* cube_cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* cube_copy = SDL_BeginGPUCopyPass(cube_cmd);

    SDL_GPUTransferBufferLocation cube_src = {};
    cube_src.transfer_buffer = cube_xfer;
    cube_src.offset = 0;
    SDL_GPUBufferRegion cube_vb_dst = {};
    cube_vb_dst.buffer = r->cube_vertex_buffer;
    cube_vb_dst.size = vb_size;
    SDL_UploadToGPUBuffer(cube_copy, &cube_src, &cube_vb_dst, false);

    cube_src.offset = vb_size;
    SDL_GPUBufferRegion cube_ib_dst = {};
    cube_ib_dst.buffer = r->cube_index_buffer;
    cube_ib_dst.size = ib_size;
    SDL_UploadToGPUBuffer(cube_copy, &cube_src, &cube_ib_dst, false);

    SDL_EndGPUCopyPass(cube_copy);
    SDL_SubmitGPUCommandBuffer(cube_cmd);
    SDL_WaitForGPUIdle(device);
    SDL_ReleaseGPUTransferBuffer(device, cube_xfer);

    SDL_GPUSamplerCreateInfo mesh_sampler_info = {};
    mesh_sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    mesh_sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    mesh_sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    mesh_sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    mesh_sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    mesh_sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    r->mesh_sampler = SDL_CreateGPUSampler(device, &mesh_sampler_info);

    // Gaussian sampler. texelFetch ignores filter/wrap state but SDL_GPU still
    // requires a sampler when binding a sampled texture.
    SDL_GPUSamplerCreateInfo gs_info = {};
    gs_info.min_filter = SDL_GPU_FILTER_NEAREST;
    gs_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    gs_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    gs_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    gs_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    gs_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    r->gaussian_sampler = SDL_CreateGPUSampler(device, &gs_info);

    fprintf(stderr, "Renderer init OK\n");
    return true;
}

// Gaussian texture layout: each gaussian occupies 16 consecutive RGBA32F
// texels (= 64 floats, same byte layout as GpuGaussian). Width is fixed at
// 4096 (POT, so the shader can use bit ops). Height grows to hold N gaussians.
#define GAUSSIAN_TEX_WIDTH      4096u
#define GAUSSIAN_TEXELS_PER     16u
#define GAUSSIAN_BYTES_PER_TEXEL 16u  // RGBA32F

void renderer_upload_gaussians(Renderer* r, const GaussianScene* scene) {
    r->gaussian_count = scene->gaussian_count;

    // --- Gaussian data texture (static) ---
    uint32_t total_texels = scene->gaussian_count * GAUSSIAN_TEXELS_PER;
    uint32_t tex_w = GAUSSIAN_TEX_WIDTH;
    uint32_t tex_h = (total_texels + tex_w - 1u) / tex_w;
    r->gaussian_tex_w = tex_w;
    r->gaussian_tex_h = tex_h;

    SDL_GPUTextureCreateInfo gtex_info = {};
    gtex_info.type = SDL_GPU_TEXTURETYPE_2D;
    gtex_info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    gtex_info.width = tex_w;
    gtex_info.height = tex_h;
    gtex_info.layer_count_or_depth = 1;
    gtex_info.num_levels = 1;
    gtex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    r->gaussian_texture = SDL_CreateGPUTexture(r->device, &gtex_info);

    // --- Index vertex buffer (dynamic, per-instance, updated each frame) ---
    SDL_GPUBufferCreateInfo idx_info = {};
    idx_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    idx_info.size = scene->gaussian_count * sizeof(uint32_t);
    r->index_buffer = SDL_CreateGPUBuffer(r->device, &idx_info);

    // Transfer buffers for per-frame index upload (one per frame in flight)
    SDL_GPUTransferBufferCreateInfo xfer_info = {};
    xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    xfer_info.size = scene->gaussian_count * sizeof(uint32_t);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        r->transfer_bufs[i] = SDL_CreateGPUTransferBuffer(r->device, &xfer_info);
    }

    // --- One-shot upload of gaussian data into the texture ---
    GpuGaussian* gpu_data = pack_gpu_gaussians(scene);

    uint32_t tex_buf_size = tex_w * tex_h * GAUSSIAN_BYTES_PER_TEXEL;
    uint32_t payload_size = scene->gaussian_count * (uint32_t)sizeof(GpuGaussian);

    SDL_GPUTransferBufferCreateInfo upload_xfer_info = {};
    upload_xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    upload_xfer_info.size = tex_buf_size;
    SDL_GPUTransferBuffer* upload_xfer = SDL_CreateGPUTransferBuffer(r->device, &upload_xfer_info);

    if (!r->gaussian_texture || !r->index_buffer || !r->transfer_bufs[0] || !upload_xfer) {
        fprintf(stderr, "FAIL: gaussian resource creation failed\n");
        free(gpu_data);
        return;
    }

    void* map = SDL_MapGPUTransferBuffer(r->device, upload_xfer, false);
    if (!map) { free(gpu_data); return; }

    // GpuGaussian is 64 floats = 16 RGBA32F texels. Tight packed array of N
    // gaussians == tight packed array of N*16 texels, so a single memcpy
    // suffices. Trailing slack texels (if any) are zeroed for cleanliness.
    memcpy(map, gpu_data, payload_size);
    if (tex_buf_size > payload_size) {
        memset((uint8_t*)map + payload_size, 0, tex_buf_size - payload_size);
    }
    SDL_UnmapGPUTransferBuffer(r->device, upload_xfer);
    free(gpu_data);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(r->device);
    if (!cmd) return;

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo tsrc = {};
    tsrc.transfer_buffer = upload_xfer;
    tsrc.offset = 0;
    tsrc.pixels_per_row = tex_w;
    tsrc.rows_per_layer = tex_h;

    SDL_GPUTextureRegion tdst = {};
    tdst.texture = r->gaussian_texture;
    tdst.w = tex_w;
    tdst.h = tex_h;
    tdst.d = 1;

    SDL_UploadToGPUTexture(copy, &tsrc, &tdst, false);
    SDL_EndGPUCopyPass(copy);

    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(r->device);
    SDL_ReleaseGPUTransferBuffer(r->device, upload_xfer);

    fprintf(stderr, "Uploaded %u gaussians (gaussian_tex = %ux%u RGBA32F)\n",
            scene->gaussian_count, tex_w, tex_h);
}

// Release everything held by a MeshGpu and zero it. Safe to call on an
// already-empty MeshGpu (all fields NULL/0).
static void mesh_gpu_release(SDL_GPUDevice* device, MeshGpu* m) {
    if (m->vertex_buffer)  { SDL_ReleaseGPUBuffer(device, m->vertex_buffer);  m->vertex_buffer = NULL; }
    if (m->index_buffer)   { SDL_ReleaseGPUBuffer(device, m->index_buffer);   m->index_buffer  = NULL; }
    if (m->textures) {
        for (uint32_t i = 0; i < m->texture_count; ++i) {
            if (m->textures[i]) SDL_ReleaseGPUTexture(device, m->textures[i]);
        }
        free(m->textures);
        m->textures = NULL;
    }
    m->texture_count = 0;
    if (m->default_texture) { SDL_ReleaseGPUTexture(device, m->default_texture); m->default_texture = NULL; }
    if (m->submeshes) { free(m->submeshes); m->submeshes = NULL; }
    m->submesh_count = 0;
}

// Upload mesh geometry, textures, and submesh metadata to `out`. Replaces any
// existing GPU resources in `out`. Returns true on success.
static bool mesh_gpu_upload(SDL_GPUDevice* device, const Mesh* mesh, MeshGpu* out) {
    mesh_gpu_release(device, out);

    uint32_t vert_count  = mesh->vertex_count;
    uint32_t index_count = mesh->index_count;
    uint32_t vb_size = vert_count * 5 * sizeof(float); // vec3 pos + vec2 uv
    uint32_t ib_size = index_count * sizeof(uint32_t);

    SDL_GPUBufferCreateInfo vb_info = {};
    vb_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vb_info.size = vb_size;
    out->vertex_buffer = SDL_CreateGPUBuffer(device, &vb_info);

    SDL_GPUBufferCreateInfo ib_info = {};
    ib_info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ib_info.size = ib_size;
    out->index_buffer = SDL_CreateGPUBuffer(device, &ib_info);

    if (!out->vertex_buffer || !out->index_buffer) {
        fprintf(stderr, "FAIL: mesh buffer creation\n");
        return false;
    }

    // Compute total size needed in transfer buffer (geometry + all textures + 1x1 default)
    uint32_t geom_xfer_size = vb_size + ib_size;
    uint32_t tex_xfer_size = 4; // 1x1 white default
    for (uint32_t i = 0; i < mesh->texture_count; ++i) {
        tex_xfer_size += mesh->textures[i].w * mesh->textures[i].h * 4;
    }

    SDL_GPUTransferBufferCreateInfo xfer_info = {};
    xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    xfer_info.size = geom_xfer_size + tex_xfer_size;
    SDL_GPUTransferBuffer* xfer = SDL_CreateGPUTransferBuffer(device, &xfer_info);

    uint8_t* map = (uint8_t*)SDL_MapGPUTransferBuffer(device, xfer, false);
    memcpy(map, mesh->vertices, vb_size);
    memcpy(map + vb_size, mesh->indices, ib_size);
    // 1x1 white pixel at offset geom_xfer_size
    uint32_t cursor = geom_xfer_size;
    map[cursor + 0] = 255; map[cursor + 1] = 255; map[cursor + 2] = 255; map[cursor + 3] = 255;
    uint32_t default_tex_offset = cursor;
    cursor += 4;
    // Per-texture offsets
    std::vector<uint32_t> tex_offsets(mesh->texture_count);
    for (uint32_t i = 0; i < mesh->texture_count; ++i) {
        tex_offsets[i] = cursor;
        uint32_t sz = mesh->textures[i].w * mesh->textures[i].h * 4;
        memcpy(map + cursor, mesh->textures[i].rgba, sz);
        cursor += sz;
    }
    SDL_UnmapGPUTransferBuffer(device, xfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTransferBufferLocation src = {};
    src.transfer_buffer = xfer;
    src.offset = 0;
    SDL_GPUBufferRegion vb_dst = {};
    vb_dst.buffer = out->vertex_buffer;
    vb_dst.size = vb_size;
    SDL_UploadToGPUBuffer(copy, &src, &vb_dst, false);

    src.offset = vb_size;
    SDL_GPUBufferRegion ib_dst = {};
    ib_dst.buffer = out->index_buffer;
    ib_dst.size = ib_size;
    SDL_UploadToGPUBuffer(copy, &src, &ib_dst, false);

    // Create + upload 1x1 white default texture
    {
        SDL_GPUTextureCreateInfo tex_info = {};
        tex_info.type = SDL_GPU_TEXTURETYPE_2D;
        tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        tex_info.width = 1;
        tex_info.height = 1;
        tex_info.layer_count_or_depth = 1;
        tex_info.num_levels = 1;
        tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        out->default_texture = SDL_CreateGPUTexture(device, &tex_info);

        SDL_GPUTextureTransferInfo tex_src = {};
        tex_src.transfer_buffer = xfer;
        tex_src.offset = default_tex_offset;
        SDL_GPUTextureRegion tex_dst = {};
        tex_dst.texture = out->default_texture;
        tex_dst.w = 1; tex_dst.h = 1; tex_dst.d = 1;
        SDL_UploadToGPUTexture(copy, &tex_src, &tex_dst, false);
    }

    // Create + upload all material textures
    if (mesh->texture_count > 0) {
        out->textures = (SDL_GPUTexture**)calloc(mesh->texture_count, sizeof(SDL_GPUTexture*));
        out->texture_count = mesh->texture_count;
        for (uint32_t i = 0; i < mesh->texture_count; ++i) {
            const MeshTexture& mt = mesh->textures[i];
            SDL_GPUTextureCreateInfo tex_info = {};
            tex_info.type = SDL_GPU_TEXTURETYPE_2D;
            tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            tex_info.width = mt.w;
            tex_info.height = mt.h;
            tex_info.layer_count_or_depth = 1;
            tex_info.num_levels = 1;
            tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            out->textures[i] = SDL_CreateGPUTexture(device, &tex_info);

            SDL_GPUTextureTransferInfo tex_src = {};
            tex_src.transfer_buffer = xfer;
            tex_src.offset = tex_offsets[i];
            SDL_GPUTextureRegion tex_dst = {};
            tex_dst.texture = out->textures[i];
            tex_dst.w = mt.w; tex_dst.h = mt.h; tex_dst.d = 1;
            SDL_UploadToGPUTexture(copy, &tex_src, &tex_dst, false);
        }
    }

    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(device);
    SDL_ReleaseGPUTransferBuffer(device, xfer);

    // Copy submesh metadata
    out->submesh_count = mesh->submesh_count;
    if (mesh->submesh_count > 0) {
        out->submeshes = (MeshSubmesh*)malloc(mesh->submesh_count * sizeof(MeshSubmesh));
        memcpy(out->submeshes, mesh->submeshes, mesh->submesh_count * sizeof(MeshSubmesh));
    }

    fprintf(stderr, "Uploaded mesh: %u verts, %u indices, %u textures, %u submeshes\n",
            vert_count, index_count, mesh->texture_count, mesh->submesh_count);

    return true;
}

bool renderer_upload_mesh(Renderer* r, const Mesh* mesh) {
    return mesh_gpu_upload(r->device, mesh, &r->mesh_gpu);
}

bool renderer_upload_object_mesh(Renderer* r, const Mesh* mesh) {
    return mesh_gpu_upload(r->device, mesh, &r->object_gpu);
}

// Multiply two column-major 4x4 matrices: out = a * b
static void mat4_mul(const float* a, const float* b, float* out) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            out[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1] + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
        }
    }
}

// Build a column-major translation+scale model matrix
static void mat4_translate_scale(float tx, float ty, float tz, float s, float* out) {
    memset(out, 0, 16 * sizeof(float));
    out[0]  = s;
    out[5]  = s;
    out[10] = s;
    out[12] = tx;
    out[13] = ty;
    out[14] = tz;
    out[15] = 1.0f;
}

// Build column-major model matrix: T * Rz * Ry * Rx * S (intrinsic Z-Y-X Euler).
static void mat4_from_transform(const MeshTransform& t, float* out) {
    float cx = cosf(t.rotation_euler[0]), sx = sinf(t.rotation_euler[0]);
    float cy = cosf(t.rotation_euler[1]), sy = sinf(t.rotation_euler[1]);
    float cz = cosf(t.rotation_euler[2]), sz = sinf(t.rotation_euler[2]);
    float s = t.scale;

    // Combined rotation R = Rz * Ry * Rx (row-major math, written into column-major)
    float r00 = cy * cz;
    float r01 = sx * sy * cz - cx * sz;
    float r02 = cx * sy * cz + sx * sz;
    float r10 = cy * sz;
    float r11 = sx * sy * sz + cx * cz;
    float r12 = cx * sy * sz - sx * cz;
    float r20 = -sy;
    float r21 = sx * cy;
    float r22 = cx * cy;

    // Column-major layout: out[col*4 + row]
    out[0]  = r00 * s; out[1]  = r10 * s; out[2]  = r20 * s; out[3]  = 0.0f;
    out[4]  = r01 * s; out[5]  = r11 * s; out[6]  = r21 * s; out[7]  = 0.0f;
    out[8]  = r02 * s; out[9]  = r12 * s; out[10] = r22 * s; out[11] = 0.0f;
    out[12] = t.translation[0];
    out[13] = t.translation[1];
    out[14] = t.translation[2];
    out[15] = 1.0f;
}

// Upload the scene's currently-sorted indices into r->index_buffer via the
// per-frame transfer buffer. cycle=true on the second-or-later upload within
// a single command buffer so the GPU sees fresh contents.
static void upload_sorted_indices(Renderer* r, const GaussianScene* scene,
                                   SDL_GPUCommandBuffer* cmd,
                                   SDL_GPUTransferBuffer* transfer_buf,
                                   bool cycle) {
    if (scene->visible_count == 0 || !transfer_buf) return;
    void* map = SDL_MapGPUTransferBuffer(r->device, transfer_buf, cycle);
    if (!map) return;
    memcpy(map, scene->sorted_indices, scene->visible_count * sizeof(uint32_t));
    SDL_UnmapGPUTransferBuffer(r->device, transfer_buf);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {};
    src.transfer_buffer = transfer_buf;
    src.offset = 0;
    SDL_GPUBufferRegion dst = {};
    dst.buffer = r->index_buffer;
    dst.offset = 0;
    dst.size = scene->visible_count * sizeof(uint32_t);
    SDL_UploadToGPUBuffer(copy, &src, &dst, cycle);
    SDL_EndGPUCopyPass(copy);
}

// Draws mesh + splats + (optional) panorama overlay + (optional) wireframe
// nodes for one camera into the currently-bound render pass. ImGui is NOT
// drawn here; the caller is responsible for that on the final pass.
static void draw_world(Renderer* r, SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass,
                       const GaussianScene* scene, const CameraUniforms* cam,
                       const OverlayParams* overlay, const NodeRenderParams* nodes,
                       float wireframe_occlusion) {
    // Mesh: writes color, depth, and stencil=1 to mark its silhouette.
    // Same lambda is reused for the static --object mesh (with identity model).
    auto draw_mesh_slot = [&](const MeshGpu* m, const float* mvp) {
        if (!r->mesh_pipeline || !m->vertex_buffer || !m->index_buffer || m->submesh_count == 0) return;

        SDL_BindGPUGraphicsPipeline(pass, r->mesh_pipeline);
        SDL_SetGPUStencilReference(pass, 1);

        SDL_GPUBufferBinding mesh_vb_bind = {};
        mesh_vb_bind.buffer = m->vertex_buffer;
        SDL_BindGPUVertexBuffers(pass, 0, &mesh_vb_bind, 1);

        SDL_GPUBufferBinding mesh_ib_bind = {};
        mesh_ib_bind.buffer = m->index_buffer;
        SDL_BindGPUIndexBuffer(pass, &mesh_ib_bind, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        for (uint32_t i = 0; i < m->submesh_count; ++i) {
            const MeshSubmesh& sm = m->submeshes[i];
            if (sm.index_count == 0) continue;

            SDL_GPUTexture* tex = m->default_texture;
            float use_texture = 0.0f;
            if (sm.texture_id >= 0 && (uint32_t)sm.texture_id < m->texture_count
                && m->textures[sm.texture_id]) {
                tex = m->textures[sm.texture_id];
                use_texture = 1.0f;
            }

            SDL_GPUTextureSamplerBinding mesh_tex_bind = {};
            mesh_tex_bind.texture = tex;
            mesh_tex_bind.sampler = r->mesh_sampler;
            SDL_BindGPUFragmentSamplers(pass, 0, &mesh_tex_bind, 1);

            struct { float mvp[16]; float color[4]; float use_texture; float _pad[3]; } mesh_uniforms;
            memcpy(mesh_uniforms.mvp, mvp, 16 * sizeof(float));
            mesh_uniforms.color[0] = 1.0f;
            mesh_uniforms.color[1] = 1.0f;
            mesh_uniforms.color[2] = 1.0f;
            mesh_uniforms.color[3] = 1.0f;
            mesh_uniforms.use_texture = use_texture;
            mesh_uniforms._pad[0] = mesh_uniforms._pad[1] = mesh_uniforms._pad[2] = 0.0f;

            SDL_PushGPUVertexUniformData(cmd, 0, &mesh_uniforms, sizeof(mesh_uniforms));
            SDL_DrawGPUIndexedPrimitives(pass, sm.index_count, 1, sm.index_offset, 0, 0);
        }
    };

    // Build VP once; reused for both mesh slots.
    float view_corrected[16];
    memcpy(view_corrected, cam->view, sizeof(view_corrected));
    view_corrected[0]  = -view_corrected[0];
    view_corrected[4]  = -view_corrected[4];
    view_corrected[8]  = -view_corrected[8];
    view_corrected[12] = -view_corrected[12];
    float vp[16];
    mat4_mul(cam->proj, view_corrected, vp);

    // Primary animated mesh.
    {
        float model[16];
        mat4_from_transform(r->mesh_transform, model);
        float mvp[16];
        mat4_mul(vp, model, mvp);
        draw_mesh_slot(&r->mesh_gpu, mvp);
    }

    // Static --object mesh (transformed by object_transform; identity by default).
    if (r->object_gpu.vertex_buffer) {
        float obj_model[16];
        mat4_from_transform(r->object_transform, obj_model);
        float obj_mvp[16];
        mat4_mul(vp, obj_model, obj_mvp);
        draw_mesh_slot(&r->object_gpu, obj_mvp);
    }

    // Splats
    if (scene->visible_count > 0 && r->splat_pipeline && r->gaussian_texture && r->index_buffer) {
        SDL_BindGPUGraphicsPipeline(pass, r->splat_pipeline);
        SDL_FColor blend_const = { 0, 0, 0, wireframe_occlusion };
        SDL_SetGPUBlendConstants(pass, blend_const);

        // Per-instance sorted-index attribute (replaces old indices SSBO).
        SDL_GPUBufferBinding splat_vb_bind = {};
        splat_vb_bind.buffer = r->index_buffer;
        splat_vb_bind.offset = 0;
        SDL_BindGPUVertexBuffers(pass, 0, &splat_vb_bind, 1);

        // Gaussian data texture (replaces old gaussian SSBO).
        SDL_GPUTextureSamplerBinding gtex_bind = {};
        gtex_bind.texture = r->gaussian_texture;
        gtex_bind.sampler = r->gaussian_sampler;
        SDL_BindGPUVertexSamplers(pass, 0, &gtex_bind, 1);

        SDL_PushGPUVertexUniformData(cmd, 0, cam, sizeof(CameraUniforms));

        SDL_DrawGPUPrimitives(pass, 6, scene->visible_count, 0, 0);
    }

    // Equirectangular panorama overlay (skipped for the map pass).
    if (overlay && overlay->texture && overlay->alpha > 0.0f && r->overlay_pipeline) {
        SDL_BindGPUGraphicsPipeline(pass, r->overlay_pipeline);
        SDL_SetGPUStencilReference(pass, 1);

        SDL_GPUTextureSamplerBinding sampler_binding = {};
        sampler_binding.texture = overlay->texture;
        sampler_binding.sampler = r->overlay_sampler;
        SDL_BindGPUFragmentSamplers(pass, 0, &sampler_binding, 1);

        struct {
            float camera_ray_basis[16];
            float camera_tan_half_fov[2];
            float camera_pad[2];
            float ref_rotation[16];
            float alpha;
            float alpha_pad[3];
        } ov_uniforms;
        memcpy(ov_uniforms.camera_ray_basis, overlay->camera_ray_basis, sizeof(float) * 16);
        memcpy(ov_uniforms.camera_tan_half_fov, overlay->camera_tan_half_fov, sizeof(float) * 2);
        ov_uniforms.camera_pad[0] = ov_uniforms.camera_pad[1] = 0.0f;
        memcpy(ov_uniforms.ref_rotation, overlay->ref_rotation, sizeof(float) * 16);
        ov_uniforms.alpha = overlay->alpha;
        ov_uniforms.alpha_pad[0] = ov_uniforms.alpha_pad[1] = ov_uniforms.alpha_pad[2] = 0.0f;

        SDL_PushGPUFragmentUniformData(cmd, 0, &ov_uniforms, sizeof(ov_uniforms));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }

    // Wireframe node cubes
    if (nodes && nodes->count > 0 && r->wireframe_pipeline) {
        SDL_BindGPUGraphicsPipeline(pass, r->wireframe_pipeline);

        SDL_GPUBufferBinding vb_bind = {};
        vb_bind.buffer = r->cube_vertex_buffer;
        SDL_BindGPUVertexBuffers(pass, 0, &vb_bind, 1);

        SDL_GPUBufferBinding ib_bind = {};
        ib_bind.buffer = r->cube_index_buffer;
        SDL_BindGPUIndexBuffer(pass, &ib_bind, SDL_GPU_INDEXELEMENTSIZE_16BIT);

        float view_corrected[16];
        memcpy(view_corrected, cam->view, sizeof(view_corrected));
        view_corrected[0]  = -view_corrected[0];
        view_corrected[4]  = -view_corrected[4];
        view_corrected[8]  = -view_corrected[8];
        view_corrected[12] = -view_corrected[12];
        float vp[16];
        mat4_mul(cam->proj, view_corrected, vp);

        float scale = nodes->half_size * 2.0f;

        for (uint32_t i = 0; i < nodes->count; i++) {
            const float* p = &nodes->positions[i * 3];

            float model[16];
            mat4_translate_scale(p[0], p[1], p[2], scale, model);

            float mvp[16];
            mat4_mul(vp, model, mvp);

            struct { float mvp[16]; float color[4]; } uniforms;
            memcpy(uniforms.mvp, mvp, sizeof(mvp));
            uniforms.color[0] = 0.0f;
            uniforms.color[1] = 1.0f;
            uniforms.color[2] = 1.0f;
            uniforms.color[3] = 1.0f;

            SDL_PushGPUVertexUniformData(cmd, 0, &uniforms, sizeof(uniforms));
            SDL_DrawGPUIndexedPrimitives(pass, 24, 1, 0, 0, 0);
        }
    }
}

void renderer_draw_frame(Renderer* r, GaussianScene* scene, const CameraUniforms* cam, const OverlayParams* overlay, const NodeRenderParams* nodes, float wireframe_occlusion, const CameraUniforms* map_cam) {
    // Get current frame's transfer buffer and fence
    uint32_t buf_idx = r->current_frame % MAX_FRAMES_IN_FLIGHT;
    SDL_GPUTransferBuffer* transfer_buf = r->transfer_bufs[buf_idx];

    // Wait for this frame slot's previous work to complete before reusing its transfer buffer
    if (r->frame_fences[buf_idx] != NULL) {
        SDL_WaitForGPUFences(r->device, true, &r->frame_fences[buf_idx], 1);
        SDL_ReleaseGPUFence(r->device, r->frame_fences[buf_idx]);
        r->frame_fences[buf_idx] = NULL;
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(r->device);
    if (!cmd) return;

    // Upload sorted indices for the FPS pass
    upload_sorted_indices(r, scene, cmd, transfer_buf, false);

    // Prepare ImGui draw data (must be before render pass per backend requirement)
    ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);

    // Acquire swapchain
    SDL_GPUTexture* swapchain_tex = NULL;
    uint32_t sw_w = 0, sw_h = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, r->window, &swapchain_tex, &sw_w, &sw_h)) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    if (!swapchain_tex) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    // Recreate depth texture if swapchain dimensions changed
    if (sw_w != r->depth_w || sw_h != r->depth_h) {
        if (r->depth_texture) SDL_ReleaseGPUTexture(r->device, r->depth_texture);
        SDL_GPUTextureCreateInfo depth_tex_info = {};
        depth_tex_info.type = SDL_GPU_TEXTURETYPE_2D;
        depth_tex_info.format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
        depth_tex_info.width = sw_w;
        depth_tex_info.height = sw_h;
        depth_tex_info.layer_count_or_depth = 1;
        depth_tex_info.num_levels = 1;
        depth_tex_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        r->depth_texture = SDL_CreateGPUTexture(r->device, &depth_tex_info);
        r->depth_w = sw_w;
        r->depth_h = sw_h;
    }

    SDL_GPUColorTargetInfo color_target = {};
    color_target.texture = swapchain_tex;
    color_target.load_op = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op = SDL_GPU_STOREOP_STORE;
    color_target.clear_color.r = 0.1f;
    color_target.clear_color.g = 0.1f;
    color_target.clear_color.b = 0.1f;
    color_target.clear_color.a = 0.0f;

    SDL_GPUDepthStencilTargetInfo depth_target = {};
    depth_target.texture = r->depth_texture;
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.clear_depth = 1.0f;
    depth_target.clear_stencil = 0;
    depth_target.cycle = true;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, &depth_target);
    if (!pass) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    // Pass 1: FPS view (clears swapchain + depth, draws full world incl. overlay).
    draw_world(r, cmd, pass, scene, cam, overlay, nodes, wireframe_occlusion);

    // ImGui goes on the LAST pass. If no map overlay, that's this pass.
    if (!map_cam) {
        ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);
    }
    SDL_EndGPURenderPass(pass);

    // Pass 2 (optional): top-down map overlay. Loads the FPS color (so untouched
    // pixels stay), clears depth+stencil so we can re-render the world from above.
    //
    // NOTE: this redraws the FPS view in full every frame while the map is up.
    // A more efficient alternative ("option B") would be to render the FPS view
    // ONCE into an offscreen color texture the moment the map is toggled on,
    // then each subsequent frame just blit that texture into the swapchain
    // before the map pass. Cull/sort/mesh/overlay would all be skipped while
    // the map is active. Cost: one extra owned texture (resized with the
    // window) plus a one-shot capture flag and a swapchain blit. Worth doing
    // if the FPS pass becomes the bottleneck while the map is up.
    if (map_cam) {
        // Re-cull + re-sort splats from the map camera. mutates `scene`.
        cull_gaussians(scene, map_cam->view, map_cam->proj, map_cam->orthographic);
        if (scene->visible_count > 0) {
            SortContext sort_ctx = {};
            sort_ctx.depths = scene->visible_depths;
            sort_ctx.input_indices = scene->visible_indices;
            sort_ctx.count = scene->visible_count;
            sort_ctx.sorted_indices = scene->sorted_indices;
            sort_ctx.scratch_indices = scene->scratch_indices;
            sort_ctx.scratch_keys = scene->scratch_keys;
            sort_ctx.scratch_keys2 = scene->scratch_keys2;
            sort_gaussians(&sort_ctx);
        }
        // Second upload to r->index_buffer; cycle=true to safely reuse the
        // transfer buffer / index buffer within the same command buffer.
        upload_sorted_indices(r, scene, cmd, transfer_buf, true);

        SDL_GPUColorTargetInfo map_color_target = {};
        map_color_target.texture = swapchain_tex;
        map_color_target.load_op = SDL_GPU_LOADOP_LOAD;     // preserve FPS pixels
        map_color_target.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo map_depth_target = {};
        map_depth_target.texture = r->depth_texture;
        map_depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
        map_depth_target.store_op = SDL_GPU_STOREOP_DONT_CARE;
        map_depth_target.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
        map_depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        map_depth_target.clear_depth = 1.0f;
        map_depth_target.clear_stencil = 0;
        map_depth_target.cycle = true;

        SDL_GPURenderPass* map_pass = SDL_BeginGPURenderPass(cmd, &map_color_target, 1, &map_depth_target);
        if (map_pass) {
            // Dim the loaded FPS view first so the map content drawn next is
            // easier to read against the bright underlying frame.
            if (r->darken_pipeline) {
                SDL_BindGPUGraphicsPipeline(map_pass, r->darken_pipeline);
                // Animated fog needs a time uniform (seconds since startup).
                struct DarkenUniforms { float time, pad0, pad1, pad2; };
                DarkenUniforms du = { (float)SDL_GetTicks() * 0.001f, 0.0f, 0.0f, 0.0f };
                SDL_PushGPUFragmentUniformData(cmd, 0, &du, sizeof(du));
                SDL_DrawGPUPrimitives(map_pass, 3, 1, 0, 0); // fullscreen triangle
            }
            // No panorama overlay in map view; mesh + splats + wireframe only.
            draw_world(r, cmd, map_pass, scene, map_cam, NULL, nodes, wireframe_occlusion);
            ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, map_pass);
            SDL_EndGPURenderPass(map_pass);
        }
    }

    // Submit and acquire fence for this frame
    r->frame_fences[buf_idx] = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    r->current_frame++;
}


void renderer_destroy(Renderer* r) {
    // Wait for all in-flight frames to complete
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (r->frame_fences[i] != NULL) {
            SDL_WaitForGPUFences(r->device, true, &r->frame_fences[i], 1);
            SDL_ReleaseGPUFence(r->device, r->frame_fences[i]);
        }
    }
    
    if (r->splat_pipeline) SDL_ReleaseGPUGraphicsPipeline(r->device, r->splat_pipeline);
    if (r->overlay_pipeline) SDL_ReleaseGPUGraphicsPipeline(r->device, r->overlay_pipeline);
    if (r->darken_pipeline) SDL_ReleaseGPUGraphicsPipeline(r->device, r->darken_pipeline);
    if (r->wireframe_pipeline) SDL_ReleaseGPUGraphicsPipeline(r->device, r->wireframe_pipeline);
    if (r->mesh_pipeline) SDL_ReleaseGPUGraphicsPipeline(r->device, r->mesh_pipeline);
    if (r->overlay_sampler) SDL_ReleaseGPUSampler(r->device, r->overlay_sampler);
    if (r->gaussian_texture) SDL_ReleaseGPUTexture(r->device, r->gaussian_texture);
    if (r->gaussian_sampler) SDL_ReleaseGPUSampler(r->device, r->gaussian_sampler);
    if (r->cube_vertex_buffer) SDL_ReleaseGPUBuffer(r->device, r->cube_vertex_buffer);
    if (r->cube_index_buffer) SDL_ReleaseGPUBuffer(r->device, r->cube_index_buffer);
    mesh_gpu_release(r->device, &r->mesh_gpu);
    mesh_gpu_release(r->device, &r->object_gpu);
    if (r->mesh_sampler) SDL_ReleaseGPUSampler(r->device, r->mesh_sampler);
    if (r->index_buffer) SDL_ReleaseGPUBuffer(r->device, r->index_buffer);
    if (r->depth_texture) SDL_ReleaseGPUTexture(r->device, r->depth_texture);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (r->transfer_bufs[i]) SDL_ReleaseGPUTransferBuffer(r->device, r->transfer_bufs[i]);
    }
}
