#pragma once
#include <stdlib.h>
#include <vulkan/vulkan.h>

// The Windows present shader writes already encoded SDR into a UNORM attachment.
// ImGui's pipelines retain the original attachment format across swapchain recreation.
static inline VkResult vk_query_sdr_surface_format(
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR enumerate, VkPhysicalDevice device,
    VkSurfaceKHR surface, VkFormat existing, VkSurfaceFormatKHR *selected)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint32_t capacity = 0;
        VkResult result = enumerate(device, surface, &capacity, NULL);
        if (result == VK_INCOMPLETE) continue;
        if (result != VK_SUCCESS) return result;
        if (!capacity) return VK_ERROR_FORMAT_NOT_SUPPORTED;

        VkSurfaceFormatKHR *formats = calloc(capacity, sizeof *formats);
        if (!formats) return VK_ERROR_OUT_OF_HOST_MEMORY;
        uint32_t count = capacity;
        result = enumerate(device, surface, &count, formats);
        if (result == VK_SUCCESS && count > capacity) result = VK_ERROR_FORMAT_NOT_SUPPORTED;
        if (result == VK_SUCCESS) {
            VkFormat preferred = existing == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : existing;
            VkFormat fallback = existing == VK_FORMAT_UNDEFINED ? VK_FORMAT_R8G8B8A8_UNORM : existing;
            VkFormat best = VK_FORMAT_UNDEFINED;
            for (uint32_t i = 0; i < count; ++i) {
                if (formats[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
                if (formats[i].format == preferred) {
                    best = preferred;
                    break;
                }
                if (formats[i].format == fallback) best = fallback;
            }
            if (best != VK_FORMAT_UNDEFINED)
                *selected = (VkSurfaceFormatKHR){best, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
            else
                result = VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        free(formats);
        if (result != VK_INCOMPLETE) return result;
    }
    return VK_INCOMPLETE;
}
