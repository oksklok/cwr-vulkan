#pragma once

#include <PoseidonVK/SwapchainPolicy.hpp>
#include <PoseidonVK/BufferVK.hpp>
#include <array>
#include <atomic>
#include <string>
#include <memory>
#include <span>

namespace Poseidon::vk
{
struct TextureMip
{
    uint32_t width, height;
    const void* rgba;
};
// Shape owners and recorded frames share immutable allocations. Shutdown releases
// even allocations whose engine-side Shape outlives the Vulkan device.
struct MeshBuffers
{
    MeshBuffers() = default;
    MeshBuffers(const MeshBuffers&) = delete;
    MeshBuffers& operator=(const MeshBuffers&) = delete;
    VkDevice device = VK_NULL_HANDLE;
    BufferVK vertices, indices;
    ~MeshBuffers();
    void Destroy() noexcept;
};
struct TextureImage
{
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 8> descriptors{};
    ~TextureImage();
    void Destroy() noexcept;
};
// Backend-private Vulkan ownership. SDL owns the window; this owns its surface.
// No engine drawing, asset or GL types are involved in device/swapchain lifetime.
class VulkanContext
{
  public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    void CreateInstance(const char* const* extensions, uint32_t count, bool validation = false);
    VkInstance Instance() const { return _instance; }
    void CreateDevice(VkSurfaceKHR surface); // Takes ownership even if device setup fails.
    unsigned Shutdown() noexcept;            // Final validation count, including destruction callbacks.
    void WaitIdle();
    void RequestRecreation() { _recreate = true; }
    bool PrepareSwapchain(uint32_t width, uint32_t height);
    void SetSwapInterval(int interval);
    int SwapInterval() const { return _swapInterval; }
    bool BeginFrame(uint32_t width, uint32_t height);
    void Clear(float r, float g, float b, float a);
    void ClearDepth();
    void DrawDiagnosticTriangle(); // Explicit DrawTestPattern seam, never an automatic gameplay draw.
    std::shared_ptr<MeshBuffers> UploadMesh(const void* vertices, size_t vertexBytes, const void* indices,
                                            size_t indexBytes);
    std::shared_ptr<TextureImage> UploadTexture(uint32_t width, uint32_t height, const void* rgba);
    std::shared_ptr<TextureImage> UploadTexture(std::span<const TextureMip> mips);
    void DrawMesh(const std::shared_ptr<MeshBuffers>& mesh, uint32_t firstIndex, uint32_t count, bool index16,
                  const std::array<float, 16>& mvp, const std::array<float, 4>& color,
                  const std::shared_ptr<TextureImage>& texture = {}, unsigned sampler = 0, float alphaCutoff = 0,
                  bool blend = false, bool screen = false, bool depthTest = true, const VkRect2D* clip = nullptr,
                  const std::shared_ptr<TextureImage>& detail = {}, float secondaryMode = 1,
                  const std::array<float, 3>& lightDirection = {0, -1, 0});
    void EndFrame();
    bool FrameOpen() const { return _frameOpen; }
    VkExtent2D Extent() const { return _extent; }
    const std::string& DeviceName() const { return _deviceName; }
    unsigned ValidationErrors() const { return _validationErrors.load(); }
    void SetGamma(float gamma) { _gamma = gamma; }
    float Gamma() const { return _gamma; }

  private:
    struct Frame
    {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence submitted = VK_NULL_HANDLE;
        std::vector<std::shared_ptr<MeshBuffers>> meshes;
        std::vector<std::shared_ptr<TextureImage>> textures;
    };
    static constexpr size_t FramesInFlight = 2;
    VkInstance _instance = VK_NULL_HANDLE;
    VkSurfaceKHR _surface = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    VkDevice _device = VK_NULL_HANDLE;
    QueueFamilies _families;
    VkQueue _graphics = VK_NULL_HANDLE;
    VkQueue _present = VK_NULL_HANDLE;
    VkCommandPool _pool = VK_NULL_HANDLE;
    std::array<Frame, FramesInFlight> _frames{};
    std::vector<std::weak_ptr<MeshBuffers>> _meshes;
    std::vector<std::weak_ptr<TextureImage>> _textures;
    std::shared_ptr<TextureImage> _whiteTexture;
    VkDescriptorSetLayout _textureLayout = VK_NULL_HANDLE;
    std::array<VkSampler, 8> _textureSamplers{};
    void CreateTextureLayout();
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    VkRenderPass _renderPass = VK_NULL_HANDLE;
    struct DepthAttachment
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    std::vector<DepthAttachment> _depth;
    VkFormat _depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout _shapeLayout = VK_NULL_HANDLE;
    VkPipeline _shapePipeline = VK_NULL_HANDLE;
    VkPipeline _blendPipeline = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> _screenPipelines{};
    bool _loggedShape = false;
    void CreateShapePipeline(bool blend = false, bool screen = false, bool depthTest = true);
    void CreateDepthAttachment(DepthAttachment& depth);
    std::vector<VkImage> _images;
    std::vector<VkImageView> _views;
    std::vector<VkFramebuffer> _framebuffers;
    // Present waits have image lifetime, not frame-fence lifetime.
    std::vector<VkSemaphore> _rendered;
    VkExtent2D _extent{};
    size_t _frame = 0;
    uint32_t _image = 0;
    bool _frameOpen = false;
    bool _recreate = true;
    int _swapInterval = 1;
    std::string _deviceName;
    PFN_vkSetDebugUtilsObjectNameEXT _setName = nullptr;
    bool _debugNamesEnabled = false;
    VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;
    bool _validationEnabled = false;
    std::atomic<unsigned> _validationErrors{0}, _validationWarnings{0};
    uint64_t _submittedFrames = 0, _presentedFrames = 0;
    unsigned _swapchainGeneration = 0;
    BufferVK _triangleVertices, _triangleIndices;
    VkPipelineLayout _triangleLayout = VK_NULL_HANDLE;
    VkPipeline _trianglePipeline = VK_NULL_HANDLE;
    std::array<float, 4> _clearColor{0, 0, 0, 1};
    bool _loggedTriangle = false;
    float _gamma = 1;
    void CreateTrianglePipeline();

    static VKAPI_ATTR VkBool32 VKAPI_CALL ValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                            VkDebugUtilsMessageTypeFlagsEXT types,
                                                            const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                            void* user);

    void SelectDevice();
    void CreateFrameResources();
    bool RecreateSwapchain(uint32_t width, uint32_t height);
    void DestroySwapchain() noexcept;
    void Name(VkObjectType type, uint64_t handle, const char* name) const;
};
} // namespace Poseidon::vk
