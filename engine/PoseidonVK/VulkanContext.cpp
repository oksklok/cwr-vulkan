#include <PoseidonVK/VulkanContext.hpp>

#include <cstring>
#include <stdexcept>
#include <type_traits>

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

void VulkanContext::CreateInstance(const char* const* extensions, uint32_t count)
{
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
    app.pEngineName = "PoseidonVK (clear/present foundation)";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.flags = flags;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    // Validation may be enabled externally with VK_INSTANCE_LAYERS.
    Check(vkCreateInstance(&info, nullptr, &_instance), "create instance");
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
    for (VkFramebuffer framebuffer : _framebuffers)
        vkDestroyFramebuffer(_device, framebuffer, nullptr);
    _framebuffers.clear();
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
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies = &dependency;
    Check(vkCreateRenderPass(_device, &pass, nullptr, &_renderPass), "create clear render pass");
    Name(VK_OBJECT_TYPE_RENDER_PASS, ObjectHandle(_renderPass), "PoseidonVK clear to present pass");

    _views.reserve(_images.size());
    _framebuffers.reserve(_images.size());
    _rendered.reserve(_images.size());
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
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = _renderPass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &_views.back();
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
    auto& frame = _frames[_frame];
    Check(vkWaitForFences(_device, 1, &frame.submitted, VK_TRUE, UINT64_MAX), "wait frame fence");
    // A finite acquire timeout avoids blocking forever if the surface stops progressing.
    const VkResult acquired =
        vkAcquireNextImageKHR(_device, _swapchain, 1000000000ULL, frame.acquired, VK_NULL_HANDLE, &_image);
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
    VkClearValue black{};
    black.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = _renderPass;
    pass.framebuffer = _framebuffers[_image];
    pass.renderArea.extent = _extent;
    pass.clearValueCount = 1;
    pass.pClearValues = &black;
    vkCmdBeginRenderPass(frame.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    _frameOpen = true;
    return true;
}

void VulkanContext::Clear(float r, float g, float b, float a)
{
    if (!_frameOpen)
        return;
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    attachment.clearValue.color = {{r, g, b, a}};
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
    Check(vkQueueSubmit(_graphics, 1, &submit, frame.submitted), "submit clear frame");
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &_rendered[_image];
    present.swapchainCount = 1;
    present.pSwapchains = &_swapchain;
    present.pImageIndices = &_image;
    const VkResult result = vkQueuePresentKHR(_present, &present);
    _frameOpen = false;
    _frame = (_frame + 1) % _frames.size();
    if (NeedsRecreation(result))
        _recreate = true;
    else
        Check(result, "present frame");
}

void VulkanContext::WaitIdle()
{
    if (_device)
        Check(vkDeviceWaitIdle(_device), "wait device idle");
}

void VulkanContext::Shutdown() noexcept
{
    if (_device)
    {
        // Also safe after partial initialization or device loss; shutdown must not throw.
        vkDeviceWaitIdle(_device);
        DestroySwapchain();
        for (auto& frame : _frames)
        {
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
}
} // namespace Poseidon::vk
