#pragma once

#include "vk.h"

typedef struct {
    VkDevice device;
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
} VkTemporalMotion;

typedef struct {
    // All five views are single-sampled 2D images of extent, in GENERAL.
    // Depth views expose only the normalized D24 aspect of D24_UNORM_S8_UINT.
    VkImageView current_depth, prior_depth;
    // R32_UINT. Zero is invalid. Each nonzero token denotes one independently
    // verified static owner, unambiguous and stable across this frame pair.
    // The current cross and prior interpolation quad must share the token.
    // Share views only when the caller verified ownership for both frames.
    VkImageView current_owner, prior_owner;
    // R16G16B16A16_SFLOAT storage view. XY points from current to prior in
    // render pixels (right and down positive), Z is zero, A is 1 for valid.
    // Rejected pixels get four zero channels. Output must not alias inputs.
    VkImageView output;
    VkExtent2D extent;
    // Same effective viewport in both frames. Full-origin, negative height,
    // depth range [0,1]. Fractional dimensions must round to the extent.
    VkViewport viewport;
    // Unjittered row-major guest projections before the vertex shader negates clip Y.
    float current_world_to_clip[4][4];
    float prior_world_to_clip[4][4];
    // Actual offsets applied to the depth and owner passes, in render pixels
    // (right and down positive). Each axis is within [-0.5, 0.5]. Motion excludes
    // these offsets. Use zero for an unjittered frame.
    float current_jitter[2], prior_jitter[2];
} VkTemporalMotionInput;

// Initialize a zeroed owner on the render thread before first use. May compile
// shaders. A false result leaves the owner zeroed or on its original device.
bool vk_temporal_motion_init(VkTemporalMotion *motion);
// Record on the renderer's current command buffer outside rendering. The
// caller establishes input/output visibility, retains views and images through
// completion, and synchronizes subsequent output readers. Supply an unused
// descriptor set from set_layout and retain it through completion as well.
// The caller supplies eligible consecutive frames with matching viewports and
// exactly matching physical extents. False records no work and leaves output
// untouched, so the caller must not consume its contents as current motion.
bool vk_temporal_motion_dispatch(const VkTemporalMotion *motion, VkCommandBuffer cb, VkDescriptorSet set,
                                 const VkTemporalMotionInput *input);
// Release after all commands that use the owner have completed.
void vk_temporal_motion_destroy(VkTemporalMotion *motion);
