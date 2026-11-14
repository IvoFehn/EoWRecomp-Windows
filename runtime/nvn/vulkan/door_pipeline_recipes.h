#pragma once

// Exact dynamic-rendering states observed at the Suthorn Ruins entrance.
// Stable stage keys only; the SPIR-V is built separately from local game data.
#define DOOR_PIPELINE_BASE \
    .depth = VK_FORMAT_D24_UNORM_S8_UINT, .stencil = VK_FORMAT_D24_UNORM_S8_UINT, \
    .samples = VK_SAMPLE_COUNT_1_BIT, .topo_class = 2, .poly_fill = 1
#define DOOR_PIPELINE_COLOR \
    .color = {VK_FORMAT_B10G11R11_UFLOAT_PACK32}, .ncolor = 1, \
    .write_mask = {VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT}
// The renderer stores R8_UNORM in the unused color slot for depth-only work.
// Preserve that value for the exact state comparison.
#define DOOR_PIPELINE_DEPTH_ONLY .color = {VK_FORMAT_R8_UNORM}
#define DOOR_PIPELINE_UNSKINNED(format6_offset, vertex_stride) \
    .attr_fmt = {[0] = VK_FORMAT_R32G32B32_SFLOAT, [6] = VK_FORMAT_A2B10G10R10_SNORM_PACK32, \
                 [7] = VK_FORMAT_R32G32B32A32_SFLOAT, [8] = VK_FORMAT_R32G32B32A32_SFLOAT}, \
    .attr_off = {[6] = (format6_offset)}, .attr_bind = {[7] = ZERO_BINDING, [8] = ZERO_BINDING}, \
    .attr_mask = 0x1c1, .stride = {(vertex_stride)}, .used_streams = (1u << ZERO_BINDING) | 1u
#define DOOR_PIPELINE_SKINNED(format7, format8, offset7, offset8, vertex_stride) \
    .attr_fmt = {[0] = VK_FORMAT_R32G32B32_SFLOAT, [6] = VK_FORMAT_A2B10G10R10_SNORM_PACK32, \
                 [7] = (format7), [8] = (format8)}, \
    .attr_off = {[6] = 12, [7] = (offset7), [8] = (offset8)}, \
    .attr_mask = 0x1c1, .stride = {(vertex_stride)}, .used_streams = 1
#define DOOR_PIPELINE_WORLD(uv_format, uv_offset, vertex_stride) \
    .attr_fmt = {[0] = VK_FORMAT_R32G32B32_SFLOAT, [1] = (uv_format), [6] = VK_FORMAT_A2B10G10R10_SNORM_PACK32, \
                 [7] = VK_FORMAT_R32G32B32A32_SFLOAT, [8] = VK_FORMAT_R32G32B32A32_SFLOAT, \
                 [9] = VK_FORMAT_A2B10G10R10_SNORM_PACK32}, \
    .attr_off = {[1] = (uv_offset), [6] = 12, [9] = 16}, .attr_bind = {[7] = ZERO_BINDING, [8] = ZERO_BINDING}, \
    .attr_mask = 0x3c3, .stride = {(vertex_stride)}, .used_streams = (1u << ZERO_BINDING) | 1u

static const PipelineRecipe door_pipeline_recipes[] = {
    {
        .shader = {UINT64_C(0x07ec3c0dded0f000), UINT64_C(0xde82533938b7dda3)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_COLOR, DOOR_PIPELINE_UNSKINNED(20, 36)},
    }, {
        .shader = {UINT64_C(0x07ec3c0dded0f000), UINT64_C(0xb79cff78b6631a8a)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_COLOR, DOOR_PIPELINE_UNSKINNED(12, 28)},
    }, {
        .shader = {UINT64_C(0x07ec3c0dded0f000), UINT64_C(0x25453b17dc907877)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_COLOR, DOOR_PIPELINE_UNSKINNED(12, 28)},
    }, {
        .shader = {UINT64_C(0x07ec3c0dded0f000), UINT64_C(0xb79cff78b6631a8a)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_COLOR, DOOR_PIPELINE_UNSKINNED(12, 36)},
    }, {
        .shader = {UINT64_C(0x07ec3c0dded0f000), UINT64_C(0xb79cff78b6631a8a)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_COLOR, DOOR_PIPELINE_UNSKINNED(12, 32)},
    }, {
        .shader = {UINT64_C(0x07ec3c0dded0f000), UINT64_C(0xde82533938b7dda3)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_DEPTH_ONLY, DOOR_PIPELINE_UNSKINNED(20, 36)},
    }, {
        .shader = {UINT64_C(0x07ec3c0dded0f000), UINT64_C(0xb79cff78b6631a8a)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_DEPTH_ONLY, DOOR_PIPELINE_UNSKINNED(12, 28)},
    }, {
        .shader = {UINT64_C(0x07ec3c0dded0f000), UINT64_C(0x25453b17dc907877)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_DEPTH_ONLY, DOOR_PIPELINE_UNSKINNED(12, 28)},
    }, {
        .shader = {UINT64_C(0x07ec3c0dded0f000), UINT64_C(0xb79cff78b6631a8a)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_DEPTH_ONLY, DOOR_PIPELINE_UNSKINNED(12, 36)},
    }, {
        .shader = {UINT64_C(0x07ec3c0dded0f000), UINT64_C(0xb79cff78b6631a8a)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_DEPTH_ONLY, DOOR_PIPELINE_UNSKINNED(12, 32)},
    }, {
        .shader = {UINT64_C(0x45a8bbba7c7d6771), UINT64_C(0x2a078c7823019024)},
        .state = {
            DOOR_PIPELINE_BASE, DOOR_PIPELINE_DEPTH_ONLY,
            .attr_fmt = {[0] = VK_FORMAT_R32G32B32_SFLOAT, [6] = VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                         [7] = VK_FORMAT_R8_UINT, [8] = VK_FORMAT_R32G32B32A32_SFLOAT},
            .attr_off = {[6] = 12, [7] = 28}, .attr_bind = {[8] = ZERO_BINDING},
            .attr_mask = 0x1c1, .stride = {32}, .used_streams = (1u << ZERO_BINDING) | 1u,
        },
    }, {
        .shader = {UINT64_C(0x45a8bbba7c7d6771), UINT64_C(0x2a078c7823019024)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_DEPTH_ONLY,
                  DOOR_PIPELINE_SKINNED(VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_UNORM, 28, 30, 32)},
    }, {
        .shader = {UINT64_C(0x45a8bbba7c7d6771), UINT64_C(0x2a078c7823019024)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_DEPTH_ONLY,
                  DOOR_PIPELINE_SKINNED(VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UNORM, 28, 32, 36)},
    }, {
        .shader = {UINT64_C(0x45a8bbba7c7d6771), UINT64_C(0xb79cff78b6631a8a)},
        .state = {
            DOOR_PIPELINE_BASE, DOOR_PIPELINE_DEPTH_ONLY,
            .attr_fmt = {[0] = VK_FORMAT_R32G32B32_SFLOAT, [6] = VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                         [7] = VK_FORMAT_R8_UINT, [8] = VK_FORMAT_R32G32B32A32_SFLOAT},
            .attr_off = {[6] = 12, [7] = 32}, .attr_bind = {[8] = ZERO_BINDING},
            .attr_mask = 0x1c1, .stride = {36}, .used_streams = (1u << ZERO_BINDING) | 1u,
        },
    }, {
        .shader = {UINT64_C(0x211326bbc732fc19), UINT64_C(0x3d9a3b393110da09)},
        .state = {
            DOOR_PIPELINE_BASE, DOOR_PIPELINE_COLOR,
            .attr_fmt = {[0] = VK_FORMAT_R32G32B32_SFLOAT, [1] = VK_FORMAT_R16G16_SNORM,
                         [6] = VK_FORMAT_A2B10G10R10_SNORM_PACK32, [7] = VK_FORMAT_R32G32B32A32_SFLOAT,
                         [8] = VK_FORMAT_R32G32B32A32_SFLOAT, [9] = VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                         [10] = VK_FORMAT_R16G16B16A16_SFLOAT},
            .attr_off = {[1] = 32, [6] = 20, [9] = 24, [10] = 12}, .attr_bind = {[7] = ZERO_BINDING, [8] = ZERO_BINDING},
            .attr_mask = 0x7c3, .stride = {36}, .used_streams = (1u << ZERO_BINDING) | 1u,
        },
    }, {
        .shader = {UINT64_C(0x1edb5ae831683611), UINT64_C(0x7928729ea662a712)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_COLOR, DOOR_PIPELINE_WORLD(VK_FORMAT_R16G16_SNORM, 28, 36)},
    }, {
        .shader = {UINT64_C(0x1edb5ae831683611), UINT64_C(0x7928729ea662a712)},
        .state = {DOOR_PIPELINE_BASE, DOOR_PIPELINE_COLOR, DOOR_PIPELINE_WORLD(VK_FORMAT_R16G16_UNORM, 24, 32)},
    },
};

// Separate opt-in control. Exact native state 358, outside the default seed.
static const PipeState door_vegetation_state = {
    DOOR_PIPELINE_BASE, DOOR_PIPELINE_COLOR,
    .attr_fmt = {[0] = VK_FORMAT_R32G32B32_SFLOAT, [1] = VK_FORMAT_R16G16_UNORM,
                 [6] = VK_FORMAT_A2B10G10R10_SNORM_PACK32, [7] = VK_FORMAT_R8_UINT,
                 [8] = VK_FORMAT_R32G32B32A32_SFLOAT, [9] = VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                 [10] = VK_FORMAT_R8G8B8A8_UNORM},
    .attr_off = {[1] = 28, [6] = 12, [7] = 32, [9] = 16, [10] = 24},
    .attr_bind = {[8] = ZERO_BINDING}, .attr_mask = 0x7c3, .stride = {36},
    .used_streams = (1u << ZERO_BINDING) | 1u,
};

#define DOOR_PIPELINE_VEGETATION_AUX \
    .attr_fmt = {[0] = VK_FORMAT_R32G32B32_SFLOAT, [6] = VK_FORMAT_A2B10G10R10_SNORM_PACK32, \
                 [7] = VK_FORMAT_R8_UINT, [8] = VK_FORMAT_R32G32B32A32_SFLOAT, [10] = VK_FORMAT_R8G8B8A8_UNORM}, \
    .attr_off = {[6] = 12, [7] = 32, [10] = 24}, .attr_bind = {[8] = ZERO_BINDING}, \
    .attr_mask = 0x5c1, .stride = {36}, .used_streams = (1u << ZERO_BINDING) | 1u

// Exact native state 330 writes the main color attachment. State 333 has depth/stencil only.
static const PipeState door_vegetation_depth_states[] = {
    {DOOR_PIPELINE_BASE, DOOR_PIPELINE_COLOR, DOOR_PIPELINE_VEGETATION_AUX},
    {DOOR_PIPELINE_BASE, DOOR_PIPELINE_DEPTH_ONLY, DOOR_PIPELINE_VEGETATION_AUX},
};

#undef DOOR_PIPELINE_VEGETATION_AUX

#undef DOOR_PIPELINE_WORLD
#undef DOOR_PIPELINE_SKINNED
#undef DOOR_PIPELINE_UNSKINNED
#undef DOOR_PIPELINE_DEPTH_ONLY
#undef DOOR_PIPELINE_COLOR
#undef DOOR_PIPELINE_BASE
