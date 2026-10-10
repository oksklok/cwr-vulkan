#include <PoseidonVK/VulkanContext.hpp>
#include <PoseidonVK/Shaders/gamma.vert.hpp>
#include <PoseidonVK/Shaders/gamma.frag.hpp>
#include <stdexcept>

namespace Poseidon::vk
{
namespace
{
void Require(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan gamma: ") + operation + " failed (" + std::to_string(result) +
                                 ")");
}
} // namespace

void VulkanContext::CreateSceneColor(DepthAttachment& color, VkFormat format)
{
    // Using the swapchain format preserves its normalized precision and blend
    // space. UNORM stays UNORM; an sRGB fallback is decoded when sampled and
    // encoded once by the matching final attachment (no manual sRGB conversion).
    constexpr auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageFormatProperties supported{};
    Require(vkGetPhysicalDeviceImageFormatProperties(_physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                     usage, 0, &supported),
            "query sampled scene color format");
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format;
    image.extent = {_extent.width, _extent.height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = usage;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Require(vkCreateImage(_device, &image, nullptr, &color.image), "create scene color");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(_device, color.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(_physical, &memory);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        FindMemoryType(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX)
        throw std::runtime_error("Vulkan gamma: no device-local scene color memory");
    Require(vkAllocateMemory(_device, &allocation, nullptr, &color.memory), "allocate scene color");
    Require(vkBindImageMemory(_device, color.image, color.memory, 0), "bind scene color");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = color.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Require(vkCreateImageView(_device, &view, nullptr, &color.view), "create scene color view");
}

void VulkanContext::CreateGammaPass(VkFormat format)
{
    CreateTextureLayout();
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // Fullscreen triangle overwrites every pixel.
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = pass.subpassCount = pass.dependencyCount = 1;
    pass.pAttachments = &attachment;
    pass.pSubpasses = &subpass;
    pass.pDependencies = &dependency;
    Require(vkCreateRenderPass(_device, &pass, nullptr, &_gammaPass), "create final pass");

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(2 * _gammaTargets.size())};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = size.descriptorCount;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    Require(vkCreateDescriptorPool(_device, &pool, nullptr, &_gammaPool), "create final descriptor pool");
    for (size_t i = 0; i < _gammaTargets.size(); ++i)
    {
        auto& target = _gammaTargets[i];
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = _gammaPool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &_textureLayout;
        Require(vkAllocateDescriptorSets(_device, &allocation, &target.set), "allocate final descriptor");
        // Existing point/clamp sampler: exact texel sampling, no anisotropy/mips.
        const VkDescriptorImageInfo image{_textureSamplers[7], target.color.view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = target.set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
        Require(vkAllocateDescriptorSets(_device, &allocation, &target.depthSet), "allocate sampled depth descriptor");
        const VkDescriptorImageInfo depth{_textureSamplers[7], _sampledDepthViews[i],
                                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        write.dstSet = target.depthSet;
        write.pImageInfo = &depth;
        vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = _gammaPass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &_views[i];
        framebuffer.width = _extent.width;
        framebuffer.height = _extent.height;
        framebuffer.layers = 1;
        Require(vkCreateFramebuffer(_device, &framebuffer, nullptr, &target.framebuffer), "create final framebuffer");
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = layout.pushConstantRangeCount = 1;
    layout.pSetLayouts = &_textureLayout;
    layout.pPushConstantRanges = &push;
    Require(vkCreatePipelineLayout(_device, &layout, nullptr, &_gammaLayout), "create final layout");
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    try
    {
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = sizeof(Cwrgamma_vert);
        shader.pCode = Cwrgamma_vert;
        Require(vkCreateShaderModule(_device, &shader, nullptr, &vertex), "create final vertex shader");
        shader.codeSize = sizeof(Cwrgamma_frag);
        shader.pCode = Cwrgamma_frag;
        Require(vkCreateShaderModule(_device, &shader, nullptr, &fragment), "create final fragment shader");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[0].module = vertex;
        stages[1].module = fragment;
        stages[0].pName = stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState output{};
        output.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &output;
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
        pipeline.pColorBlendState = &blend;
        pipeline.pDynamicState = &dynamic;
        pipeline.layout = _gammaLayout;
        pipeline.renderPass = _gammaPass;
        Require(vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &_gammaPipeline),
                "create final pipeline");
    }
    catch (...)
    {
        if (fragment)
            vkDestroyShaderModule(_device, fragment, nullptr);
        if (vertex)
            vkDestroyShaderModule(_device, vertex, nullptr);
        throw;
    }
    vkDestroyShaderModule(_device, fragment, nullptr);
    vkDestroyShaderModule(_device, vertex, nullptr);
}

void VulkanContext::DrawGammaPass()
{
    const auto command = _frames[_frame].command;
    const auto& target = _gammaTargets[_image];
    // This layout is independent of the scene; never leave its bindings cached
    // as ordinary Shape state, including across command-buffer reuse.
    _commands = {};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = _gammaPass;
    pass.framebuffer = target.framebuffer;
    pass.renderArea.extent = _extent;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _gammaPipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _gammaLayout, 0, 1, &target.set, 0, nullptr);
    const VkViewport viewport{0, 0, float(_extent.width), float(_extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, _extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    // Match GL33's small identity tolerance, including live settings changes.
    const float inverse = _gamma > 0.999f && _gamma < 1.001f ? 1.f : 1.f / _gamma;
    vkCmdPushConstants(command, _gammaLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(inverse), &inverse);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
}

void VulkanContext::DestroyGammaResources() noexcept
{
    if (_gammaPipeline)
        vkDestroyPipeline(_device, _gammaPipeline, nullptr);
    if (_gammaLayout)
        vkDestroyPipelineLayout(_device, _gammaLayout, nullptr);
    if (_gammaPool)
        vkDestroyDescriptorPool(_device, _gammaPool, nullptr);
    _gammaPipeline = VK_NULL_HANDLE;
    _gammaLayout = VK_NULL_HANDLE;
    _gammaPool = VK_NULL_HANDLE;
    for (auto& target : _gammaTargets)
    {
        if (target.framebuffer)
            vkDestroyFramebuffer(_device, target.framebuffer, nullptr);
        if (target.color.view)
            vkDestroyImageView(_device, target.color.view, nullptr);
        if (target.color.image)
            vkDestroyImage(_device, target.color.image, nullptr);
        if (target.color.memory)
            vkFreeMemory(_device, target.color.memory, nullptr);
    }
    _gammaTargets.clear();
    if (_gammaPass)
        vkDestroyRenderPass(_device, _gammaPass, nullptr);
    _gammaPass = VK_NULL_HANDLE;
}
} // namespace Poseidon::vk
