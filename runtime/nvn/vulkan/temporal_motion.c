#include "temporal_motion.h"
#include "temporal_viewport.h"
#include "shaders.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { MOTION_GROUP = 8, MOTION_MAX_EXTENT = 32768 };

typedef struct {
    float rows[4][4];
    float viewport[2], max_error;
    uint32_t max_span;
    float current_jitter[2], prior_jitter[2];
} MotionProjection;

_Static_assert(sizeof(MotionProjection) == 96, "motion push constant layout");

static const char motion_glsl[] =
    "#version 450\n"
    "layout(local_size_x=8, local_size_y=8) in;\n"
    "layout(set=0,binding=0) uniform sampler2D current_depth;\n"
    "layout(set=0,binding=1) uniform sampler2D prior_depth;\n"
    "layout(set=0,binding=2) uniform usampler2D current_owner;\n"
    "layout(set=0,binding=3) uniform usampler2D prior_owner;\n"
    "layout(rgba16f,set=0,binding=4) uniform writeonly image2D velocity;\n"
    "layout(push_constant) uniform Projection {\n"
    "    vec4 rows[4];\n"
    "    vec2 viewport;\n"
    "    float max_error;\n"
    "    uint max_span;\n"
    "    vec2 current_jitter;\n"
    "    vec2 prior_jitter;\n"
    "} pc;\n"
    "const uint D24_MAX = 0xffffffu;\n"
    "uint depth_at(sampler2D tex, ivec2 p) {\n"
    "    return uint(roundEven(texelFetch(tex,p,0).r * float(D24_MAX)));\n"
    "}\n"
    "uint owner_at(usampler2D owners, ivec2 p) { return texelFetch(owners,p,0).r; }\n"
    "vec4 project_motion(ivec2 p, ivec2 extent) {\n"
    "    if (any(lessThan(p,ivec2(1))) || any(greaterThanEqual(p,extent-1))) return vec4(0);\n"
    "    uint owner = owner_at(current_owner,p);\n"
    "    if (owner == 0u) return vec4(0);\n"
    "    uint d = depth_at(current_depth,p), dlo = d, dhi = d;\n"
    "    const ivec2 neighbors[4] = ivec2[](ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));\n"
    "    for (int i=0; i<4; ++i) {\n"
    "        ivec2 n = p + neighbors[i];\n"
    "        if (owner_at(current_owner,n) != owner) return vec4(0);\n"
    "        uint nd = depth_at(current_depth,n);\n"
    "        dlo = min(dlo,nd); dhi = max(dhi,nd);\n"
    "    }\n"
    "    if (dhi == D24_MAX || dhi-dlo > pc.max_span) return vec4(0);\n"
    "    vec2 center = vec2(p) + 0.5 - pc.current_jitter;\n"
    "    vec4 c = vec4(2.0*center/pc.viewport-1.0,float(d)/float(D24_MAX),1.0);\n"
    "    vec4 delta = vec4(dot(pc.rows[0],c),dot(pc.rows[1],c),dot(pc.rows[2],c),dot(pc.rows[3],c));\n"
    "    vec4 q = c + delta;\n"
    "    if (q.w <= 0.0 || any(isnan(q)) || any(isinf(q))) return vec4(0);\n"
    "    float prior_z = q.z / q.w;\n"
    "    vec2 shift = (delta.xy - c.xy*delta.w) * (pc.viewport*0.5 / q.w);\n"
    "    vec2 xy = center + shift + pc.prior_jitter;\n"
    "    if (prior_z < 0.0 || prior_z > 1.0 || any(isnan(xy)) || any(isinf(xy)) ||\n"
    "        any(lessThan(xy,vec2(0.5))) || any(greaterThanEqual(xy,vec2(extent)-0.5))) return vec4(0);\n"
    "    ivec2 b = ivec2(floor(xy-0.5));\n"
    "    const ivec2 x = ivec2(1,0), y = ivec2(0,1);\n"
    "    if (owner_at(prior_owner,b) != owner || owner_at(prior_owner,b+x) != owner ||\n"
    "        owner_at(prior_owner,b+y) != owner || owner_at(prior_owner,b+1) != owner) return vec4(0);\n"
    "    uvec4 depths = uvec4(depth_at(prior_depth,b),depth_at(prior_depth,b+x),\n"
    "                         depth_at(prior_depth,b+y),depth_at(prior_depth,b+1));\n"
    "    uint min_d = min(min(depths.x,depths.y),min(depths.z,depths.w));\n"
    "    uint max_d = max(max(depths.x,depths.y),max(depths.z,depths.w));\n"
    "    if (max_d == D24_MAX || max_d-min_d > pc.max_span) return vec4(0);\n"
    "    vec2 t = xy - 0.5 - vec2(b);\n"
    "    // Keep the interpolated depth near zero to preserve sub-code precision.\n"
    "    vec4 local = vec4(ivec4(depths)-int(min_d));\n"
    "    float interpolated = mix(mix(local.x,local.y,t.x),mix(local.z,local.w,t.x),t.y);\n"
    "    if (abs(interpolated-(prior_z*float(D24_MAX)-float(min_d))) > pc.max_error) return vec4(0);\n"
    "    return vec4(shift,0,1);\n"
    "}\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_GlobalInvocationID.xy), extent = imageSize(velocity);\n"
    "    if (any(greaterThanEqual(p,extent))) return;\n"
    "    if (any(notEqual(textureSize(current_depth,0),extent)) || any(notEqual(textureSize(prior_depth,0),extent)) ||\n"
    "        any(notEqual(textureSize(current_owner,0),extent)) || any(notEqual(textureSize(prior_owner,0),extent))) {\n"
    "        imageStore(velocity,p,vec4(0));\n"
    "        return;\n"
    "    }\n"
    "    imageStore(velocity,p,project_motion(p,extent));\n"
    "}\n";

static bool supports_format(VkFormat format, VkFormatFeatureFlags flags) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, format, &props);
    return (props.optimalTilingFeatures & flags) == flags;
}

void vk_temporal_motion_destroy(VkTemporalMotion *motion) {
    if (!motion || !motion->device) return;
    if (motion->pipeline) vkDestroyPipeline(motion->device, motion->pipeline, NULL);
    if (motion->layout) vkDestroyPipelineLayout(motion->device, motion->layout, NULL);
    if (motion->set_layout) vkDestroyDescriptorSetLayout(motion->device, motion->set_layout, NULL);
    if (motion->sampler) vkDestroySampler(motion->device, motion->sampler, NULL);
    *motion = (VkTemporalMotion){0};
}

bool vk_temporal_motion_init(VkTemporalMotion *motion) {
    if (!motion || !g_vk.dev || !g_vk.phys) return false;
    if (motion->device) return motion->device == g_vk.dev && motion->pipeline;
    const VkPhysicalDeviceLimits *limits = &g_vk.props.limits;
    if (limits->maxPushConstantsSize < sizeof(MotionProjection) ||
        limits->maxPerStageDescriptorSamplers < 4 || limits->maxPerStageDescriptorSampledImages < 4 ||
        limits->maxPerStageDescriptorStorageImages < 1 ||
        limits->maxComputeWorkGroupInvocations < MOTION_GROUP * MOTION_GROUP ||
        limits->maxComputeWorkGroupSize[0] < MOTION_GROUP || limits->maxComputeWorkGroupSize[1] < MOTION_GROUP ||
        !supports_format(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !supports_format(VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !supports_format(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;

    VkTemporalMotion next = {.device = g_vk.dev};
    const VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkResult result = vkCreateSampler(next.device, &sampler, NULL, &next.sampler);
    VkDescriptorSetLayoutBinding bindings[5];
    for (uint32_t i = 0; i < 5; i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorCount = 1,
            .descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    const VkDescriptorSetLayoutCreateInfo sets = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 5, .pBindings = bindings};
    if (result == VK_SUCCESS) result = vkCreateDescriptorSetLayout(next.device, &sets, NULL, &next.set_layout);
    const VkPushConstantRange constants = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(MotionProjection)};
    const VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &next.set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &constants};
    if (result == VK_SUCCESS) result = vkCreatePipelineLayout(next.device, &layout, NULL, &next.layout);
    if (result == VK_SUCCESS) {
        size_t words;
        uint32_t *spirv = vk_glsl_to_spirv(motion_glsl, STAGE_COMPUTE, "temporal_motion", &words);
        VkShaderModule shader = VK_NULL_HANDLE;
        if (spirv) {
            const VkShaderModuleCreateInfo module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = words * sizeof *spirv, .pCode = spirv};
            result = vkCreateShaderModule(next.device, &module, NULL, &shader);
            free(spirv);
        } else result = VK_ERROR_INITIALIZATION_FAILED;
        if (result == VK_SUCCESS) {
            const VkComputePipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                          .module = shader, .pName = "main"}, .layout = next.layout};
            result = vkCreateComputePipelines(next.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &next.pipeline);
            vkDestroyShaderModule(next.device, shader, NULL);
        }
    }
    if (result != VK_SUCCESS) {
        vk_temporal_motion_destroy(&next);
        return false;
    }
    *motion = next;
    return true;
}

// Partial-pivot inversion also rejects singular or excessively ill-conditioned input.
static bool invert_projection(const float matrix[4][4], double inverse[4][4]) {
    double a[4][8], scale = 0;
    for (unsigned r = 0; r < 4; r++) {
        for (unsigned c = 0; c < 4; c++) {
            const double value = matrix[r][c];
            if (!isfinite(value)) return false;
            a[r][c] = value;
            a[r][c + 4] = r == c;
            scale = fmax(scale, fabs(value));
        }
    }
    for (unsigned c = 0; c < 4; c++) {
        unsigned pivot = c;
        for (unsigned r = c + 1; r < 4; r++)
            if (fabs(a[r][c]) > fabs(a[pivot][c])) pivot = r;
        if (fabs(a[pivot][c]) <= scale * 1e-12) return false;
        for (unsigned i = 0; i < 8; i++) {
            double v = a[pivot][i];
            a[pivot][i] = a[c][i];
            a[c][i] = v;
        }
        const double divisor = a[c][c];
        for (unsigned i = 0; i < 8; i++) a[c][i] /= divisor;
        for (unsigned r = 0; r < 4; r++) {
            if (r == c) continue;
            const double factor = a[r][c];
            for (unsigned i = 0; i < 8; i++) a[r][i] -= factor * a[c][i];
        }
    }
    for (unsigned r = 0; r < 4; r++)
        memcpy(inverse[r], a[r] + 4, sizeof inverse[r]);
    return true;
}

// Calculate prior * inverse(current) - identity in double precision so small
// camera changes survive conversion to the shader's float push constants.
static bool relative_projection(const VkTemporalMotionInput *in, MotionProjection *out) {
    double inverse[4][4];
    if (!invert_projection(in->current_world_to_clip, inverse)) return false;
    if (!memcmp(in->current_world_to_clip, in->prior_world_to_clip, sizeof in->current_world_to_clip)) return true;
    double prior_inverse[4][4];
    if (!invert_projection(in->prior_world_to_clip, prior_inverse)) return false;
    for (unsigned r = 0; r < 4; r++) {
        for (unsigned c = 0; c < 4; c++) {
            double value = -(r == c);
            for (unsigned k = 0; k < 4; k++) value += in->prior_world_to_clip[r][k] * inverse[k][c];
            // A much larger relative transform cannot represent eligible history.
            if (!isfinite(value) || fabs(value) > 1e6) return false;
            out->rows[r][c] = (float)value;
        }
    }
    return true;
}

bool vk_temporal_motion_dispatch(const VkTemporalMotion *motion, VkCommandBuffer cb, VkDescriptorSet set,
                                 const VkTemporalMotionInput *in) {
    if (!motion || motion->device != g_vk.dev || !motion->pipeline || !cb || !set || !in ||
        !in->current_depth || !in->prior_depth || !in->current_owner || !in->prior_owner || !in->output) return false;
    const VkExtent2D extent = in->extent;
    const VkViewport v = in->viewport;
    if (extent.width < 3 || extent.height < 3 || extent.width > MOTION_MAX_EXTENT || extent.height > MOTION_MAX_EXTENT ||
        !temporal_full_viewport(v, extent)) return false;
    const uint32_t x = (extent.width + MOTION_GROUP - 1) / MOTION_GROUP;
    const uint32_t y = (extent.height + MOTION_GROUP - 1) / MOTION_GROUP;
    if (x > g_vk.props.limits.maxComputeWorkGroupCount[0] || y > g_vk.props.limits.maxComputeWorkGroupCount[1]) return false;
    MotionProjection projection = {.viewport = {v.width, -v.height}, .max_error = 8, .max_span = 65536};
    for (unsigned axis = 0; axis < 2; axis++) {
        if (!(fabsf(in->current_jitter[axis]) <= 0.5f && fabsf(in->prior_jitter[axis]) <= 0.5f)) return false;
        projection.current_jitter[axis] = in->current_jitter[axis];
        projection.prior_jitter[axis] = in->prior_jitter[axis];
    }
    if (!relative_projection(in, &projection)) return false;

    const VkImageView views[5] = {in->current_depth, in->prior_depth, in->current_owner, in->prior_owner, in->output};
    VkDescriptorImageInfo images[5];
    VkWriteDescriptorSet writes[5];
    for (uint32_t i = 0; i < 5; i++) {
        images[i] = (VkDescriptorImageInfo){.sampler = i == 4 ? VK_NULL_HANDLE : motion->sampler,
            .imageView = views[i], .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
            .dstBinding = i, .descriptorCount = 1, .pImageInfo = &images[i],
            .descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    }
    vkUpdateDescriptorSets(g_vk.dev, 5, writes, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, motion->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, motion->layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, motion->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof projection, &projection);
    vkCmdDispatch(cb, x, y, 1);
    return true;
}
