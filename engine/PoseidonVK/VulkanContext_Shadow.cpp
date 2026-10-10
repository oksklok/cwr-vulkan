#include <PoseidonVK/VulkanContext.hpp>
#include <PoseidonVK/Shaders/csm.vert.hpp>
#include <PoseidonVK/Shaders/csm.frag.hpp>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace Poseidon::vk
{
namespace
{
void Require(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan CSM: ") + operation + " failed (" + std::to_string(result) + ")");
}
void SampledSet(VkDevice device, VkDescriptorSetLayout layout, VkImageView view, VkSampler sampler,
                VkImageLayout imageLayout, VkDescriptorPool& pool, VkDescriptorSet& set)
{
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo create{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    create.maxSets = create.poolSizeCount = 1;
    create.pPoolSizes = &size;
    Require(vkCreateDescriptorPool(device, &create, nullptr, &pool), "create descriptor pool");
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout;
    Require(vkAllocateDescriptorSets(device, &alloc, &set), "allocate descriptor");
    const VkDescriptorImageInfo image{sampler, view, imageLayout};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}
} // namespace

void VulkanContext::CreateShadowFallback()
{
    if (_csmFallbackSet)
        return;
    if (!_whiteTexture)
    {
        const uint32_t white = 0xffffffff;
        _whiteTexture = UploadTexture(1, 1, &white);
    }
    // A valid array descriptor even for disabled CSM and UI pipelines. No
    // depth allocation/clear is needed for the opt-out path.
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = _whiteTexture->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Require(vkCreateImageView(_device, &view, nullptr, &_csmFallbackView), "create fallback array view");
    SampledSet(_device, _textureLayout, _csmFallbackView, _textureSamplers[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               _csmFallbackPool, _csmFallbackSet);
}

void VulkanContext::CreateShadowResources()
{
    if (_csmPass)
        return;
    CreateTextureLayout();
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(_physical, VK_FORMAT_D32_SFLOAT, &format);
    const auto required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((format.optimalTilingFeatures & required) != required)
        throw std::runtime_error("Vulkan CSM requires sampled D32 depth support");
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    Require(vkCreateSampler(_device, &sampler, nullptr, &_csmSampler), "create depth sampler");
    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &reference;
    VkSubpassDependency dependencies[2]{};
    dependencies[0] = {VK_SUBPASS_EXTERNAL,
                       0,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_SHADER_READ_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       0};
    dependencies[1] = {0,
                       VK_SUBPASS_EXTERNAL,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT,
                       0};
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = pass.subpassCount = 1;
    pass.pAttachments = &depth;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 2;
    pass.pDependencies = dependencies;
    Require(vkCreateRenderPass(_device, &pass, nullptr, &_csmPass), "create depth pass");
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 68};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = layout.pushConstantRangeCount = 1;
    layout.pSetLayouts = &_textureLayout;
    layout.pPushConstantRanges = &push;
    Require(vkCreatePipelineLayout(_device, &layout, nullptr, &_csmLayout), "create depth layout");
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    try
    {
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = sizeof(Cwrcsm_vert);
        module.pCode = Cwrcsm_vert;
        Require(vkCreateShaderModule(_device, &module, nullptr, &vertex), "create depth vertex shader");
        module.codeSize = sizeof(Cwrcsm_frag);
        module.pCode = Cwrcsm_frag;
        Require(vkCreateShaderModule(_device, &module, nullptr, &fragment), "create cutout shader");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";
        VkVertexInputBindingDescription binding{0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, 0}}; // Solid UV is unused, but valid.
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 2;
        input.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.lineWidth = 1;
        // Y inversion changes winding versus GL's lower-left viewport.
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo testing{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        testing.depthTestEnable = testing.depthWriteEnable = VK_TRUE;
        testing.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline.stageCount = 2;
        pipeline.pStages = stages;
        pipeline.pVertexInputState = &input;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState = &viewport;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState = &samples;
        pipeline.pDepthStencilState = &testing;
        pipeline.pColorBlendState = &blend;
        pipeline.pDynamicState = &dynamic;
        pipeline.layout = _csmLayout;
        pipeline.renderPass = _csmPass;
        for (int alpha = 0; alpha < 2; ++alpha)
        {
            binding.stride = alpha ? 20 : 12;
            attributes[1].offset = alpha ? 12 : 0;
            raster.cullMode = alpha ? VK_CULL_MODE_NONE : VK_CULL_MODE_FRONT_BIT;
            Require(vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &_csmPipelines[alpha]),
                    "create depth pipeline");
        }
    }
    catch (...)
    {
        if (vertex)
            vkDestroyShaderModule(_device, vertex, nullptr);
        if (fragment)
            vkDestroyShaderModule(_device, fragment, nullptr);
        throw;
    }
    vkDestroyShaderModule(_device, vertex, nullptr);
    vkDestroyShaderModule(_device, fragment, nullptr);
}

void VulkanContext::CreateShadowTarget(Frame::ShadowTarget& target, int resolution, int count)
{
    if (target.resolution == resolution && target.count == count)
        return;
    // Only the current fence-retired frame owns this target. No device idle,
    // cross-frame image writes, descriptor rewrites, or per-cascade uploads.
    DestroyShadowTarget(target);
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_D32_SFLOAT;
    image.extent = {uint32_t(resolution), uint32_t(resolution), 1};
    image.mipLevels = 1;
    image.arrayLayers = count;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    Require(vkCreateImage(_device, &image, nullptr, &target.image), "create depth array");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(_device, target.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(_physical, &memory);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        FindMemoryType(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX)
        throw std::runtime_error("Vulkan CSM no depth memory type");
    Require(vkAllocateMemory(_device, &allocation, nullptr, &target.memory), "allocate depth array");
    Require(vkBindImageMemory(_device, target.image, target.memory, 0), "bind depth array");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = target.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = image.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, uint32_t(count)};
    Require(vkCreateImageView(_device, &view, nullptr, &target.view), "create depth array view");
    for (int i = 0; i < count; ++i)
    {
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange.baseArrayLayer = i;
        view.subresourceRange.layerCount = 1;
        Require(vkCreateImageView(_device, &view, nullptr, &target.layers[i]), "create cascade view");
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = _csmPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &target.layers[i];
        fb.width = fb.height = resolution;
        fb.layers = 1;
        Require(vkCreateFramebuffer(_device, &fb, nullptr, &target.framebuffers[i]), "create cascade framebuffer");
    }
    SampledSet(_device, _textureLayout, target.view, _csmSampler, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
               target.pool, target.set);
    target.resolution = resolution;
    target.count = count;
    std::fprintf(stderr, "Vulkan CSM: frame %zu depth array %dx%dx%d ready\n", _frame, resolution, resolution, count);
}

void VulkanContext::RenderShadowDepth(const float* matrices, int count, int resolution, std::span<const float> solid,
                                      std::span<const float> alpha, std::span<const ShadowAlphaBatch> batches)
{
    if (!_frameOpen)
        return;
    if (_csmActive || _shadowPass || !matrices || count < 1 || count > 4 || resolution < 128 || resolution > 4096 ||
        solid.size() % 9 || alpha.size() % 15)
        throw std::invalid_argument("Vulkan invalid or repeated CSM depth pass");
    FlushScreenBatch();
    CreateShadowResources();
    CreateShadowFallback();
    auto& frame = _frames[_frame];
    CreateShadowTarget(frame.shadow, resolution, count);
    const uint32_t unusedIndex = 0;
    MeshSlice solidMesh, alphaMesh;
    // Reuse the existing fence-retired streaming pages. The index placeholder
    // keeps their allocation protocol; depth draws themselves are unindexed.
    if (!solid.empty())
        solidMesh = UploadTransientMesh(solid.data(), solid.size_bytes(), &unusedIndex, sizeof(unusedIndex));
    if (!alpha.empty())
        alphaMesh = UploadTransientMesh(alpha.data(), alpha.size_bytes(), &unusedIndex, sizeof(unusedIndex));
    for (const auto& batch : batches)
    {
        if (size_t(batch.first) + batch.count > alpha.size() / 5 || batch.count % 3)
            throw std::out_of_range("Vulkan shadow alpha batch exceeds vertices");
        if (batch.texture)
            frame.textures.push_back(batch.texture);
    }
    const auto command = frame.command;
    vkCmdEndRenderPass(command);
    if (frame.csmQueries)
    {
        vkCmdResetQueryPool(command, frame.csmQueries, 0, 2);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.csmQueries, 0);
    }
    _commands = {};
    const VkViewport viewport{0, 0, float(resolution), float(resolution), 0, 1};
    const VkRect2D scissor{{0, 0}, {uint32_t(resolution), uint32_t(resolution)}};
    VkClearValue clear{};
    clear.depthStencil.depth = 1;
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = _csmPass;
    pass.renderArea = scissor;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    for (int c = 0; c < count; ++c)
    {
        pass.framebuffer = frame.shadow.framebuffers[c];
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        std::array<float, 17> push{};
        std::copy_n(matrices + c * 16, 16, push.begin());
        for (int cutout = 0; cutout < 2; ++cutout)
        {
            const auto& mesh = cutout ? alphaMesh : solidMesh;
            if (!mesh.buffers)
                continue;
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _csmPipelines[cutout]);
            vkCmdBindVertexBuffers(command, 0, 1, &mesh.buffers->vertices.buffer, &mesh.vertexOffset);
            push[16] = cutout ? 0.5f : 0.f; // GL33 caster threshold.
            vkCmdPushConstants(command, _csmLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), push.data());
            if (!cutout)
            {
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _csmLayout, 0, 1,
                                        &_whiteTexture->descriptors[0], 0, nullptr);
                vkCmdDraw(command, uint32_t(solid.size() / 3), 1, 0, 0);
            }
            else
                for (const auto& batch : batches)
                {
                    const auto set = (batch.texture ? batch.texture : _whiteTexture)->descriptors[0];
                    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _csmLayout, 0, 1, &set, 0,
                                            nullptr);
                    vkCmdDraw(command, batch.count, 1, batch.first, 0);
                }
        }
        vkCmdEndRenderPass(command);
    }
    if (frame.csmQueries)
    {
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.csmQueries, 1);
        frame.csmTimestamped = true;
    }
    if (_profile.enabled)
    {
        ++_profile.csmPasses;
        _profile.csmVertices += solid.size() / 3 + alpha.size() / 5;
    }
    pass.renderPass = ResumePass();
    pass.framebuffer = SceneFramebuffer();
    pass.renderArea.extent = RenderExtent();
    pass.clearValueCount = 0;
    pass.pClearValues = nullptr;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    _csmActive = true;
}

void VulkanContext::DestroyShadowTarget(Frame::ShadowTarget& target) noexcept
{
    if (target.pool)
        vkDestroyDescriptorPool(_device, target.pool, nullptr);
    for (auto fb : target.framebuffers)
        if (fb)
            vkDestroyFramebuffer(_device, fb, nullptr);
    for (auto view : target.layers)
        if (view)
            vkDestroyImageView(_device, view, nullptr);
    if (target.view)
        vkDestroyImageView(_device, target.view, nullptr);
    if (target.image)
        vkDestroyImage(_device, target.image, nullptr);
    if (target.memory)
        vkFreeMemory(_device, target.memory, nullptr);
    target = {};
}

void VulkanContext::DestroyShadowResources() noexcept
{
    for (auto& frame : _frames)
        DestroyShadowTarget(frame.shadow);
    for (auto pipeline : _csmPipelines)
        if (pipeline)
            vkDestroyPipeline(_device, pipeline, nullptr);
    if (_csmLayout)
        vkDestroyPipelineLayout(_device, _csmLayout, nullptr);
    if (_csmPass)
        vkDestroyRenderPass(_device, _csmPass, nullptr);
    if (_csmSampler)
        vkDestroySampler(_device, _csmSampler, nullptr);
    if (_csmFallbackPool)
        vkDestroyDescriptorPool(_device, _csmFallbackPool, nullptr);
    if (_csmFallbackView)
        vkDestroyImageView(_device, _csmFallbackView, nullptr);
    _csmPipelines = {};
    _csmLayout = VK_NULL_HANDLE;
    _csmPass = VK_NULL_HANDLE;
    _csmSampler = VK_NULL_HANDLE;
    _csmFallbackPool = VK_NULL_HANDLE;
    _csmFallbackView = VK_NULL_HANDLE;
    _csmFallbackSet = VK_NULL_HANDLE;
    _csmActive = false;
}
} // namespace Poseidon::vk
