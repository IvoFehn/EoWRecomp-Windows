#pragma once

#include <stdbool.h>
#include <vulkan/vulkan.h>

// Scene images use lroundf on the scaled width and height. The viewport
// preserves the fractional span for projection and motion-vector math.
static inline bool temporal_full_viewport(VkViewport viewport, VkExtent2D extent) {
    return extent.width && extent.height && viewport.x == 0 && viewport.y == -viewport.height &&
           viewport.minDepth == 0 && viewport.maxDepth == 1 &&
           viewport.width >= extent.width - 0.5f && viewport.width < extent.width + 0.5f &&
           -viewport.height >= extent.height - 0.5f && -viewport.height < extent.height + 0.5f;
}
