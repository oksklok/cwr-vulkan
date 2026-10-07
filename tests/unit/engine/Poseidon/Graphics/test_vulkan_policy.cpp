// Deliberately standalone: no SDL initialization, instance, surface or physical device.
#include <PoseidonVK/VulkanContext.hpp>
#include <cstdio>
#include <stdexcept>

namespace
{
int checks = 0;
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

int main()
{
    using namespace Poseidon::vk;
    try
    {
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

        VulkanContext context;
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
