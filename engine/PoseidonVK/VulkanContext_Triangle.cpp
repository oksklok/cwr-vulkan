#include <PoseidonVK/VulkanContext.hpp>
#include <PoseidonVK/TriangleVK.hpp>
#include <PoseidonVK/Shaders/triangle.vert.hpp>
#include <PoseidonVK/Shaders/triangle.frag.hpp>
#include <cstdio>
#include <stdexcept>
#include <type_traits>

namespace Poseidon::vk
{
namespace
{
void Require(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan triangle: ") + operation + " failed (VkResult " +
                                 std::to_string(static_cast<int>(result)) + ")");
}
template <class Handle>
uint64_t Bits(Handle handle)
{
    if constexpr (std::is_pointer_v<Handle>)
        return reinterpret_cast<uint64_t>(handle);
    else
        return static_cast<uint64_t>(handle);
}
} // namespace

void VulkanContext::CreateTrianglePipeline()
{
    if (!_triangleLayout)
    {
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        Require(vkCreatePipelineLayout(_device, &layout, nullptr, &_triangleLayout), "create pipeline layout");
        Name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, Bits(_triangleLayout), "PoseidonVK triangle layout");
    }
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    try
    {
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = sizeof(CwrTriangle_vert);
        shader.pCode = CwrTriangle_vert;
        Require(vkCreateShaderModule(_device, &shader, nullptr, &vertex), "create vertex shader");
        shader.codeSize = sizeof(CwrTriangle_frag);
        shader.pCode = CwrTriangle_frag;
        Require(vkCreateShaderModule(_device, &shader, nullptr, &fragment), "create fragment shader");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[0].module = vertex;
        stages[1].module = fragment;
        stages[0].pName = stages[1].pName = "main";
        const VkVertexInputBindingDescription binding{0, sizeof(TriangleVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attributes[] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TriangleVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TriangleVertex, color)}};
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
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
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
        pipeline.layout = _triangleLayout;
        pipeline.renderPass = _renderPass;
        Require(vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &_trianglePipeline),
                "create graphics pipeline");
        Name(VK_OBJECT_TYPE_PIPELINE, Bits(_trianglePipeline), "PoseidonVK indexed triangle");
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
    std::fprintf(stderr, "Vulkan: triangle pipeline ready for swapchain #%u (SPIR-V 1.0, no descriptors/depth)\n",
                 _swapchainGeneration);
}

void VulkanContext::DrawDiagnosticTriangle()
{
    if (!_frameOpen)
        throw std::logic_error("Vulkan triangle: indexed draw requires an open frame");
    if (!_triangleVertices.buffer)
    {
        Require(CreateHostVisibleBuffer(_physical, _device, sizeof(TriangleVertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                        _triangleVertices),
                "create vertex buffer");
        Require(CreateHostVisibleBuffer(_physical, _device, sizeof(TriangleIndices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                        _triangleIndices),
                "create index buffer");
        UploadMappedBuffer(_triangleVertices, TriangleVertices.data(), sizeof(TriangleVertices));
        UploadMappedBuffer(_triangleIndices, TriangleIndices.data(), sizeof(TriangleIndices));
        Name(VK_OBJECT_TYPE_BUFFER, Bits(_triangleVertices.buffer), "PoseidonVK triangle vertices");
        Name(VK_OBJECT_TYPE_BUFFER, Bits(_triangleIndices.buffer), "PoseidonVK triangle indices");
        std::fprintf(stderr, "Vulkan: immutable triangle uploaded: 3 vertices, 3 uint16 indices (2,0,1)\n");
    }
    if (!_trianglePipeline)
        CreateTrianglePipeline();
    const VkCommandBuffer command = _frames[_frame].command;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _trianglePipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &_triangleVertices.buffer, &offset);
    vkCmdBindIndexBuffer(command, _triangleIndices.buffer, 0, VK_INDEX_TYPE_UINT16);
    const VkViewport viewport{0, 0, static_cast<float>(_extent.width), static_cast<float>(_extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, _extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    const std::array<float, 8> constants{
        0, 0, viewport.width, viewport.height, _clearColor[0], _clearColor[1], _clearColor[2], _clearColor[3]};
    vkCmdPushConstants(command, _triangleLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(constants), constants.data());
    vkCmdDrawIndexed(command, static_cast<uint32_t>(TriangleIndices.size()), 1, 0, 0, 0);
    if (!_loggedTriangle)
    {
        std::fprintf(stderr, "Vulkan: first vkCmdDrawIndexed recorded\n");
        _loggedTriangle = true;
    }
}
} // namespace Poseidon::vk
