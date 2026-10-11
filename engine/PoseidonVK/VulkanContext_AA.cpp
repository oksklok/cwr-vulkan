#include <PoseidonVK/VulkanContext.hpp>
#include <PoseidonVK/Shaders/gamma.vert.hpp>
#include <PoseidonVK/Shaders/fxaa.frag.hpp>
#include <PoseidonVK/Shaders/smaa_edge.frag.hpp>
#include <PoseidonVK/Shaders/smaa_weight.frag.hpp>
#include <PoseidonVK/Shaders/smaa_blend.frag.hpp>
#include <PoseidonVK/Shaders/aa_composite.frag.hpp>
#include <PoseidonVK/Shaders/aa_composite_ms.frag.hpp>
#include <PoseidonVK/Shaders/taa.frag.hpp>
#include <PoseidonVK/ThirdParty/SMAA/AreaTex.h>
#include <PoseidonVK/ThirdParty/SMAA/SearchTex.h>
#include <cstdio>
#include <stdexcept>

namespace Poseidon::vk
{
namespace
{
void Require(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan AA: ") + operation + " failed (" + std::to_string(result) + ")");
}
} // namespace

std::string VulkanContext::SetAntiAliasing(std::string_view name)
{
    if (name == "motion" || name == "motion-off")
    {
        _motionDebug = name == "motion";
        ResetTemporalHistory();
        return "OK: motion debug changed (requires taa)";
    }
    if (name == "status")
        return std::string("active=") + AAName(_aaMode) + " requested=" + AAName(_requestedAA) +
               " scale=" + std::to_string(_renderScale) + " requestedScale=" + std::to_string(_requestedScale) +
               " sampleMask=" + std::to_string(_aaSampleSupport);
    AAMode mode;
    if (!ParseAA(name, mode))
        return "INVALID: off, fxaa, smaa, taa, msaa2, msaa4, msaa8, motion, motion-off, status";
    if (mode == AAMode::TAA && _physical)
    {
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(_physical, &features);
        if (!features.independentBlend) return "UNSUPPORTED: TAA requires independent attachment blending";
    }
    if (!(_aaSampleSupport & AASamples(mode)))
        return "UNSUPPORTED: requested color/depth sample count; previous mode retained";
    if (_requestedAA != mode)
    {
        _requestedAA = mode;
        _recreate = true;
    }
    return std::string("OK: ") + AAName(mode) + " (next frame)";
}

bool VulkanContext::SetRenderScale(int percent)
{
    if (!ValidRenderScale(percent) || !_physical)
        return false;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(_physical, &properties);
    const auto size = ScaledExtent(_extent, percent);
    if (size.width > properties.limits.maxFramebufferWidth || size.height > properties.limits.maxFramebufferHeight)
        return false;
    if (_requestedScale != percent)
    {
        _requestedScale = percent;
        _recreate = true;
    }
    return true;
}

void VulkanContext::CreateAAImage(DepthAttachment& target, VkFormat format, VkImageUsageFlags usage,
                                  VkSampleCountFlagBits samples, VkImageAspectFlags aspect)
{
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format;
    image.extent = {_worldExtent.width, _worldExtent.height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = samples;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = usage;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Require(vkCreateImage(_device, &image, nullptr, &target.image), "create world image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(_device, target.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(_physical, &memory);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        FindMemoryType(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX)
        throw std::runtime_error("Vulkan AA: no device-local memory");
    Require(vkAllocateMemory(_device, &allocation, nullptr, &target.memory), "allocate world image");
    Require(vkBindImageMemory(_device, target.image, target.memory, 0), "bind world image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = target.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    Require(vkCreateImageView(_device, &view, nullptr, &target.view), "create world view");
}

void VulkanContext::CreateAAResources(VkFormat format)
{
    constexpr auto colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    constexpr auto depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageFormatProperties colorProperties{}, depthProperties{};
    Require(vkGetPhysicalDeviceImageFormatProperties(_physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                     colorUsage, 0, &colorProperties),
            "query world color samples");
    Require(vkGetPhysicalDeviceImageFormatProperties(_physical, _depthFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                     depthUsage, 0, &depthProperties),
            "query world depth samples");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(_physical, &properties);
    _aaSampleSupport = colorProperties.sampleCounts & depthProperties.sampleCounts &
                       properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts &
                       properties.limits.framebufferStencilSampleCounts &
                       properties.limits.sampledImageDepthSampleCounts;
    _aaMode = _requestedAA;
    _renderScale = _requestedScale;
    _worldSamples = AASamples(_aaMode);
    if (!(_aaSampleSupport & _worldSamples))
        throw std::runtime_error("Vulkan AA: requested sample count became unsupported");
    _worldExtent = ScaledExtent(_extent, _renderScale);
    if (_worldExtent.width > properties.limits.maxFramebufferWidth ||
        _worldExtent.height > properties.limits.maxFramebufferHeight)
        throw std::runtime_error("Vulkan AA: scaled drawable exceeds framebuffer limits");
    std::fprintf(stderr, "Vulkan AA: mode=%s scale=%d world=%ux%u samples=%u supported=%u\n", AAName(_aaMode),
                 _renderScale, _worldExtent.width, _worldExtent.height, _worldSamples, _aaSampleSupport);
    if (_aaMode == AAMode::Off && _renderScale == 100)
        return;

    VkAttachmentDescription attachments[3]{};
    auto& color = attachments[0];
    color.format = format;
    color.samples = _worldSamples;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    auto& depth = attachments[1];
    depth = color;
    depth.format = _depthFormat;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    attachments[2] = color;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    const bool temporal = _aaMode == AAMode::TAA;
    if (temporal)
    {
        attachments[2].format = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    }
    const VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    const VkAttachmentReference temporalRefs[]{colorRef, {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    if (temporal)
    {
        subpass.colorAttachmentCount = 2;
        subpass.pColorAttachments = temporalRefs;
    }
    subpass.pDepthStencilAttachment = &depthRef;
    subpass.pResolveAttachments = _worldSamples != VK_SAMPLE_COUNT_1_BIT ? &resolveRef : nullptr;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1] = dependencies[0];
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = temporal || _worldSamples != VK_SAMPLE_COUNT_1_BIT ? 3 : 2;
    pass.pAttachments = attachments;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 2;
    pass.pDependencies = dependencies;
    Require(vkCreateRenderPass(_device, &pass, nullptr, &_worldPass), "create world pass");
    color.loadOp = depth.loadOp = depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.initialLayout = color.finalLayout;
    depth.initialLayout = depth.finalLayout;
    if (temporal)
    {
        attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[2].initialLayout = attachments[2].finalLayout;
    }
    Require(vkCreateRenderPass(_device, &pass, nullptr, &_worldResume), "create world resume");
    // SMAA's discard in edge detection requires a zero-cleared intermediate.
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pass.attachmentCount = 1;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pResolveAttachments = nullptr;
    subpass.pDepthStencilAttachment = nullptr;
    Require(vkCreateRenderPass(_device, &pass, nullptr, &_aaPass), "create AA pass");
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    Require(vkCreateRenderPass(_device, &pass, nullptr, &_aaDataPass), "create AA weights pass");
    if (temporal)
    {
        color.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        Require(vkCreateRenderPass(_device, &pass, nullptr, &_taaPass), "create temporal history pass");
    }

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Require(vkCreateSampler(_device, &sampler, nullptr, &_aaSampler), "create AA sampler");
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(_images.size() * 6 + 4)};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = size.descriptorCount;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    Require(vkCreateDescriptorPool(_device, &pool, nullptr, &_aaPool), "create AA pool");
    auto descriptor = [&](VkImageView view, bool isDepth, VkDescriptorSet& set)
    {
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = _aaPool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &_textureLayout;
        Require(vkAllocateDescriptorSets(_device, &allocation, &set), "allocate AA descriptor");
        const VkDescriptorImageInfo image{isDepth ? _textureSamplers[7] : _aaSampler, view,
                                          isDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
    };
    auto framebuffer = [&](VkRenderPass renderPass, const VkImageView* views, uint32_t count, VkFramebuffer& result)
    {
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = renderPass;
        info.attachmentCount = count;
        info.pAttachments = views;
        info.width = _worldExtent.width;
        info.height = _worldExtent.height;
        info.layers = 1;
        Require(vkCreateFramebuffer(_device, &info, nullptr, &result), "create AA framebuffer");
    };
    _aaTargets.resize(_images.size());
    for (auto& target : _aaTargets)
    {
        CreateAAImage(target.color, format, colorUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        CreateAAImage(target.depth, _depthFormat, depthUsage, _worldSamples,
                      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = target.depth.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = _depthFormat;
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        Require(vkCreateImageView(_device, &view, nullptr, &target.sampledDepth), "create sampled world depth");
        descriptor(target.color.view, false, target.colorSet);
        descriptor(target.sampledDepth, true, target.depthSet);
        VkImageView views[]{target.color.view, target.depth.view, target.color.view};
        if (temporal)
        {
            CreateAAImage(target.motion, VK_FORMAT_R16G16B16A16_SFLOAT, colorUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            descriptor(target.motion.view, false, target.motionSet);
            views[2] = target.motion.view;
        }
        if (_worldSamples != VK_SAMPLE_COUNT_1_BIT)
        {
            CreateAAImage(target.multisample, format, colorUsage, _worldSamples, VK_IMAGE_ASPECT_COLOR_BIT);
            views[0] = target.multisample.view;
        }
        framebuffer(_worldPass, views, temporal || _worldSamples != VK_SAMPLE_COUNT_1_BIT ? 3 : 2, target.scene);
        framebuffer(_ssaoPass, &target.color.view, 1, target.ao);
        if (_renderScale != 100 && (_aaMode == AAMode::FXAA || _aaMode == AAMode::SMAA))
        {
            CreateAAImage(target.filtered, format, colorUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            descriptor(target.filtered.view, false, target.filteredSet);
            framebuffer(_aaPass, &target.filtered.view, 1, target.filterFB);
        }
        if (_aaMode == AAMode::SMAA)
        {
            CreateAAImage(target.edges, VK_FORMAT_R8G8B8A8_UNORM, colorUsage, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT);
            CreateAAImage(target.weights, VK_FORMAT_R8G8B8A8_UNORM, colorUsage, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT);
            descriptor(target.edges.view, false, target.edgesSet);
            descriptor(target.weights.view, false, target.weightsSet);
            framebuffer(_aaDataPass, &target.edges.view, 1, target.edgeFB);
            framebuffer(_aaDataPass, &target.weights.view, 1, target.weightFB);
        }
    }
    if (temporal)
        for (auto& history : _taaHistory)
        {
            CreateAAImage(history.color, VK_FORMAT_R16G16B16A16_SFLOAT, colorUsage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            descriptor(history.color.view, false, history.set);
            framebuffer(_taaPass, &history.color.view, 1, history.framebuffer);
        }
    if (_aaMode == AAMode::SMAA && !_smaaArea)
    {
        std::vector<unsigned char> bytes(AREATEX_WIDTH * AREATEX_HEIGHT * 4, 255);
        for (size_t i = 0; i < AREATEX_WIDTH * AREATEX_HEIGHT; ++i)
        {
            bytes[4 * i] = areaTexBytes[2 * i];
            bytes[4 * i + 1] = areaTexBytes[2 * i + 1];
        }
        _smaaArea = UploadTexture(AREATEX_WIDTH, AREATEX_HEIGHT, bytes.data());
        bytes.assign(SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT * 4, 255);
        for (size_t i = 0; i < SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT; ++i)
            bytes[4 * i] = searchTexBytes[i];
        _smaaSearch = UploadTexture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, bytes.data());
    }
    if (_aaMode == AAMode::SMAA)
    {
        // Reference lookup tables require bilinear clamp without anisotropy.
        descriptor(_smaaArea->view, false, _smaaAreaSet);
        descriptor(_smaaSearch->view, false, _smaaSearchSet);
    }
    const VkDescriptorSetLayout sets[]{_textureLayout, _textureLayout, _textureLayout, _textureLayout};
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 * sizeof(float)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 4;
    layout.pSetLayouts = sets;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    Require(vkCreatePipelineLayout(_device, &layout, nullptr, &_aaLayout), "create AA layout");
    const uint32_t* codes[]{Cwrfxaa_frag, Cwrsmaa_edge_frag, Cwrsmaa_weight_frag, Cwrsmaa_blend_frag,
                            _worldSamples == VK_SAMPLE_COUNT_1_BIT ? Cwraa_composite_frag : Cwraa_composite_ms_frag, Cwrtaa_frag};
    const size_t sizes[]{
        sizeof(Cwrfxaa_frag), sizeof(Cwrsmaa_edge_frag), sizeof(Cwrsmaa_weight_frag), sizeof(Cwrsmaa_blend_frag),
        _worldSamples == VK_SAMPLE_COUNT_1_BIT ? sizeof(Cwraa_composite_frag) : sizeof(Cwraa_composite_ms_frag), sizeof(Cwrtaa_frag)};
    for (unsigned i = 0; i < 6; ++i)
    {
        if (i == 5 && !temporal) continue;
        if (i == 0 && _aaMode != AAMode::FXAA)
            continue;
        if (i > 0 && i < 4 && _aaMode != AAMode::SMAA)
            continue;
        VkShaderModule vertex{}, fragment{};
        try
        {
            VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            shader.codeSize = sizeof(Cwrgamma_vert);
            shader.pCode = Cwrgamma_vert;
            Require(vkCreateShaderModule(_device, &shader, nullptr, &vertex), "create AA vertex shader");
            shader.codeSize = sizes[i];
            shader.pCode = codes[i];
            Require(vkCreateShaderModule(_device, &shader, nullptr, &fragment), "create AA fragment shader");
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[0].module = vertex;
            stages[1].module = fragment;
            stages[0].pName = stages[1].pName = "main";
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
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
            output.colorWriteMask = 15;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1;
            blend.pAttachments = &output;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            const bool composite = i == 4 || (_renderScale == 100 && (i == 0 || i == 3));
            ds.depthTestEnable = ds.depthWriteEnable = composite;
            ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
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
            pipeline.pDepthStencilState = &ds;
            pipeline.pDynamicState = &dynamic;
            pipeline.layout = _aaLayout;
            pipeline.renderPass = composite ? _resumePass : i == 5 ? _taaPass : (i == 1 || i == 2) ? _aaDataPass : _aaPass;
            Require(vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &_aaPipelines[i]),
                    "create AA pipeline");
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
}

void VulkanContext::BeginAAWorld()
{
    if (!_frameOpen || _aaTargets.empty())
        return;
    if (_worldActive)
        throw std::logic_error("Vulkan AA: nested world");
    FlushScreenBatch();
    auto command = _frames[_frame].command;
    vkCmdEndRenderPass(command);
    if (_aaMode == AAMode::TAA)
    {
        if (!_taaInitialized)
        {
            VkImageMemoryBarrier barriers[2]{};
            for (int i = 0; i < 2; ++i)
            {
                auto& b = barriers[i];
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = _taaHistory[i].color.image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            }
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0,0,nullptr,0,nullptr,2,barriers);
            _taaInitialized = true;
        }
        auto halton = [](unsigned index, unsigned base)
        {
            float result=0, f=1;
            while(index) { f/=base; result+=f*(index%base); index/=base; }
            return result;
        };
        const unsigned sample = (++_taaFrame % 8) + 1;
        _taaJitter = {2*(halton(sample,2)-.5f)/_worldExtent.width, 2*(halton(sample,3)-.5f)/_worldExtent.height};
    }
    _worldActive = _worldPassOpen = true;
    _commands = {};
    VkClearValue clears[3]{};
    std::copy(_clearColor.begin(), _clearColor.end(), clears[0].color.float32);
    clears[1].depthStencil.depth = 1;
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = _worldPass;
    pass.framebuffer = SceneFramebuffer();
    pass.renderArea.extent = _worldExtent;
    pass.clearValueCount = _aaMode == AAMode::TAA ? 3 : 2;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanContext::DrawAAPass(unsigned index, VkFramebuffer framebuffer, VkDescriptorSet source, VkExtent2D extent)
{
    auto command = _frames[_frame].command;
    const auto& target = _aaTargets[_image];
    const bool composite = index == 4 || (_renderScale == 100 && (index == 0 || index == 3));
    VkClearValue clear{};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = composite ? _resumePass : index == 5 ? _taaPass : (index == 1 || index == 2) ? _aaDataPass : _aaPass;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent = extent;
    pass.clearValueCount = composite ? 0 : 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _aaPipelines[index]);
    const VkDescriptorSet sets[]{source, index == 3 ? target.weightsSet : target.depthSet,
                                 index == 5 ? target.motionSet : index == 2 ? _smaaAreaSet : target.depthSet,
                                 index == 5 ? _taaHistory[1-_taaIndex].set : index == 2 ? _smaaSearchSet : source};
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _aaLayout, 0, 4, sets, 0, nullptr);
    const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    float push[]{1.f / _worldExtent.width,   1.f / _worldExtent.height, float(_worldExtent.width),
                       float(_worldExtent.height), 1.f / _extent.width,       1.f / _extent.height,
                       float(_extent.width),       float(_extent.height)};
    if (index == 5)
    {
        push[4]=_taaProjection[2]; push[5]=_taaProjection[3];
        push[6]=_taaValid ? 1.f : 0.f; push[7]=_motionDebug ? 1.f : 0.f;
    }
    vkCmdPushConstants(command, _aaLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(command, 3, 1, 0, 0);
    // Composite leaves the native-resolution pass open for cockpit/HUD/UI.
    if (!composite)
        vkCmdEndRenderPass(command);
    _commands = {};
}

void VulkanContext::FinishAAWorld(const std::array<float, 4>& projection)
{
    DrawSSAO(projection);
    if (!_worldActive)
        return;
    FlushScreenBatch();
    auto command = _frames[_frame].command;
    if (_worldPassOpen)
        vkCmdEndRenderPass(command);
    _worldPassOpen = false;
    auto& target = _aaTargets[_image];
    VkDescriptorSet source = target.colorSet;
    if (_aaMode == AAMode::TAA)
    {
        _taaProjection = projection;
        auto& frame = _frames[_frame];
        if (frame.frameQueries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.frameQueries, 2);
        DrawAAPass(5, _taaHistory[_taaIndex].framebuffer, source, _worldExtent);
        if (frame.frameQueries)
        {
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.frameQueries, 3);
            frame.taaTimestamped = true;
        }
        source = _taaHistory[_taaIndex].set;
        _taaIndex = 1-_taaIndex;
        _taaValid = true;
    }
    const auto destination = _renderScale == 100 ? _framebuffers[_image] : target.filterFB;
    if (_aaMode == AAMode::FXAA)
    {
        DrawAAPass(0, destination, source, _worldExtent);
        source = target.filteredSet;
    }
    else if (_aaMode == AAMode::SMAA)
    {
        DrawAAPass(1, target.edgeFB, source, _worldExtent);
        DrawAAPass(2, target.weightFB, target.edgesSet, _worldExtent);
        DrawAAPass(3, destination, source, _worldExtent);
        source = target.filteredSet;
    }
    if (_renderScale != 100 || (_aaMode != AAMode::FXAA && _aaMode != AAMode::SMAA))
        DrawAAPass(4, _framebuffers[_image], source, _extent);
    _worldActive = false;
}

void VulkanContext::DestroyAAResources() noexcept
{
    _worldActive = _worldPassOpen = false;
    _taaValid = _taaInitialized = false;
    _taaIndex = _taaFrame = 0;
    auto destroyPipelines = [&](auto& pipelines)
    {
        for (auto& p : pipelines)
        {
            if (p)
                vkDestroyPipeline(_device, p, nullptr);
            p = VK_NULL_HANDLE;
        }
    };
    destroyPipelines(_aaPipelines);
    destroyPipelines(_worldShapePipelines);
    destroyPipelines(_worldScreenPipelines);
    destroyPipelines(_worldShadowPipelines);
    if (_aaLayout)
        vkDestroyPipelineLayout(_device, _aaLayout, nullptr);
    if (_aaPool)
        vkDestroyDescriptorPool(_device, _aaPool, nullptr);
    if (_aaSampler)
        vkDestroySampler(_device, _aaSampler, nullptr);
    _aaLayout = VK_NULL_HANDLE;
    _aaPool = VK_NULL_HANDLE;
    _aaSampler = VK_NULL_HANDLE;
    _smaaAreaSet = _smaaSearchSet = VK_NULL_HANDLE;
    for (auto& target : _aaTargets)
    {
        for (auto fb : {target.scene, target.ao, target.edgeFB, target.weightFB, target.filterFB})
            if (fb)
                vkDestroyFramebuffer(_device, fb, nullptr);
        if (target.sampledDepth)
            vkDestroyImageView(_device, target.sampledDepth, nullptr);
        for (auto* image :
             {&target.color, &target.depth, &target.multisample, &target.edges, &target.weights, &target.filtered, &target.motion})
        {
            if (image->view)
                vkDestroyImageView(_device, image->view, nullptr);
            if (image->image)
                vkDestroyImage(_device, image->image, nullptr);
            if (image->memory)
                vkFreeMemory(_device, image->memory, nullptr);
        }
    }
    _aaTargets.clear();
    for (auto& history : _taaHistory)
    {
        if (history.framebuffer) vkDestroyFramebuffer(_device,history.framebuffer,nullptr);
        if (history.color.view) vkDestroyImageView(_device,history.color.view,nullptr);
        if (history.color.image) vkDestroyImage(_device,history.color.image,nullptr);
        if (history.color.memory) vkFreeMemory(_device,history.color.memory,nullptr);
        history = {};
    }
    for (auto pass : {_worldPass, _worldResume, _aaPass, _aaDataPass, _taaPass})
        if (pass)
            vkDestroyRenderPass(_device, pass, nullptr);
    _worldPass = _worldResume = _aaPass = _aaDataPass = VK_NULL_HANDLE;
    _taaPass = VK_NULL_HANDLE;
}
} // namespace Poseidon::vk
