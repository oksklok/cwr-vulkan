#pragma once

// Adapted from koosoli/PoseidonVK 7523bd5, BufferVK.hpp/.cpp.
// GPL-3.0-or-later with the additional terms in this repository's LICENSE.
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>

namespace Poseidon::vk
{
struct BufferVK
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

// The owning context must wait for all referencing submissions before destruction.
inline uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties& memory, uint32_t mask,
                               VkMemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

VkResult CreateHostVisibleBuffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                                 VkBufferUsageFlags usage, BufferVK& out);
void UploadMappedBuffer(const BufferVK& buffer, const void* data, size_t size);
void DestroyBuffer(VkDevice device, BufferVK& buffer) noexcept;
} // namespace Poseidon::vk
