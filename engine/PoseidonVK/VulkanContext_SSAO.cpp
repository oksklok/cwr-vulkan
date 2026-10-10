#include <PoseidonVK/VulkanContext.hpp>
#include <PoseidonVK/Shaders/gamma.vert.hpp>
#include <PoseidonVK/Shaders/ssao.frag.hpp>
#include <cmath>
#include <stdexcept>

namespace Poseidon::vk
{
namespace
{
void Require(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan SSAO: ") + operation + " failed (" + std::to_string(result) + ")");
}
}

bool VulkanContext::SetSSAO(bool enabled, float strength, float radius, float bias, float fade)
{
    if (!std::isfinite(strength) || !std::isfinite(radius) || !std::isfinite(bias) || !std::isfinite(fade) ||
        strength < 0 || strength > 2 || radius < 0.05f || radius > 5 || bias < 0 || bias >= radius ||
        fade < 5 || fade > 500)
        return false;
    _ssaoSettings = {strength, radius, bias, fade};
    _ssaoEnabled = enabled;
    return true;
}

void VulkanContext::CreateSSAOResources(VkFormat format)
{
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependencies[2]{};
    auto& in = dependencies[0];
    in.srcSubpass = VK_SUBPASS_EXTERNAL;
    in.dstSubpass = 0;
    in.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    in.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    in.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    in.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    auto& out = dependencies[1];
    out.srcSubpass = 0;
    out.dstSubpass = VK_SUBPASS_EXTERNAL;
    out.srcStageMask = in.dstStageMask;
    out.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    out.dstStageMask = in.srcStageMask | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    out.dstAccessMask = in.srcAccessMask | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = pass.subpassCount = 1;
    pass.pAttachments = &attachment;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 2;
    pass.pDependencies = dependencies;
    Require(vkCreateRenderPass(_device, &pass, nullptr, &_ssaoPass), "create modulation pass");
    for (auto& target : _gammaTargets)
    {
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = _ssaoPass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &target.color.view;
        framebuffer.width = _extent.width;
        framebuffer.height = _extent.height;
        framebuffer.layers = 1;
        Require(vkCreateFramebuffer(_device, &framebuffer, nullptr, &target.ssaoFramebuffer), "create modulation framebuffer");
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 * sizeof(float)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = layout.pushConstantRangeCount = 1;
    layout.pSetLayouts = &_textureLayout;
    layout.pPushConstantRanges = &push;
    Require(vkCreatePipelineLayout(_device, &layout, nullptr, &_ssaoLayout), "create layout");
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    try
    {
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = sizeof(Cwrgamma_vert);
        shader.pCode = Cwrgamma_vert;
        Require(vkCreateShaderModule(_device, &shader, nullptr, &vertex), "create vertex shader");
        shader.codeSize = sizeof(Cwrssao_frag);
        shader.pCode = Cwrssao_frag;
        Require(vkCreateShaderModule(_device, &shader, nullptr, &fragment), "create fragment shader");
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
        output.blendEnable = VK_TRUE;
        output.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        output.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        output.colorBlendOp = output.alphaBlendOp = VK_BLEND_OP_ADD;
        output.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        output.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        output.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
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
        pipeline.layout = _ssaoLayout;
        pipeline.renderPass = _ssaoPass;
        Require(vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &_ssaoPipeline), "create pipeline");
    }
    catch (...)
    {
        if (fragment) vkDestroyShaderModule(_device, fragment, nullptr);
        if (vertex) vkDestroyShaderModule(_device, vertex, nullptr);
        throw;
    }
    vkDestroyShaderModule(_device, fragment, nullptr);
    vkDestroyShaderModule(_device, vertex, nullptr);
}

void VulkanContext::DrawSSAO(const std::array<float, 4>& projection)
{
    if (!_frameOpen || !_ssaoEnabled || _ssaoSettings[0] == 0)
        return;
    for (float value : projection)
        if (!std::isfinite(value)) return;
    if (projection[0] <= 0 || projection[1] <= 0 || projection[2] <= 1 || projection[3] >= 0)
        return;
    FlushScreenBatch();
    const double start = _profile.enabled ? ProfileClock() : 0;
    const auto command = _frames[_frame].command;
    vkCmdEndRenderPass(command);
    _commands = {};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = _ssaoPass;
    pass.framebuffer = _gammaTargets[_image].ssaoFramebuffer;
    pass.renderArea.extent = _extent;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _ssaoPipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _ssaoLayout, 0, 1,
                            &_gammaTargets[_image].depthSet, 0, nullptr);
    const VkViewport viewport{0, 0, float(_extent.width), float(_extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, _extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    const std::array<float, 8> push{projection[0], projection[1], projection[2], projection[3],
                                  _ssaoSettings[0], _ssaoSettings[1], _ssaoSettings[2], _ssaoSettings[3]};
    vkCmdPushConstants(command, _ssaoLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push.data());
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    pass.renderPass = _resumePass;
    pass.framebuffer = _framebuffers[_image];
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    if (_profile.enabled)
    {
        _profile.ssaoMs += ProfileClock() - start;
        ++_profile.ssaoPasses;
    }
}

void VulkanContext::DestroySSAOResources() noexcept
{
    for (auto& target : _gammaTargets)
    {
        if (target.ssaoFramebuffer) vkDestroyFramebuffer(_device, target.ssaoFramebuffer, nullptr);
        target.ssaoFramebuffer = VK_NULL_HANDLE;
    }
    if (_ssaoPipeline) vkDestroyPipeline(_device, _ssaoPipeline, nullptr);
    if (_ssaoLayout) vkDestroyPipelineLayout(_device, _ssaoLayout, nullptr);
    if (_ssaoPass) vkDestroyRenderPass(_device, _ssaoPass, nullptr);
    _ssaoPipeline = VK_NULL_HANDLE;
    _ssaoLayout = VK_NULL_HANDLE;
    _ssaoPass = VK_NULL_HANDLE;
}
} // namespace Poseidon::vk
