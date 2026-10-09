// Deterministic buffer allocation/failure tests: these entry points never reach a driver.
#include <PoseidonVK/BufferVK.hpp>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace
{
int failAt = 0, step = 0, destroys = 0, frees = 0, unmaps = 0;
std::array<unsigned char, 256> mappedBytes{};
template <class T>
T Handle(uintptr_t value)
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}
VkResult Next()
{
    return ++step == failAt ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
} // namespace

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* p)
{
    *p = {};
    p->memoryTypeCount = 2;
    p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*,
                                              VkBuffer* out)
{
    const auto result = Next();
    if (result == VK_SUCCESS)
        *out = Handle<VkBuffer>(2);
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements* out)
{
    *out = {256, 16, 2};
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*,
                                                VkDeviceMemory* out)
{
    const auto result = Next();
    if (result == VK_SUCCESS)
        *out = Handle<VkDeviceMemory>(3);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)
{
    return Next();
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags,
                                           void** out)
{
    const auto result = Next();
    if (result == VK_SUCCESS)
        *out = mappedBytes.data();
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory)
{
    ++unmaps;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*)
{
    ++destroys;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*)
{
    ++frees;
}

int TestVulkanBuffers()
{
    using namespace Poseidon::vk;
    int checks = 0;
    const auto check = [&](bool condition, const char* message)
    {
        ++checks;
        if (!condition)
            throw std::runtime_error(message);
    };
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(VK_NULL_HANDLE, &memory);
    check(FindMemoryType(memory, 2, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 1,
          "coherent host upload must select an allowed memory type");
    check(FindMemoryType(memory, 1, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == UINT32_MAX,
          "unsupported memory masks must fail explicitly");
    const auto physical = Handle<VkPhysicalDevice>(1);
    const auto device = Handle<VkDevice>(1);
    for (int failedStep = 0; failedStep <= 4; ++failedStep)
    {
        failAt = failedStep;
        step = destroys = frees = unmaps = 0;
        BufferVK buffer;
        const auto result = CreateHostVisibleBuffer(physical, device, 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, buffer);
        if (failedStep)
        {
            check(result == VK_ERROR_OUT_OF_DEVICE_MEMORY, "allocation error must propagate");
            check(!buffer.buffer && !buffer.memory && !buffer.mapped && !buffer.size,
                  "partial allocation must reset every owned resource");
            check(destroys == (failedStep > 1) && frees == (failedStep > 2) && !unmaps,
                  "failure cleanup must destroy only successfully created resources");
        }
        else
        {
            check(result == VK_SUCCESS && buffer.size == 16 && buffer.mapped, "host buffer must be mapped");
            const uint32_t bytes = 0x12345678;
            UploadMappedBuffer(buffer, &bytes, sizeof(bytes));
            check(std::memcmp(mappedBytes.data(), &bytes, sizeof(bytes)) == 0, "upload must copy the actual bytes");
            check(CreateHostVisibleBuffer(physical, device, 16, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, buffer) ==
                      VK_ERROR_INITIALIZATION_FAILED,
                  "live buffers must not be silently replaced");
            bool rejected = false;
            try
            {
                UploadMappedBuffer(buffer, mappedBytes.data(), 17);
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            check(rejected, "oversized uploads must fail before writing");
            DestroyBuffer(device, buffer);
            check(destroys == 1 && frees == 1 && unmaps == 1, "mapped teardown must release each object exactly once");
        }
        DestroyBuffer(device, buffer);
        check(destroys == (failedStep == 1 ? 0 : 1) && frees == (failedStep == 1 || failedStep == 2 ? 0 : 1),
              "repeated cleanup must be idempotent");
    }
    return checks;
}
