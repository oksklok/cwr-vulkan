#include <PoseidonVK/VulkanContext.hpp>
#include <algorithm>
#include <stdexcept>
#include <PoseidonVK/Shaders/shape.vert.hpp>
#include <PoseidonVK/Shaders/shape.frag.hpp>
#include <cstdio>

namespace Poseidon::vk
{
namespace
{
void Require(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan Shape: ") + operation + " failed (" + std::to_string(result) +
                                 ")");
}
} // namespace
MeshBuffers::~MeshBuffers()
{
    if (device)
        vkDeviceWaitIdle(device);
    Destroy();
}
void MeshBuffers::Destroy() noexcept
{
    if (device)
    {
        DestroyBuffer(device, indices);
        DestroyBuffer(device, vertices);
    }
    device = VK_NULL_HANDLE;
}

std::shared_ptr<MeshBuffers> VulkanContext::UploadMesh(const void* vertices, size_t vertexBytes, const void* indices,
                                                       size_t indexBytes)
{
    if (!_device || !vertices || !indices || !vertexBytes || !indexBytes)
        throw std::invalid_argument("Vulkan Shape: upload needs a live device and nonempty geometry");
    auto mesh = std::make_shared<MeshBuffers>();
    mesh->device = _device;
    const auto vertexResult =
        CreateHostVisibleBuffer(_physical, _device, vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mesh->vertices);
    if (vertexResult != VK_SUCCESS)
        throw std::runtime_error("Vulkan Shape: vertex buffer allocation failed (" + std::to_string(vertexResult) +
                                 ")");
    const auto indexResult =
        CreateHostVisibleBuffer(_physical, _device, indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh->indices);
    if (indexResult != VK_SUCCESS)
        throw std::runtime_error("Vulkan Shape: index buffer allocation failed (" + std::to_string(indexResult) + ")");
    UploadMappedBuffer(mesh->vertices, vertices, vertexBytes);
    UploadMappedBuffer(mesh->indices, indices, indexBytes);
    std::erase_if(_meshes, [](const auto& entry) { return entry.expired(); });
    _meshes.push_back(mesh);
    return mesh;
}

void VulkanContext::CreateShapePipeline()
{
    if (!_shapeLayout)
    {
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        Require(vkCreatePipelineLayout(_device, &layout, nullptr, &_shapeLayout), "create pipeline layout");
    }
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    try
    {
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = sizeof(Cwrshape_vert);
        shader.pCode = Cwrshape_vert;
        Require(vkCreateShaderModule(_device, &shader, nullptr, &vertex), "create vertex shader");
        shader.codeSize = sizeof(Cwrshape_frag);
        shader.pCode = Cwrshape_frag;
        Require(vkCreateShaderModule(_device, &shader, nullptr, &fragment), "create fragment shader");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[0].module = vertex;
        stages[1].module = fragment;
        stages[0].pName = stages[1].pName = "main";
        // GL33 SVertex-compatible position/normal/UV packing; only position is
        // consumed by this deliberately unlit, untextured shader.
        const VkVertexInputBindingDescription binding{0, 8 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 1;
        input.pVertexAttributeDescriptions = &attribute;
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
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
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
        pipeline.pDepthStencilState = &depth;
        pipeline.pColorBlendState = &blend;
        pipeline.pDynamicState = &dynamic;
        pipeline.layout = _shapeLayout;
        pipeline.renderPass = _renderPass;
        Require(vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &_shapePipeline),
                "create graphics pipeline");
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
    std::fprintf(stderr, "Vulkan: Shape pipeline ready for swapchain #%u (opaque, depth tested)\n",
                 _swapchainGeneration);
}

void VulkanContext::DrawMesh(const std::shared_ptr<MeshBuffers>& mesh, uint32_t firstIndex, uint32_t count,
                             bool index16, const std::array<float, 16>& mvp, const std::array<float, 4>& color)
{
    if (!_frameOpen || !mesh || mesh->device != _device || !mesh->vertices.buffer || !mesh->indices.buffer)
        throw std::logic_error("Vulkan Shape: indexed draw needs an open frame and live buffers from this device");
    const VkDeviceSize indexSize = index16 ? 2 : 4;
    if (VkDeviceSize(firstIndex) + count > mesh->indices.size / indexSize || count % 3 != 0)
        throw std::out_of_range("Vulkan Shape: indexed draw exceeds geometry or is not a triangle list");
    if (!count)
        return;
    if (!_shapePipeline)
        CreateShapePipeline();
    auto& frame = _frames[_frame];
    // Retain each mesh once per frame until that frame's submission fence completes.
    if (std::find(frame.meshes.begin(), frame.meshes.end(), mesh) == frame.meshes.end())
        frame.meshes.push_back(mesh);
    vkCmdBindPipeline(frame.command, VK_PIPELINE_BIND_POINT_GRAPHICS, _shapePipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(frame.command, 0, 1, &mesh->vertices.buffer, &offset);
    vkCmdBindIndexBuffer(frame.command, mesh->indices.buffer, 0, index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    const VkViewport viewport{0, 0, float(_extent.width), float(_extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, _extent};
    vkCmdSetViewport(frame.command, 0, 1, &viewport);
    vkCmdSetScissor(frame.command, 0, 1, &scissor);
    std::array<float, 20> constants{};
    std::copy(mvp.begin(), mvp.end(), constants.begin());
    std::copy(color.begin(), color.end(), constants.begin() + 16);
    vkCmdPushConstants(frame.command, _shapeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(constants), constants.data());
    vkCmdDrawIndexed(frame.command, count, 1, firstIndex, 0, 0);
    if (!_loggedShape)
    {
        std::fprintf(stderr, "Vulkan: first production Shape draw: firstIndex=%u count=%u\n", firstIndex, count);
        _loggedShape = true;
    }
}

void VulkanContext::CreateDepthAttachment(DepthAttachment& depth)
{
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = _depthFormat;
    image.extent = {_extent.width, _extent.height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Require(vkCreateImage(_device, &image, nullptr, &depth.image), "create depth image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(_device, depth.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(_physical, &memory);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        FindMemoryType(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX)
        throw std::runtime_error("Vulkan Shape: no device-local depth memory");
    Require(vkAllocateMemory(_device, &allocation, nullptr, &depth.memory), "allocate depth memory");
    Require(vkBindImageMemory(_device, depth.image, depth.memory, 0), "bind depth memory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = depth.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = _depthFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    Require(vkCreateImageView(_device, &view, nullptr, &depth.view), "create depth view");
}
} // namespace Poseidon::vk
