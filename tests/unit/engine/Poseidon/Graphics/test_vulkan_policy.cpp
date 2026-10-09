// Deliberately standalone: no SDL initialization, instance, surface or physical device.
#include <PoseidonVK/VulkanContext.hpp>
#include <PoseidonVK/TriangleVK.hpp>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace
{
int checks = 0;
std::vector<const char*> availableInstanceExtensions;
std::vector<std::string> requestedInstanceExtensions;
uint32_t requestedApiVersion = 0;
VkInstanceCreateFlags requestedInstanceFlags = 0;
PFN_vkDebugUtilsMessengerCallbackEXT instanceCallback = nullptr;
void* instanceCallbackUser = nullptr;
bool injectTeardownError = false;

template <class Handle>
Handle DebugMessengerHandle()
{
    if constexpr (std::is_pointer_v<Handle>)
        return reinterpret_cast<Handle>(uintptr_t{2});
    else
        return static_cast<Handle>(2);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDebugMessenger(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*,
                                                    const VkAllocationCallbacks*, VkDebugUtilsMessengerEXT* out)
{
    *out = DebugMessengerHandle<VkDebugUtilsMessengerEXT>();
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyDebugMessenger(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks*) {}
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}

template <class Exception, class Action>
void CheckThrows(Action action, const char* message)
{
    ++checks;
    try
    {
        action();
    }
    catch (const Exception&)
    {
        return;
    }
    throw std::runtime_error(message);
}
} // namespace

// Replace only instance entry points: exercise real extension selection without
// creating a driver instance or requiring any particular machine's extensions.
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char*, uint32_t* count,
                                                                      VkExtensionProperties* properties)
{
    if (!properties)
    {
        *count = static_cast<uint32_t>(availableInstanceExtensions.size());
        return VK_SUCCESS;
    }
    const auto copied = std::min(*count, static_cast<uint32_t>(availableInstanceExtensions.size()));
    for (uint32_t i = 0; i < copied; ++i)
    {
        properties[i] = {};
        std::memcpy(properties[i].extensionName, availableInstanceExtensions[i],
                    std::strlen(availableInstanceExtensions[i]) + 1);
    }
    *count = copied;
    return copied == availableInstanceExtensions.size() ? VK_SUCCESS : VK_INCOMPLETE;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks*,
                                                VkInstance* instance)
{
    requestedInstanceExtensions.assign(info->ppEnabledExtensionNames,
                                       info->ppEnabledExtensionNames + info->enabledExtensionCount);
    requestedApiVersion = info->pApplicationInfo->apiVersion;
    requestedInstanceFlags = info->flags;
    instanceCallback = nullptr;
    instanceCallbackUser = nullptr;
    for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT)
        {
            const auto* debug = reinterpret_cast<const VkDebugUtilsMessengerCreateInfoEXT*>(next);
            instanceCallback = debug->pfnUserCallback;
            instanceCallbackUser = debug->pUserData;
        }
    *instance = reinterpret_cast<VkInstance>(uintptr_t{1});
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char* name)
{
    if (std::strcmp(name, "vkCreateDebugUtilsMessengerEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateDebugMessenger);
    if (std::strcmp(name, "vkDestroyDebugUtilsMessengerEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&DestroyDebugMessenger);
    return nullptr;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks*)
{
    // The instance pNext callback remains valid during vkDestroyInstance, after
    // the persistent debug messenger has already been destroyed.
    if (injectTeardownError && instanceCallback)
    {
        VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        data.pMessageIdName = "TEST-teardown-only";
        data.pMessage = "Intentional driver-free destruction error";
        instanceCallback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
                         &data, instanceCallbackUser);
    }
}
int TestVulkanBuffers();

int main()
{
    using namespace Poseidon::vk;
    try
    {
        checks += TestVulkanBuffers();
        Check(sizeof(TriangleVertex) == 20 && sizeof(TriangleIndices[0]) == 2,
              "diagnostic vertex/index formats must match the pipeline");
        Check(TriangleIndices == std::array<uint16_t, 3>{2, 0, 1}, "diagnostic must exercise non-sequential indexing");
        for (auto index : TriangleIndices)
            Check(index < TriangleVertices.size(), "every index must address a real vertex");
        VkSurfaceCapabilitiesKHR caps{};
        caps.currentExtent = {1920, 1080};
        auto extent = ChooseExtent(caps, 640, 480);
        Check(extent.width == 1920 && extent.height == 1080, "fixed surface extent must win over requested size");
        caps.currentExtent = {UINT32_MAX, UINT32_MAX};
        caps.minImageExtent = {64, 64};
        caps.maxImageExtent = {4096, 2160};
        extent = ChooseExtent(caps, 1, 5000);
        Check(extent.width == 64 && extent.height == 2160, "variable extent must clamp both dimensions");
        extent = ChooseExtent(caps, 800, 600);
        Check(extent.width == 800 && extent.height == 600, "valid variable extent must survive");
        caps.currentExtent = {0, 0};
        extent = ChooseExtent(caps, 800, 600);
        Check(!extent.width && !extent.height, "zero surface extent must suspend creation");
        caps.minImageCount = 2;
        caps.maxImageCount = 0;
        Check(ChooseImageCount(caps) == 3, "zero maximum means unbounded image count");
        caps.maxImageCount = 2;
        Check(ChooseImageCount(caps) == 2, "bounded image count must honor surface maximum");
        caps.minImageCount = UINT32_MAX;
        caps.maxImageCount = 0;
        Check(ChooseImageCount(caps) == UINT32_MAX, "image count must not overflow");

        const VkSurfaceFormatKHR fallback{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        const VkSurfaceFormatKHR preferred{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        Check(ChooseFormat({fallback, preferred}).format == preferred.format, "prefer BGRA UNORM surface");
        Check(ChooseFormat({fallback}).format == fallback.format, "respect a supported non-preferred format");
        Check(ChooseFormat({{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}}).format == preferred.format,
              "undefined singleton permits choosing a concrete format");
        CheckThrows<std::runtime_error>([] { ChooseFormat({}); }, "empty format list must fail");
        Check(ChoosePresentMode({VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR}, 1) == VK_PRESENT_MODE_FIFO_KHR,
              "vsync default must be FIFO");
        Check(ChoosePresentMode({VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR}, 0) ==
                  VK_PRESENT_MODE_MAILBOX_KHR,
              "off prefers mailbox when available");
        Check(ChoosePresentMode({VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}, 0) ==
                  VK_PRESENT_MODE_IMMEDIATE_KHR,
              "off can use immediate mode");
        Check(ChoosePresentMode({VK_PRESENT_MODE_FIFO_KHR}, 0) == VK_PRESENT_MODE_FIFO_KHR,
              "off must fall back on FIFO-only surfaces");
        Check(ChoosePresentMode({VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR}, -1) ==
                  VK_PRESENT_MODE_FIFO_RELAXED_KHR,
              "adaptive prefers relaxed FIFO");
        Check(ChoosePresentMode({VK_PRESENT_MODE_FIFO_KHR}, -1) == VK_PRESENT_MODE_FIFO_KHR,
              "unsupported adaptive must fall back to FIFO");
        Check(ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ==
                  VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
              "prefer opaque composition");
        Check(ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) == VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
              "composition must respect the surface mask");
        CheckThrows<std::runtime_error>([] { ChooseCompositeAlpha(0); }, "empty alpha mask must fail");

        std::vector<VkQueueFamilyProperties> families(3);
        families[0].queueCount = families[1].queueCount = families[2].queueCount = 1;
        families[0].queueFlags = families[2].queueFlags = VK_QUEUE_GRAPHICS_BIT;
        auto queues = ChooseQueues(families, {VK_FALSE, VK_TRUE, VK_TRUE});
        Check(queues.Complete() && queues.graphics == 2 && queues.present == 2,
              "combined family must beat earlier split families");
        queues = ChooseQueues(families, {VK_FALSE, VK_TRUE, VK_FALSE});
        Check(queues.Complete() && queues.graphics == 0 && queues.present == 1,
              "separate graphics/present queues must be supported");
        families[2].queueCount = 0;
        queues = ChooseQueues(families, {VK_FALSE, VK_FALSE, VK_TRUE});
        Check(!queues.Complete(), "zero-queue family cannot present");
        Check(!ChooseQueues({}, {}).Complete(), "empty queue inventory must fail");
        Check(!ChooseQueues(families, {}).Complete(), "missing surface-support inventory must fail");
        Check(NeedsRecreation(VK_ERROR_OUT_OF_DATE_KHR), "out-of-date requires recreation");
        Check(NeedsRecreation(VK_SUBOPTIMAL_KHR), "suboptimal requires recreation");
        Check(!NeedsRecreation(VK_ERROR_DEVICE_LOST), "device loss is terminal, not a resize");
        Check(!NeedsRecreation(VK_SUCCESS), "successful present must not force recreation");

        const char* surfaceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME};
        VulkanContext instanceContext;
        availableInstanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME};
        instanceContext.CreateInstance(surfaceExtensions, 1);
        Check(requestedInstanceExtensions == std::vector<std::string>{VK_KHR_SURFACE_EXTENSION_NAME},
              "native Vulkan 1.0 must work without optional properties2 support");
        Check(requestedApiVersion == VK_API_VERSION_1_0 && requestedInstanceFlags == 0,
              "native instance must retain the Vulkan 1.0 baseline and flags");
        instanceContext.Shutdown();

#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        availableInstanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
                                       VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
        instanceContext.CreateInstance(surfaceExtensions, 1);
        Check(std::count(requestedInstanceExtensions.begin(), requestedInstanceExtensions.end(),
                         VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 1,
              "Vulkan 1.0 portability requires properties2 enabled at instance creation");
        Check(requestedApiVersion == VK_API_VERSION_1_0 &&
                  (requestedInstanceFlags & VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR),
              "portability enumeration must retain its flag without raising the API baseline");
        instanceContext.Shutdown();
#endif

        availableInstanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                       VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
        const char* suppliedExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
        instanceContext.CreateInstance(suppliedExtensions, 2);
        Check(std::count(requestedInstanceExtensions.begin(), requestedInstanceExtensions.end(),
                         VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 1,
              "properties2 supplied by the caller must not be added twice");
        instanceContext.Shutdown();

        availableInstanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
        VulkanContext teardownContext;
        teardownContext.CreateInstance(surfaceExtensions, 1);
        Check(teardownContext.ValidationErrors() == 0 && instanceCallback,
              "teardown regression must start with no validation errors and a registered callback");
        injectTeardownError = true;
        Check(teardownContext.Shutdown() == 1,
              "shutdown must return errors raised only during resource destruction, not the pre-teardown count");
        injectTeardownError = false;
        Check(teardownContext.Shutdown() == 1 && !teardownContext.Instance(),
              "repeated shutdown must retain the final error count without recreating resources");

        VulkanContext context;
        CheckThrows<std::logic_error>([&] { context.DrawDiagnosticTriangle(); },
                                      "indexed draw must require an acquired recording frame");
        Check(!context.Instance() && !context.FrameOpen(), "context must start empty");
        Check(context.SwapInterval() == 1, "vsync starts enabled");
        context.SetSwapInterval(-1);
        Check(context.SwapInterval() == -1, "adaptive interval can be requested before device setup");
        CheckThrows<std::invalid_argument>([&] { context.SetSwapInterval(2); }, "invalid swap interval must fail");
        CheckThrows<std::runtime_error>([&] { context.CreateInstance(nullptr, 0); },
                                        "missing SDL extensions must fail before driver access");
        CheckThrows<std::logic_error>([&] { context.CreateDevice(VK_NULL_HANDLE); },
                                      "missing instance/surface must fail");
        CheckThrows<std::logic_error>([&] { context.BeginFrame(800, 600); }, "uninitialized frame must fail");
        context.Clear(0, 0, 0, 1); // No frame: no Vulkan call.
        context.EndFrame();
        context.WaitIdle();
        context.Shutdown();
        context.Shutdown();
        Check(!context.Instance() && !context.FrameOpen(), "empty shutdown must be idempotent");
        std::printf("Vulkan driver-free policy/lifetime guards: %d checks passed\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Vulkan test failed: %s\n", error.what());
        return 1;
    }
}
