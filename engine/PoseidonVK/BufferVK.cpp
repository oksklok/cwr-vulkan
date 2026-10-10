// Adapted from koosoli/PoseidonVK 7523bd5; see BufferVK.hpp and docs/vulkan-foundation.md.
#include <PoseidonVK/BufferVK.hpp>
#include <cstring>
#include <stdexcept>

namespace Poseidon::vk
{
VkResult CreateHostVisibleBuffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                                 VkBufferUsageFlags usage, BufferVK& out)
{
    // Do not silently replace a buffer which may still be in flight.
    if (!physical || !device || !size || !usage || out.buffer || out.memory || out.mapped)
        return VK_ERROR_INITIALIZATION_FAILED;
    out.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(device, &info, nullptr, &out.buffer);
    if (result == VK_SUCCESS)
    {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        const uint32_t type =
            FindMemoryType(memory, requirements.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX)
            result = VK_ERROR_FEATURE_NOT_PRESENT;
        else
        {
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = type;
            result = vkAllocateMemory(device, &allocation, nullptr, &out.memory);
        }
    }
    if (result == VK_SUCCESS)
        result = vkBindBufferMemory(device, out.buffer, out.memory, 0);
    if (result == VK_SUCCESS)
    {
        void* mapped = nullptr;
        result = vkMapMemory(device, out.memory, 0, size, 0, &mapped);
        if (result == VK_SUCCESS)
            out.mapped = mapped;
    }
    if (result != VK_SUCCESS)
        DestroyBuffer(device, out);
    return result;
}

void UploadMappedBuffer(const BufferVK& buffer, const void* data, size_t size, VkDeviceSize offset)
{
    if (!buffer.mapped || !data || offset > buffer.size || size > buffer.size - offset)
        throw std::invalid_argument("Vulkan: invalid or oversized mapped-buffer upload");
    std::memcpy(static_cast<char*>(buffer.mapped) + offset, data, size);
}

void DestroyBuffer(VkDevice device, BufferVK& buffer) noexcept
{
    if (device && buffer.mapped)
        vkUnmapMemory(device, buffer.memory);
    if (device && buffer.buffer)
        vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (device && buffer.memory)
        vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}
} // namespace Poseidon::vk
