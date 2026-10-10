#include <PoseidonVK/VulkanContext.hpp>
#include <algorithm>
#include <stdexcept>
#include <cstring>

namespace Poseidon::vk
{
namespace
{
void Require(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan texture: ") + operation + " failed (" + std::to_string(result) +
                                 ")");
}
} // namespace
TextureImage::~TextureImage()
{
    Destroy();
}
void TextureImage::Destroy() noexcept
{
    if (device)
    {
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
    }
    device = VK_NULL_HANDLE;
}
void VulkanContext::CreateTextureLayout()
{
    if (_textureLayout)
        return;
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 1;
    info.pBindings = &binding;
    Require(vkCreateDescriptorSetLayout(_device, &info, nullptr, &_textureLayout), "create descriptor layout");
    // Sampling states are device-wide, not eight sampler objects per asset.
    for (unsigned i = 0; i < 8; ++i)
    {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = (i & 4) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sampler.mipmapMode = (i & 4) ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.maxLod = VK_LOD_CLAMP_NONE; // Image view bounds naturally clamp valid shorter chains.
        sampler.addressModeU = (i & 1) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeV = (i & 2) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        Require(vkCreateSampler(_device, &sampler, nullptr, &_textureSamplers[i]), "create shared sampler");
    }
}
std::shared_ptr<TextureImage> VulkanContext::UploadTexture(uint32_t width, uint32_t height, const void* rgba)
{
    const TextureMip mip{width, height, rgba};
    return UploadTexture(std::span(&mip, 1));
}
std::shared_ptr<TextureImage> VulkanContext::UploadTexture(std::span<const TextureMip> mips)
{
    if (!_device || mips.empty())
        throw std::invalid_argument("Vulkan texture upload requires pixels and a live device");
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize bytes = 0;
    for (size_t level = 0; level < mips.size(); ++level)
    {
        const auto& mip = mips[level];
        if (!mip.width || !mip.height || !mip.rgba ||
            (level && (mip.width != std::max(1u, mips[level - 1].width / 2) ||
                       mip.height != std::max(1u, mips[level - 1].height / 2) ||
                       (mips[level - 1].width == 1 && mips[level - 1].height == 1))))
            throw std::invalid_argument("Vulkan texture upload has an invalid RGBA mip chain");
        VkBufferImageCopy copy{};
        copy.bufferOffset = bytes; // RGBA levels are naturally four-byte aligned.
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(level), 0, 1};
        copy.imageExtent = {mip.width, mip.height, 1};
        copies.push_back(copy);
        bytes += VkDeviceSize(mip.width) * mip.height * 4;
    }
    CreateTextureLayout();
    auto texture = std::make_shared<TextureImage>();
    texture->device = _device;
    BufferVK staging;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence complete = VK_NULL_HANDLE;
    bool submitted = false;
    try
    {
        Require(CreateHostVisibleBuffer(_physical, _device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging),
                "create staging buffer");
        for (size_t level = 0; level < mips.size(); ++level)
            std::memcpy(static_cast<char*>(staging.mapped) + copies[level].bufferOffset, mips[level].rgba,
                        size_t(mips[level].width) * mips[level].height * 4);
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = {mips[0].width, mips[0].height, 1};
        image.mipLevels = uint32_t(mips.size());
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Require(vkCreateImage(_device, &image, nullptr, &texture->image), "create image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(_device, texture->image, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(_physical, &memory);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex =
            FindMemoryType(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocation.memoryTypeIndex == UINT32_MAX)
            throw std::runtime_error("No Vulkan texture memory type");
        Require(vkAllocateMemory(_device, &allocation, nullptr, &texture->memory), "allocate image memory");
        Require(vkBindImageMemory(_device, texture->image, texture->memory, 0), "bind image memory");
        VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commands.commandPool = _pool;
        commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands.commandBufferCount = 1;
        Require(vkAllocateCommandBuffers(_device, &commands, &command), "allocate upload command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Require(vkBeginCommandBuffer(command, &begin), "begin upload");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture->image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(mips.size()), 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
        vkCmdCopyBufferToImage(command, staging.buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               uint32_t(copies.size()), copies.data());
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        Require(vkEndCommandBuffer(command), "end upload");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Require(vkCreateFence(_device, &fence, nullptr, &complete), "create upload fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        Require(vkQueueSubmit(_graphics, 1, &submit, complete), "submit upload");
        submitted = true;
        Require(vkWaitForFences(_device, 1, &complete, VK_TRUE, UINT64_MAX), "wait upload fence");
        submitted = false;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = texture->image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange = barrier.subresourceRange;
        Require(vkCreateImageView(_device, &view, nullptr, &texture->view), "create texture view");
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 8;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        Require(vkCreateDescriptorPool(_device, &pool, nullptr, &texture->pool), "create texture descriptor pool");
        VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set.descriptorPool = texture->pool;
        set.descriptorSetCount = 1;
        set.pSetLayouts = &_textureLayout;
        for (unsigned i = 0; i < 8; ++i)
        {
            Require(vkAllocateDescriptorSets(_device, &set, &texture->descriptors[i]), "allocate texture descriptor");
            const VkDescriptorImageInfo imageInfo{_textureSamplers[i], texture->view,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = texture->descriptors[i];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
        }
    }
    catch (...)
    {
        if (submitted)
            vkQueueWaitIdle(_graphics); // Only exceptional failed upload recovery.
        if (complete)
            vkDestroyFence(_device, complete, nullptr);
        if (command)
            vkFreeCommandBuffers(_device, _pool, 1, &command);
        DestroyBuffer(_device, staging);
        throw;
    }
    vkDestroyFence(_device, complete, nullptr);
    vkFreeCommandBuffers(_device, _pool, 1, &command);
    DestroyBuffer(_device, staging);
    std::erase_if(_textures, [](const auto& entry) { return entry.expired(); });
    _textures.push_back(texture);
    return texture;
}
} // namespace Poseidon::vk
