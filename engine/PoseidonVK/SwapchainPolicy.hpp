#pragma once

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Poseidon::vk
{
// These decisions do not query a driver or require a window.
inline VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, uint32_t width, uint32_t height)
{
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
        return caps.currentExtent;
    return {std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height)};
}

inline uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& caps)
{
    const uint32_t desired =
        caps.minImageCount == std::numeric_limits<uint32_t>::max() ? caps.minImageCount : caps.minImageCount + 1;
    return caps.maxImageCount ? std::min(desired, caps.maxImageCount) : desired;
}

inline VkSurfaceFormatKHR ChooseFormat(const std::vector<VkSurfaceFormatKHR>& formats)
{
    if (formats.empty())
        throw std::runtime_error("Vulkan: surface has no formats");
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
        return {VK_FORMAT_B8G8R8A8_UNORM, formats[0].colorSpace};
    for (const auto& format : formats)
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;
    return formats.front();
}

inline VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes, int interval)
{
    const auto has = [&](VkPresentModeKHR mode) { return std::find(modes.begin(), modes.end(), mode) != modes.end(); };
    if (interval == -1 && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    if (interval == 0)
    {
        if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            return VK_PRESENT_MODE_MAILBOX_KHR;
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR; // Required by Vulkan, including the fallback for adaptive/off.
}

inline VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported)
{
    for (auto alpha : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                       VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
        if (supported & alpha)
            return alpha;
    throw std::runtime_error("Vulkan: surface has no composite alpha mode");
}

struct QueueFamilies
{
    static constexpr uint32_t Missing = std::numeric_limits<uint32_t>::max();
    uint32_t graphics = Missing;
    uint32_t present = Missing;
    bool Complete() const { return graphics != Missing && present != Missing; }
};

inline QueueFamilies ChooseQueues(const std::vector<VkQueueFamilyProperties>& families,
                                  const std::vector<VkBool32>& present)
{
    QueueFamilies result;
    for (uint32_t i = 0; i < families.size() && i < present.size(); ++i)
    {
        if (!families[i].queueCount)
            continue;
        const bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (graphics && present[i])
            return {i, i}; // Prefer one family; otherwise use concurrent swapchain sharing.
        if (graphics && result.graphics == QueueFamilies::Missing)
            result.graphics = i;
        if (present[i] && result.present == QueueFamilies::Missing)
            result.present = i;
    }
    return result;
}

inline bool NeedsRecreation(VkResult result)
{
    return result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
}
} // namespace Poseidon::vk
