#include <PoseidonVK/VulkanContext.hpp>
#include <PoseidonVK/ScreenPipelineVK.hpp>
#include <algorithm>
#include <stdexcept>
#include <PoseidonVK/Shaders/shape.vert.hpp>
#include <PoseidonVK/Shaders/shape.frag.hpp>
#include <PoseidonVK/Shaders/screen.vert.hpp>
#include <cstdio>
#include <cstring>

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
    // DrawMesh retains buffers in every referencing frame until its fence signals.
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
    if (_profile.enabled)
        _profile.allocations += 2;
    std::erase_if(_meshes, [](const auto& entry) { return entry.expired(); });
    _meshes.push_back(mesh);
    return mesh;
}

MeshSlice VulkanContext::UploadTransientMesh(const void* vertices, size_t vertexBytes, const void* indices,
                                             size_t indexBytes)
{
    if (!_frameOpen || !vertices || !indices || !vertexBytes || !indexBytes)
        throw std::invalid_argument("Vulkan transient upload requires an acquired frame and nonempty geometry");
    const double started = _profile.enabled ? ProfileClock() : 0;
    auto& frame = _frames[_frame];
    for (;; ++frame.transientPage)
    {
        if (frame.transientPage == frame.transientPages.size())
        {
            // Grow by adding a page, never replacing a buffer already referenced
            // by recorded commands. Retained pages are reused at the next fence.
            auto buffers = std::make_shared<MeshBuffers>();
            buffers->device = _device;
            Require(CreateHostVisibleBuffer(_physical, _device, std::max<size_t>(4 * 1024 * 1024, vertexBytes),
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, buffers->vertices),
                    "allocate transient vertices");
            Require(CreateHostVisibleBuffer(_physical, _device, std::max<size_t>(1024 * 1024, indexBytes),
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, buffers->indices),
                    "allocate transient indices");
            frame.transientPages.push_back({buffers});
            _meshes.push_back(buffers);
            if (_profile.enabled)
                _profile.allocations += 2;
        }
        auto& page = frame.transientPages[frame.transientPage];
        const VkDeviceSize vertexOffset = (page.vertexUsed + 3) & ~VkDeviceSize(3);
        const VkDeviceSize indexOffset = (page.indexUsed + 3) & ~VkDeviceSize(3);
        if (vertexOffset > page.buffers->vertices.size || indexOffset > page.buffers->indices.size ||
            vertexBytes > page.buffers->vertices.size - vertexOffset ||
            indexBytes > page.buffers->indices.size - indexOffset)
            continue;
        UploadMappedBuffer(page.buffers->vertices, vertices, vertexBytes, vertexOffset);
        UploadMappedBuffer(page.buffers->indices, indices, indexBytes, indexOffset);
        page.vertexUsed = vertexOffset + vertexBytes;
        page.indexUsed = indexOffset + indexBytes;
        if (_profile.enabled)
        {
            ++_profile.transient;
            _profile.geometryMs += ProfileClock() - started;
        }
        return {page.buffers, vertexOffset, indexOffset};
    }
}

void VulkanContext::CreateShapePipeline(bool translucent, bool screen, bool depthTest, bool depthWrite)
{
    if (!_shapeLayout)
    {
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 112};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        CreateTextureLayout();
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_physical, &properties);
        _uniformAlignment = uint32_t(std::max<VkDeviceSize>(16, properties.limits.minUniformBufferOffsetAlignment));
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo uniforms{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        uniforms.bindingCount = 1;
        uniforms.pBindings = &binding;
        Require(vkCreateDescriptorSetLayout(_device, &uniforms, nullptr, &_lightingLayout), "create lighting layout");
        const VkDescriptorSetLayout sets[] = {_textureLayout, _textureLayout, _lightingLayout};
        layout.setLayoutCount = 3;
        layout.pSetLayouts = sets;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        Require(vkCreatePipelineLayout(_device, &layout, nullptr, &_shapeLayout), "create pipeline layout");
    }
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    try
    {
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = screen ? sizeof(Cwrscreen_vert) : sizeof(Cwrshape_vert);
        shader.pCode = screen ? Cwrscreen_vert : Cwrshape_vert;
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
        // GL33 SVertex-compatible position/negated-normal/UV packing.
        const VkVertexInputBindingDescription binding{0, 8 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attributes[] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                                {1, 0, VK_FORMAT_R32G32_SFLOAT, 6 * sizeof(float)},
                                                                {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)}};
        const VkVertexInputBindingDescription screenBinding{0, 11 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription screenAttributes[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 6 * sizeof(float)},
            {3, 0, VK_FORMAT_R32_SFLOAT, 10 * sizeof(float)}};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = screen ? &screenBinding : &binding;
        input.vertexAttributeDescriptionCount = screen ? 4 : 3;
        input.pVertexAttributeDescriptions = screen ? screenAttributes : attributes;
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
        depth.depthTestEnable = depthTest;
        depth.depthWriteEnable = depthTest && depthWrite;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable = translucent;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
        Require(vkCreateGraphicsPipelines(
                    _device, VK_NULL_HANDLE, 1, &pipeline, nullptr,
                    screen        ? &_screenPipelines[ScreenPipelineIndex(depthTest, translucent, depthWrite)]
                                  : &_shapePipelines[ScreenPipelineIndex(depthTest, translucent, depthWrite)]),
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
    std::fprintf(stderr, "Vulkan: %s pipeline ready for swapchain #%u (%s, depth %s)\n", screen ? "2D" : "Shape",
                 _swapchainGeneration, translucent ? "alpha blend" : "opaque", depthTest ? "tested" : "disabled");
}

void VulkanContext::DrawMesh(const std::shared_ptr<MeshBuffers>& mesh, uint32_t firstIndex, uint32_t count,
                             bool index16, const std::array<float, 16>& mvp, const std::array<float, 4>& color,
                             const std::shared_ptr<TextureImage>& texture, unsigned sampler, float alphaCutoff,
                             bool blend, bool screen, bool depthTest, const VkRect2D* clip,
                             const std::shared_ptr<TextureImage>& detail, float secondaryMode,
                             const std::array<float, 3>& lightDirection, VkDeviceSize vertexOffset,
                             VkDeviceSize indexOffset, bool depthWrite, const ShapeLighting* lighting)
{
    if (!_frameOpen || !mesh || mesh->device != _device || !mesh->vertices.buffer || !mesh->indices.buffer)
        throw std::logic_error("Vulkan Shape: indexed draw needs an open frame and live buffers from this device");
    const VkDeviceSize indexSize = index16 ? 2 : 4;
    if (indexOffset > mesh->indices.size || vertexOffset >= mesh->vertices.size || indexOffset % indexSize ||
        VkDeviceSize(firstIndex) + count > (mesh->indices.size - indexOffset) / indexSize || count % 3 != 0)
        throw std::out_of_range("Vulkan Shape: indexed draw exceeds geometry or is not a triangle list");
    if (!count)
        return;
    if ((!texture || !detail) && !_whiteTexture)
    {
        const uint32_t white = 0xffffffff;
        _whiteTexture = UploadTexture(1, 1, &white);
    }
    const auto& sampled = texture ? texture : _whiteTexture;
    const auto& sampledDetail = detail ? detail : _whiteTexture;
    if (sampled->device != _device || sampledDetail->device != _device)
        throw std::logic_error("Vulkan Shape: texture is not live on this device");
    if (sampler >= 8)
        throw std::out_of_range("Vulkan Shape: sampler index");
    auto& pipeline = screen  ? _screenPipelines[ScreenPipelineIndex(depthTest, blend, depthWrite)]
                             : _shapePipelines[ScreenPipelineIndex(depthTest, blend, depthWrite)];
    if (!pipeline)
        CreateShapePipeline(blend, screen, depthTest, depthWrite);
    if (lighting)
    {
        BindLighting(*lighting, false);
        if (_profile.enabled)
        {
            ++_profile.litDraws;
            _profile.localLights += uint64_t(lighting->localCount[0]);
            _profile.fogRange = {lighting->fogParams[0],
                                 lighting->fogParams[0] +
                                     (lighting->fogParams[1] > 0 ? 1 / lighting->fogParams[1] : 0)};
        }
    }
    else
    {
        ShapeLighting unlit;
        unlit.fogColor = _fogColor;
        unlit.eyeCoef = _eyeCoef;
        BindLighting(unlit, true);
    }
    auto& frame = _frames[_frame];
    // Retain each mesh once per frame until that frame's submission fence completes.
    if (std::find(frame.meshes.begin(), frame.meshes.end(), mesh) == frame.meshes.end())
        frame.meshes.push_back(mesh);
    if (std::find(frame.textures.begin(), frame.textures.end(), sampled) == frame.textures.end())
        frame.textures.push_back(sampled);
    if (std::find(frame.textures.begin(), frame.textures.end(), sampledDetail) == frame.textures.end())
        frame.textures.push_back(sampledDetail);
    vkCmdBindPipeline(frame.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDescriptorSet descriptors[] = {sampled->descriptors[sampler], sampledDetail->descriptors[0]};
    vkCmdBindDescriptorSets(frame.command, VK_PIPELINE_BIND_POINT_GRAPHICS, _shapeLayout, 0, 2, descriptors, 0,
                            nullptr);
    vkCmdBindVertexBuffers(frame.command, 0, 1, &mesh->vertices.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(frame.command, mesh->indices.buffer, indexOffset,
                         index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    const VkViewport viewport{0, 0, float(_extent.width), float(_extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, _extent};
    vkCmdSetViewport(frame.command, 0, 1, &viewport);
    vkCmdSetScissor(frame.command, 0, 1, clip ? clip : &scissor);
    std::array<float, 28> constants{};
    std::copy(mvp.begin(), mvp.end(), constants.begin());
    std::copy(color.begin(), color.end(), constants.begin() + 16);
    constants[20] = alphaCutoff;
    constants[21] = 1 / _gamma;
    constants[22] = detail ? secondaryMode : 0;
    std::copy(lightDirection.begin(), lightDirection.end(), constants.begin() + 24);
    vkCmdPushConstants(frame.command, _shapeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(constants), constants.data());
    vkCmdDrawIndexed(frame.command, count, 1, firstIndex, 0, 0);
    if (!_loggedShape)
    {
        std::fprintf(stderr, "Vulkan: first production %s draw: firstIndex=%u count=%u\n", screen ? "2D" : "Shape",
                     firstIndex, count);
        _loggedShape = true;
    }
}

void VulkanContext::BindLighting(const ShapeLighting& lighting, bool screen)
{
    auto& frame = _frames[_frame];
    auto& cache = screen ? frame.screenUniform : frame.nativeUniform;
    if (cache.set && std::memcmp(&cache.value, &lighting, sizeof(lighting)) == 0)
    {
        vkCmdBindDescriptorSets(frame.command, VK_PIPELINE_BIND_POINT_GRAPHICS, _shapeLayout, 2, 1, &cache.set, 1,
                                &cache.offset);
        return;
    }
    for (;; ++frame.uniformPage)
    {
        if (frame.uniformPage == frame.uniforms.size())
        {
            // Frame-fenced mapped pages: append on overflow, never overwrite a
            // recorded draw. No per-section allocation or synchronous upload.
            auto& page = frame.uniforms.emplace_back();
            Require(CreateHostVisibleBuffer(_physical, _device, 1024 * 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                            page.buffer),
                    "allocate lighting page");
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool.maxSets = pool.poolSizeCount = 1;
            pool.pPoolSizes = &size;
            Require(vkCreateDescriptorPool(_device, &pool, nullptr, &page.pool), "create lighting pool");
            VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocation.descriptorPool = page.pool;
            allocation.descriptorSetCount = 1;
            allocation.pSetLayouts = &_lightingLayout;
            Require(vkAllocateDescriptorSets(_device, &allocation, &page.set), "allocate lighting set");
            const VkDescriptorBufferInfo buffer{page.buffer.buffer, 0, sizeof(ShapeLighting)};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = page.set;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            write.pBufferInfo = &buffer;
            vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
            if (_profile.enabled)
                ++_profile.allocations;
        }
        auto& page = frame.uniforms[frame.uniformPage];
        const uint32_t offset = (page.used + _uniformAlignment - 1) & ~(_uniformAlignment - 1);
        if (offset + sizeof(lighting) > page.buffer.size)
            continue;
        UploadMappedBuffer(page.buffer, &lighting, sizeof(lighting), offset);
        page.used = offset + sizeof(lighting);
        cache = {lighting, page.set, offset};
        vkCmdBindDescriptorSets(frame.command, VK_PIPELINE_BIND_POINT_GRAPHICS, _shapeLayout, 2, 1, &page.set, 1,
                                &offset);
        return;
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
