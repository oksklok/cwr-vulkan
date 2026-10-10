#include <PoseidonVK/VulkanContext.hpp>

#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <type_traits>
#include <cstdlib>
#include <numeric>

namespace Poseidon::vk
{
namespace
{
void Check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan: ") + operation + " failed (VkResult " +
                                 std::to_string(static_cast<int>(result)) + ")");
}

// Two-call Vulkan enumerations can grow between calls. Retry VK_INCOMPLETE.
template <class T, class Enumerate>
std::vector<T> EnumerateList(Enumerate enumerate, const char* operation)
{
    for (;;)
    {
        uint32_t count = 0;
        Check(enumerate(&count, nullptr), operation);
        if (!count)
            return {};
        std::vector<T> values(count);
        const VkResult result = enumerate(&count, values.data());
        if (result == VK_INCOMPLETE)
            continue;
        Check(result, operation);
        values.resize(count);
        return values;
    }
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    for (const auto& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;
    return false;
}

template <class Handle>
uint64_t ObjectHandle(Handle handle)
{
    if constexpr (std::is_pointer_v<Handle>)
        return reinterpret_cast<uint64_t>(handle);
    else
        return static_cast<uint64_t>(handle); // Non-dispatchable handles are integers on 32-bit targets.
}
} // namespace

VulkanContext::~VulkanContext()
{
    Shutdown();
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::ValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                                VkDebugUtilsMessageTypeFlagsEXT types,
                                                                const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                                void* user)
{
    auto& context = *static_cast<VulkanContext*>(user);
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    const bool validation = (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0;
    if (validation)
    {
        if (error)
            ++context._validationErrors;
        else
            ++context._validationWarnings;
    }
    std::fprintf(stderr, "Vulkan %s %s [%s]: %s\n", validation ? "validation" : "loader", error ? "ERROR" : "WARNING",
                 data->pMessageIdName ? data->pMessageIdName : "unknown",
                 data->pMessage ? data->pMessage : "no message");
    return VK_FALSE;
}

void VulkanContext::CreateInstance(const char* const* extensions, uint32_t count, bool validation)
{
    _profile.enabled = std::getenv("CWR_VK_PROFILE") != nullptr;
    if (_instance)
        throw std::logic_error("Vulkan: instance already created");
    if (!extensions || !count)
        throw std::runtime_error("Vulkan: SDL supplied no surface extensions");
    auto available = EnumerateList<VkExtensionProperties>(
        [](uint32_t* n, VkExtensionProperties* p) { return vkEnumerateInstanceExtensionProperties(nullptr, n, p); },
        "enumerate instance extensions");
    std::vector<const char*> enabled(extensions, extensions + count);
    for (const char* extension : enabled)
        if (!HasExtension(available, extension))
            throw std::runtime_error(std::string("Vulkan: missing SDL surface extension ") + extension);
    // portability_subset needs this instance dependency with our Vulkan 1.0 baseline.
    if (HasExtension(available, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) &&
        std::none_of(enabled.begin(), enabled.end(), [](const char* name)
                     { return std::strcmp(name, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0; }))
        enabled.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (HasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    {
        enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        _debugNamesEnabled = true;
    }

    VkInstanceCreateFlags flags = 0;
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    if (HasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "CWR Remastered";
    app.pEngineName = "PoseidonVK (diagnostic indexed rendering)";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.flags = flags;
    info.pApplicationInfo = &app;
    const char* layerName = "VK_LAYER_KHRONOS_validation";
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = &ValidationMessage;
    debug.pUserData = this;
    VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    const VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    bool syncValidation = false;
    if (validation)
    {
        auto layers = EnumerateList<VkLayerProperties>([](uint32_t* n, VkLayerProperties* p)
                                                       { return vkEnumerateInstanceLayerProperties(n, p); },
                                                       "enumerate validation layers");
        _validationEnabled = std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& layer)
                                         { return std::strcmp(layer.layerName, layerName) == 0; });
        if (_validationEnabled)
        {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &layerName;
            auto layerExtensions = EnumerateList<VkExtensionProperties>(
                [&](uint32_t* n, VkExtensionProperties* p)
                { return vkEnumerateInstanceExtensionProperties(layerName, n, p); }, "enumerate layer extensions");
            if (HasExtension(layerExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME))
            {
                enabled.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                features.enabledValidationFeatureCount = 1;
                features.pEnabledValidationFeatures = &sync;
                info.pNext = &features;
                syncValidation = true;
            }
        }
        else
            throw std::runtime_error("Vulkan: --vk-validation requires VK_LAYER_KHRONOS_validation; "
                                     "install/enable the Khronos validation layer or omit --vk-validation");
    }
    if (_debugNamesEnabled)
    {
        debug.pNext = info.pNext;
        info.pNext = &debug;
    }
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    // Validation may be enabled externally with VK_INSTANCE_LAYERS.
    Check(vkCreateInstance(&info, nullptr, &_instance), "create instance");
    if (_debugNamesEnabled)
    {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (!create)
            throw std::runtime_error("Vulkan: debug-utils messenger entry point unavailable");
        debug.pNext = nullptr;
        Check(create(_instance, &debug, nullptr, &_debugMessenger), "create validation messenger");
    }
    std::fprintf(stderr, "Vulkan: instance created (API 1.0); validation=%s, synchronization validation=%s\n",
                 _validationEnabled ? "on" : "off", syncValidation ? "on" : "off");
}

void VulkanContext::SelectDevice()
{
    auto devices = EnumerateList<VkPhysicalDevice>([&](uint32_t* n, VkPhysicalDevice* p)
                                                   { return vkEnumeratePhysicalDevices(_instance, n, p); },
                                                   "enumerate physical devices");
    int bestScore = -1;
    for (VkPhysicalDevice physical : devices)
    {
        auto extensions = EnumerateList<VkExtensionProperties>(
            [&](uint32_t* n, VkExtensionProperties* p)
            { return vkEnumerateDeviceExtensionProperties(physical, nullptr, n, p); }, "enumerate device extensions");
        if (!HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            continue;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> properties(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, properties.data());
        properties.resize(count);
        std::vector<VkBool32> present(count);
        for (uint32_t i = 0; i < count; ++i)
            Check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, _surface, &present[i]), "query present support");
        const auto families = ChooseQueues(properties, present);
        if (!families.Complete())
            continue;
        VkSurfaceCapabilitiesKHR caps{};
        Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, _surface, &caps), "query surface capabilities");
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            continue;
        auto formats = EnumerateList<VkSurfaceFormatKHR>(
            [&](uint32_t* n, VkSurfaceFormatKHR* p)
            { return vkGetPhysicalDeviceSurfaceFormatsKHR(physical, _surface, n, p); }, "query surface formats");
        auto modes = EnumerateList<VkPresentModeKHR>(
            [&](uint32_t* n, VkPresentModeKHR* p)
            { return vkGetPhysicalDeviceSurfacePresentModesKHR(physical, _surface, n, p); }, "query present modes");
        if (formats.empty() || modes.empty())
            continue;
        VkPhysicalDeviceProperties deviceProperties{};
        vkGetPhysicalDeviceProperties(physical, &deviceProperties);
        int score = 10;
        if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score = 1000;
        else if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score = 500;
        if (families.graphics == families.present)
            score += 1;
        if (score > bestScore)
        {
            bestScore = score;
            _physical = physical;
            _families = families;
            _deviceName = deviceProperties.deviceName;
            std::fprintf(stderr, "Vulkan: candidate '%s', api=%u, driver=%u, queues=%u/%u\n",
                         deviceProperties.deviceName, deviceProperties.apiVersion, deviceProperties.driverVersion,
                         families.graphics, families.present);
        }
    }
    if (!_physical)
        throw std::runtime_error("Vulkan: no device supports graphics, presentation and a color-attachment swapchain");
}

void VulkanContext::CreateDevice(VkSurfaceKHR surface)
{
    if (!_instance || !surface || _surface)
        throw std::logic_error("Vulkan: invalid surface/device initialization");
    _surface = surface;
    SelectDevice();
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queues;
    for (uint32_t family : {_families.graphics, _families.present})
    {
        if (!queues.empty() && family == queues[0].queueFamilyIndex)
            continue;
        VkDeviceQueueCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queues.push_back(info);
    }
    std::vector<const char*> enabled{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    auto extensions =
        EnumerateList<VkExtensionProperties>([&](uint32_t* n, VkExtensionProperties* p)
                                             { return vkEnumerateDeviceExtensionProperties(_physical, nullptr, n, p); },
                                             "enumerate selected device extensions");
    // The name is usable without opting the entire engine into beta Vulkan headers.
    if (HasExtension(extensions, "VK_KHR_portability_subset"))
        enabled.push_back("VK_KHR_portability_subset");
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
    info.pQueueCreateInfos = queues.data();
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    Check(vkCreateDevice(_physical, &info, nullptr, &_device), "create logical device");
    vkGetDeviceQueue(_device, _families.graphics, 0, &_graphics);
    vkGetDeviceQueue(_device, _families.present, 0, &_present);
    if (_debugNamesEnabled)
        _setName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetDeviceProcAddr(_device, "vkSetDebugUtilsObjectNameEXT"));
    Name(VK_OBJECT_TYPE_DEVICE, ObjectHandle(_device), "PoseidonVK device");
    Name(VK_OBJECT_TYPE_QUEUE, ObjectHandle(_graphics), "PoseidonVK graphics queue");
    if (_present != _graphics)
        Name(VK_OBJECT_TYPE_QUEUE, ObjectHandle(_present), "PoseidonVK present queue");
    CreateFrameResources();
    std::fprintf(stderr, "Vulkan: logical device ready: %s, two frame slots\n", _deviceName.c_str());
}

void VulkanContext::Name(VkObjectType type, uint64_t handle, const char* name) const
{
    if (!_setName || !handle)
        return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    _setName(_device, &info); // Diagnostic only; names never determine rendering success.
}

void VulkanContext::CreateFrameResources()
{
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = _families.graphics;
    Check(vkCreateCommandPool(_device, &pool, nullptr, &_pool), "create command pool");
    Name(VK_OBJECT_TYPE_COMMAND_POOL, ObjectHandle(_pool), "PoseidonVK frame command pool");
    for (size_t i = 0; i < _frames.size(); ++i)
    {
        auto& frame = _frames[i];
        VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commands.commandPool = _pool;
        commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(_device, &commands, &frame.command), "allocate frame command buffer");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        Check(vkCreateSemaphore(_device, &semaphore, nullptr, &frame.acquired), "create acquire semaphore");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        Check(vkCreateFence(_device, &fence, nullptr, &frame.submitted), "create frame fence");
        const auto prefix = "PoseidonVK frame " + std::to_string(i);
        Name(VK_OBJECT_TYPE_COMMAND_BUFFER, ObjectHandle(frame.command), (prefix + " commands").c_str());
        Name(VK_OBJECT_TYPE_SEMAPHORE, ObjectHandle(frame.acquired), (prefix + " acquire").c_str());
        Name(VK_OBJECT_TYPE_FENCE, ObjectHandle(frame.submitted), (prefix + " submitted").c_str());
    }
}

void VulkanContext::DestroySwapchain() noexcept
{
    for (auto& pipeline : _screenPipelines)
    {
        if (pipeline)
            vkDestroyPipeline(_device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (_blendPipeline)
        vkDestroyPipeline(_device, _blendPipeline, nullptr);
    _blendPipeline = VK_NULL_HANDLE;
    if (_shapePipeline)
        vkDestroyPipeline(_device, _shapePipeline, nullptr);
    _shapePipeline = VK_NULL_HANDLE;
    if (_trianglePipeline)
        vkDestroyPipeline(_device, _trianglePipeline, nullptr);
    _trianglePipeline = VK_NULL_HANDLE;
    for (VkFramebuffer framebuffer : _framebuffers)
        vkDestroyFramebuffer(_device, framebuffer, nullptr);
    _framebuffers.clear();
    for (auto& depth : _depth)
    {
        if (depth.view)
            vkDestroyImageView(_device, depth.view, nullptr);
        if (depth.image)
            vkDestroyImage(_device, depth.image, nullptr);
        if (depth.memory)
            vkFreeMemory(_device, depth.memory, nullptr);
    }
    _depth.clear();
    if (_renderPass)
        vkDestroyRenderPass(_device, _renderPass, nullptr);
    _renderPass = VK_NULL_HANDLE;
    for (VkImageView view : _views)
        vkDestroyImageView(_device, view, nullptr);
    _views.clear();
    for (VkSemaphore semaphore : _rendered)
        vkDestroySemaphore(_device, semaphore, nullptr);
    _rendered.clear();
    _images.clear();
    if (_swapchain)
        vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    _swapchain = VK_NULL_HANDLE;
    _extent = {};
}

bool VulkanContext::RecreateSwapchain(uint32_t width, uint32_t height)
{
    if (!width || !height)
        return false; // Minimized; keep old resources until a drawable extent exists.
    VkSurfaceCapabilitiesKHR caps{};
    Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physical, _surface, &caps), "query surface capabilities");
    const auto extent = ChooseExtent(caps, width, height);
    if (!extent.width || !extent.height)
        return false;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        throw std::runtime_error("Vulkan: surface no longer supports color attachments");
    auto formats = EnumerateList<VkSurfaceFormatKHR>(
        [&](uint32_t* n, VkSurfaceFormatKHR* p)
        { return vkGetPhysicalDeviceSurfaceFormatsKHR(_physical, _surface, n, p); }, "query surface formats");
    auto modes = EnumerateList<VkPresentModeKHR>(
        [&](uint32_t* n, VkPresentModeKHR* p)
        { return vkGetPhysicalDeviceSurfacePresentModesKHR(_physical, _surface, n, p); }, "query present modes");
    const auto format = ChooseFormat(formats);
    if (modes.empty())
        throw std::runtime_error("Vulkan: surface has no present modes");
    WaitIdle();
    // Simple first-stage recreation: idle, release, rebuild. Partial failure is
    // terminal in EngineVK and Shutdown releases every successfully created object.
    DestroySwapchain();
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = _surface;
    info.minImageCount = ChooseImageCount(caps);
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const uint32_t families[] = {_families.graphics, _families.present};
    info.imageSharingMode =
        _families.graphics == _families.present ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
    if (info.imageSharingMode == VK_SHARING_MODE_CONCURRENT)
    {
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    }
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = ChooseCompositeAlpha(caps.supportedCompositeAlpha);
    info.presentMode = ChoosePresentMode(modes, _swapInterval);
    info.clipped = VK_TRUE;
    Check(vkCreateSwapchainKHR(_device, &info, nullptr, &_swapchain), "create swapchain");
    _extent = extent;
    Name(VK_OBJECT_TYPE_SWAPCHAIN_KHR, ObjectHandle(_swapchain), "PoseidonVK SDL swapchain");
    _images =
        EnumerateList<VkImage>([&](uint32_t* n, VkImage* p)
                               { return vkGetSwapchainImagesKHR(_device, _swapchain, n, p); }, "get swapchain images");

    VkAttachmentDescription attachment{};
    attachment.format = format.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // Every acquired image starts with a deterministic clear.
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    _depthFormat = VK_FORMAT_UNDEFINED;
    for (auto candidate : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM})
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(_physical, candidate, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            _depthFormat = candidate;
            break;
        }
    }
    if (_depthFormat == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("Vulkan Shape: no supported depth attachment format");
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = _depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkAttachmentDescription attachments[] = {attachment, depthAttachment};
    const VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    subpass.pDepthStencilAttachment = &depthReference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = dependency.srcStageMask;
    dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 2;
    pass.pAttachments = attachments;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies = &dependency;
    Check(vkCreateRenderPass(_device, &pass, nullptr, &_renderPass), "create clear render pass");
    Name(VK_OBJECT_TYPE_RENDER_PASS, ObjectHandle(_renderPass), "PoseidonVK clear to present pass");

    _views.reserve(_images.size());
    _framebuffers.reserve(_images.size());
    _rendered.reserve(_images.size());
    _depth.reserve(_images.size());
    for (size_t i = 0; i < _images.size(); ++i)
    {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = _images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView imageView = VK_NULL_HANDLE;
        Check(vkCreateImageView(_device, &view, nullptr, &imageView), "create swapchain image view");
        _views.push_back(imageView);
        _depth.emplace_back();
        CreateDepthAttachment(_depth.back());
        const VkImageView attachmentViews[] = {imageView, _depth.back().view};
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = _renderPass;
        framebuffer.attachmentCount = 2;
        framebuffer.pAttachments = attachmentViews;
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        VkFramebuffer handle = VK_NULL_HANDLE;
        Check(vkCreateFramebuffer(_device, &framebuffer, nullptr, &handle), "create clear framebuffer");
        _framebuffers.push_back(handle);
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore rendered = VK_NULL_HANDLE;
        Check(vkCreateSemaphore(_device, &semaphore, nullptr, &rendered), "create image present semaphore");
        _rendered.push_back(rendered);
        const auto prefix = "PoseidonVK swapchain image " + std::to_string(i);
        Name(VK_OBJECT_TYPE_IMAGE, ObjectHandle(_images[i]), prefix.c_str());
        Name(VK_OBJECT_TYPE_IMAGE_VIEW, ObjectHandle(imageView), (prefix + " view").c_str());
        Name(VK_OBJECT_TYPE_FRAMEBUFFER, ObjectHandle(handle), (prefix + " framebuffer").c_str());
        Name(VK_OBJECT_TYPE_SEMAPHORE, ObjectHandle(rendered), (prefix + " present").c_str());
    }
    _recreate = false;
    std::fprintf(stderr, "Vulkan: swapchain #%u ready: %ux%u, %zu images, format=%d, present mode=%d\n",
                 ++_swapchainGeneration, _extent.width, _extent.height, _images.size(), format.format,
                 info.presentMode);
    return true;
}

void VulkanContext::SetSwapInterval(int interval)
{
    if (interval < -1 || interval > 1)
        throw std::invalid_argument("Vulkan: swap interval must be -1, 0 or 1");
    if (_swapInterval != interval)
    {
        _swapInterval = interval;
        _recreate = true;
    }
}

bool VulkanContext::PrepareSwapchain(uint32_t width, uint32_t height)
{
    if (!_device)
        throw std::logic_error("Vulkan: no logical device");
    if (_frameOpen)
        throw std::logic_error("Vulkan: cannot rebuild an open frame");
    return width && height && (!_recreate || RecreateSwapchain(width, height));
}

bool VulkanContext::BeginFrame(uint32_t width, uint32_t height)
{
    if (_frameOpen)
        return true;
    if (!_device)
        throw std::logic_error("Vulkan: no logical device");
    if (!width || !height)
        return false;
    if (!PrepareSwapchain(width, height))
        return false;
    if (_profile.enabled)
        _profile.frameStart = ProfileClock();
    auto& frame = _frames[_frame];
    Check(vkWaitForFences(_device, 1, &frame.submitted, VK_TRUE, UINT64_MAX), "wait frame fence");
    const double fenceEnd = _profile.enabled ? ProfileClock() : 0;
    if (_profile.enabled)
        _profile.fenceMs += fenceEnd - _profile.frameStart;
    frame.meshes.clear();
    frame.textures.clear();
    // Only this frame's completed fence permits overwriting its mapped pages.
    frame.transientPage = 0;
    for (auto& page : frame.transientPages)
        page.vertexUsed = page.indexUsed = 0;
    frame.uniformPage = 0;
    for (auto& page : frame.uniforms)
        page.used = 0;
    const double retireEnd = _profile.enabled ? ProfileClock() : 0;
    if (_profile.enabled)
        _profile.retireMs += retireEnd - fenceEnd;
    // A finite acquire timeout avoids blocking forever if the surface stops progressing.
    const VkResult acquired =
        vkAcquireNextImageKHR(_device, _swapchain, 1000000000ULL, frame.acquired, VK_NULL_HANDLE, &_image);
    if (_profile.enabled)
        _profile.acquireMs += ProfileClock() - retireEnd;
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
    {
        _recreate = true;
        return false; // Fence remains signaled; no submit has been promised.
    }
    if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY)
        return false;
    if (acquired == VK_SUBOPTIMAL_KHR)
        _recreate = true; // Consume this successful acquire before rebuilding.
    else
        Check(acquired, "acquire frame");
    Check(vkResetCommandBuffer(frame.command, 0), "reset frame command buffer");
    VkCommandBufferBeginInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    commands.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(frame.command, &commands), "begin frame command buffer");
    VkClearValue clears[2]{};
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    _clearColor = {0, 0, 0, 1};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = _renderPass;
    pass.framebuffer = _framebuffers[_image];
    pass.renderArea.extent = _extent;
    pass.clearValueCount = 2;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(frame.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    _frameOpen = true;
    return true;
}

void VulkanContext::Clear(float r, float g, float b, float a)
{
    if (!_frameOpen)
        return;
    _clearColor = {r, g, b, a};
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    attachment.clearValue.color = {{r, g, b, a}};
    VkClearRect rect{};
    rect.rect.extent = _extent;
    rect.layerCount = 1;
    vkCmdClearAttachments(_frames[_frame].command, 1, &attachment, 1, &rect);
}

void VulkanContext::ClearDepth()
{
    if (!_frameOpen)
        return;
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    attachment.clearValue.depthStencil.depth = 1.0f;
    VkClearRect rect{};
    rect.rect.extent = _extent;
    rect.layerCount = 1;
    vkCmdClearAttachments(_frames[_frame].command, 1, &attachment, 1, &rect);
}

void VulkanContext::EndFrame()
{
    if (!_frameOpen)
        return;
    auto& frame = _frames[_frame];
    vkCmdEndRenderPass(frame.command);
    Check(vkEndCommandBuffer(frame.command), "end frame command buffer");
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.acquired;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &_rendered[_image];
    // Reset only immediately before submitting. An out-of-date acquire must
    // never strand a reset fence with no workload to signal it.
    Check(vkResetFences(_device, 1, &frame.submitted), "reset frame fence");
    Check(vkQueueSubmit(_graphics, 1, &submit, frame.submitted), "submit frame");
    ++_submittedFrames;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &_rendered[_image];
    present.swapchainCount = 1;
    present.pSwapchains = &_swapchain;
    present.pImageIndices = &_image;
    const double presentStart = _profile.enabled ? ProfileClock() : 0;
    const VkResult result = vkQueuePresentKHR(_present, &present);
    if (_profile.enabled)
    {
        _profile.presentMs += ProfileClock() - presentStart;
        ReportProfile();
    }
    _frameOpen = false;
    _frame = (_frame + 1) % _frames.size();
    if (NeedsRecreation(result))
        _recreate = true;
    else
        Check(result, "present frame");
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
    {
        if (!_presentedFrames)
            std::fprintf(stderr, "Vulkan: first frame submitted and presented\n");
        ++_presentedFrames;
    }
}

void VulkanContext::ReportProfile()
{
    const double now = ProfileClock();
    _profile.recordMs += now - _profile.frameStart;
    if (_profile.lastEnd)
        _profile.times.push_back(now - _profile.lastEnd);
    _profile.lastEnd = now;
    const double total = std::accumulate(_profile.times.begin(), _profile.times.end(), 0.0);
    if (total < 2000 || _profile.times.empty())
        return;
    auto times = _profile.times;
    std::sort(times.begin(), times.end());
    const double frames = double(times.size());
    std::fprintf(
        stderr,
        "Vulkan profile: frames=%zu fps=%.2f frame_ms=%.3f p95_ms=%.3f record_ms=%.3f "
        "transient/frame=%.1f allocations/frame=%.1f geometry_upload_ms/frame=%.3f "
        "texture_uploads=%llu texture_upload_ms=%.3f fence_ms/frame=%.3f retire_ms/frame=%.3f acquire_ms/frame=%.3f "
        "present_ms/frame=%.3f\n",
        times.size(), frames * 1000 / total, total / frames, times[size_t((times.size() - 1) * 0.95)],
        _profile.recordMs / frames, _profile.transient / frames, _profile.allocations / frames,
        _profile.geometryMs / frames, static_cast<unsigned long long>(_profile.textureUploads), _profile.textureMs,
        _profile.fenceMs / frames, _profile.retireMs / frames, _profile.acquireMs / frames,
        _profile.presentMs / frames);
    const double last = _profile.lastEnd;
    _profile = {};
    _profile.enabled = true;
    _profile.lastEnd = last;
}

void VulkanContext::WaitIdle()
{
    if (_device)
        Check(vkDeviceWaitIdle(_device), "wait device idle");
}

unsigned VulkanContext::Shutdown() noexcept
{
    const bool hadInstance = _instance != VK_NULL_HANDLE;
    if (_device)
    {
        // Also safe after partial initialization or device loss; shutdown must not throw.
        vkDeviceWaitIdle(_device);
        for (auto& entry : _meshes)
            if (auto mesh = entry.lock())
                mesh->Destroy();
        _meshes.clear();
        for (auto& entry : _textures)
            if (auto texture = entry.lock())
                texture->Destroy();
        _textures.clear();
        _whiteTexture.reset();
        DestroySwapchain();
        // Immutable geometry can be shared by both frame slots; teardown follows device idle.
        DestroyBuffer(_device, _triangleIndices);
        DestroyBuffer(_device, _triangleVertices);
        if (_triangleLayout)
            vkDestroyPipelineLayout(_device, _triangleLayout, nullptr);
        _triangleLayout = VK_NULL_HANDLE;
        if (_shapeLayout)
            vkDestroyPipelineLayout(_device, _shapeLayout, nullptr);
        _shapeLayout = VK_NULL_HANDLE;
        if (_lightingLayout)
            vkDestroyDescriptorSetLayout(_device, _lightingLayout, nullptr);
        _lightingLayout = VK_NULL_HANDLE;
        if (_textureLayout)
            vkDestroyDescriptorSetLayout(_device, _textureLayout, nullptr);
        _textureLayout = VK_NULL_HANDLE;
        for (auto& sampler : _textureSamplers)
        {
            if (sampler)
                vkDestroySampler(_device, sampler, nullptr);
            sampler = VK_NULL_HANDLE;
        }
        for (auto& frame : _frames)
        {
            for (auto& page : frame.uniforms)
            {
                if (page.pool)
                    vkDestroyDescriptorPool(_device, page.pool, nullptr);
                DestroyBuffer(_device, page.buffer);
            }
            if (frame.acquired)
                vkDestroySemaphore(_device, frame.acquired, nullptr);
            if (frame.submitted)
                vkDestroyFence(_device, frame.submitted, nullptr);
            frame = {};
        }
        if (_pool)
            vkDestroyCommandPool(_device, _pool, nullptr);
        _pool = VK_NULL_HANDLE;
        vkDestroyDevice(_device, nullptr);
        _device = VK_NULL_HANDLE;
    }
    if (_surface)
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
    _surface = VK_NULL_HANDLE;
    if (_debugMessenger)
    {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy)
            destroy(_instance, _debugMessenger, nullptr);
        _debugMessenger = VK_NULL_HANDLE;
    }
    if (_instance)
        vkDestroyInstance(_instance, nullptr);
    _instance = VK_NULL_HANDLE;
    _physical = VK_NULL_HANDLE;
    _graphics = _present = VK_NULL_HANDLE;
    _setName = nullptr;
    _debugNamesEnabled = false;
    _frameOpen = false;
    _recreate = true;
    _frame = 0;
    if (hadInstance)
        std::fprintf(
            stderr, "Vulkan: shutdown complete; submitted=%llu, presented=%llu, validation errors=%u, warnings=%u\n",
            static_cast<unsigned long long>(_submittedFrames), static_cast<unsigned long long>(_presentedFrames),
            _validationErrors.load(), _validationWarnings.load());
    return ValidationErrors();
}
} // namespace Poseidon::vk
