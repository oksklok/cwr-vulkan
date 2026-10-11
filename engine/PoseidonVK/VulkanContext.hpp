#pragma once

#include <PoseidonVK/SwapchainPolicy.hpp>
#include <PoseidonVK/BufferVK.hpp>
#include <PoseidonVK/ShapeLightingData.hpp>
#include <PoseidonVK/ScreenBatchVK.hpp>
#include <PoseidonVK/AntiAliasingVK.hpp>
#include <array>
#include <atomic>
#include <string>
#include <memory>
#include <span>
#include <chrono>

namespace Poseidon::vk
{
struct TextureMip
{
    uint32_t width, height;
    const void* rgba;
};
inline VkPipelineRasterizationStateCreateInfo ShapeRasterization(bool shadow)
{
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // GL33 keeps back-face culling in BOTH native and software/screen passes.
    // Without it coplanar reverse faces overwrite the front's lighting (fences).
    // Preserve the existing projected-shadow rasterization.
    raster.cullMode = shadow ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1;
    return raster;
}
inline uint32_t TextureSampledMipCount(std::span<const TextureMip> mips)
{
    // GL33's TextureSourcePac stops BEFORE either dimension reaches 4 pixels.
    // Keep all decoded/uploaded bytes, but expose the same sampled tail range.
    // Always retain a valid base for tiny/single-level dynamic UI textures.
    for (size_t level = 1; level < mips.size(); ++level)
        if (mips[level].width <= 4 || mips[level].height <= 4)
            return uint32_t(level);
    return uint32_t(mips.size());
}
// The eight existing shared samplers mirror GL33: anisotropic trilinear for
// linear modes, exact nearest sampling for point modes, no LOD bias.
inline float TextureAnisotropy(bool supported, float limit)
{
    return supported ? std::min(16.f, limit) : 1.f;
}
inline VkSamplerCreateInfo TextureSamplerInfo(unsigned index, float anisotropy)
{
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const bool point = (index & 4) != 0;
    sampler.magFilter = sampler.minFilter = point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sampler.mipmapMode = point ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.anisotropyEnable = !point && anisotropy > 1.f;
    sampler.maxAnisotropy = sampler.anisotropyEnable ? anisotropy : 1.f;
    sampler.maxLod = VK_LOD_CLAMP_NONE; // Image view bounds the stored mip chain.
    sampler.addressModeU = (index & 1) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = (index & 2) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    return sampler;
}
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
struct MeshSlice
{
    std::shared_ptr<MeshBuffers> buffers;
    VkDeviceSize vertexOffset = 0, indexOffset = 0;
};
// Backend-private Vulkan ownership. SDL owns the window; this owns its surface.
struct ShadowAlphaBatch
{
    std::shared_ptr<TextureImage> texture;
    uint32_t first = 0, count = 0;
};
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
    void BeginShadowPass();
    void EndShadowPass();
    void RenderShadowDepth(const float* matrices, int count, int resolution,
                           std::span<const float> solid, std::span<const float> alpha,
                           std::span<const ShadowAlphaBatch> batches);
    void SetShadowState(const ShadowLighting& state) { _csmState = state; }
    void ResetShadowState() { _csmActive = false; }
    void DrawDiagnosticTriangle(); // Explicit DrawTestPattern seam, never an automatic gameplay draw.
    std::shared_ptr<MeshBuffers> UploadMesh(const void* vertices, size_t vertexBytes, const void* indices,
                                            size_t indexBytes);
    MeshSlice UploadTransientMesh(const void* vertices, size_t vertexBytes, const void* indices, size_t indexBytes);
    void QueueScreenPolygon(std::span<const ScreenVertex> polygon, const ScreenState& state);
    void FlushScreenBatch();
    std::shared_ptr<TextureImage> UploadTexture(uint32_t width, uint32_t height, const void* rgba);
    std::shared_ptr<TextureImage> UploadTexture(std::span<const TextureMip> mips);
    void DrawMesh(const std::shared_ptr<MeshBuffers>& mesh, uint32_t firstIndex, uint32_t count, bool index16,
                  const std::array<float, 16>& mvp, const std::array<float, 4>& color,
                  const std::shared_ptr<TextureImage>& texture = {}, unsigned sampler = 0, float alphaCutoff = 0,
                  bool blend = false, bool screen = false, bool depthTest = true, const VkRect2D* clip = nullptr,
                  const std::shared_ptr<TextureImage>& detail = {}, float secondaryMode = 1,
                  const std::array<float, 3>& lightDirection = {0, -1, 0}, VkDeviceSize vertexOffset = 0,
                  VkDeviceSize indexOffset = 0, bool depthWrite = true, const ShapeLighting* lighting = nullptr,
                  bool shadow = false, bool additive = false);
    void EndFrame();
    bool SetSSAO(bool enabled, float strength, float radius, float bias, float fade);
    bool SSAOEnabled() const { return _ssaoEnabled; }
    void DrawSSAO(const std::array<float, 4>& projection);
    std::string SetAntiAliasing(std::string_view mode);
    bool SetRenderScale(int percent);
    void BeginAAWorld();
    void FinishAAWorld(const std::array<float, 4>& projection);
    bool TemporalEnabled() const { return _aaMode == AAMode::TAA && _worldActive; }
    bool TemporalHistoryValid() const { return _taaValid; }
    void ResetTemporalHistory() { _taaValid = false; }
    std::array<float, 2> TemporalJitter() const { return _taaJitter; }
    bool FrameOpen() const { return _frameOpen; }
    VkExtent2D Extent() const { return _extent; }
    const std::string& DeviceName() const { return _deviceName; }
    unsigned ValidationErrors() const { return _validationErrors.load(); }
    // Like GL33, the last setting applies to the complete finished framebuffer.
    void SetGamma(float gamma) { _gamma = gamma; }
    float Gamma() const { return _gamma; }
    void SetFogColor(const std::array<float, 4>& color) { if (_fogColor != color) FlushScreenBatch(); _fogColor = color; }
    void SetNightEye(float night)
    {
        const auto eye = night > 0.01f ? std::array<float, 4>{0.2f, 0.9f, 0.4f, 1 - night} : std::array<float, 4>{0, 0, 0, 1};
        if (_eyeCoef != eye) FlushScreenBatch();
        _eyeCoef = eye;
    }
    const std::array<float, 4>& EyeCoef() const { return _eyeCoef; }

  private:
    friend struct VulkanCommandStateTest;
    // Only the currently recording command buffer. All production draws use
    // _shapeLayout; diagnostic drawing invalidates this before its other layout.
    // Handles are non-owning: frame retention below still owns every resource.
    struct CommandState
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, 2> textures{};
        VkDescriptorSet lighting = VK_NULL_HANDLE;
        VkDescriptorSet cascades = VK_NULL_HANDLE;
        uint32_t lightingOffset = 0;
        VkBuffer vertex = VK_NULL_HANDLE, index = VK_NULL_HANDLE;
        VkDeviceSize vertexOffset = 0, indexOffset = 0;
        VkIndexType indexType = VK_INDEX_TYPE_UINT16;
        VkViewport viewport{};
        VkRect2D scissor{};
        std::array<float, 28> constants{};
        bool viewportValid = false, scissorValid = false, constantsValid = false;
    } _commands;
    void BindLightingSet(VkDescriptorSet set, uint32_t offset);
    ScreenBatch _screenBatch;
    bool _batchScreens = true; // Process-local A/B profiling control; normal operation batches.
    // Opt-in bounded measurement, not a scheduler or frame-time governor.
    struct Profile
    {
        bool enabled = false;
        double lastEnd = 0, frameStart = 0;
        double recordMs = 0, geometryMs = 0, textureMs = 0, fenceMs = 0, retireMs = 0, acquireMs = 0, presentMs = 0;
        double commandStart = 0, commandMs = 0, submitMs = 0, retentionMs = 0, bindingMs = 0;
        double ssaoMs = 0, ssaoGpuMs = 0;
        double gpuMs = 0;
        uint64_t gpuSamples = 0;
        double taaGpuMs = 0;
        uint64_t taaGpuSamples = 0;
        uint64_t ssaoPasses = 0, ssaoGpuSamples = 0;
        double csmGpuMs = 0;
        uint64_t csmPasses = 0, csmGpuSamples = 0, csmVertices = 0;
        // Pipeline, textures, lighting, vertex, index, viewport, scissor, push constants.
        std::array<uint64_t, 8> stateCommands{};
        uint64_t transient = 0, allocations = 0, textureUploads = 0, litDraws = 0, localLights = 0;
        uint64_t nativeShadows = 0, softwareShadows = 0, shadowTriangles = 0;
        uint64_t screenPolygons = 0, screenBatches = 0, screenDraws = 0;
        std::array<float, 2> fogRange{};
        std::vector<double> times;
    } _profile;
    static double ProfileClock()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void ReportProfile();
    struct Frame
    {
        struct ShadowTarget
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            std::array<VkImageView, 4> layers{};
            std::array<VkFramebuffer, 4> framebuffers{};
            VkDescriptorPool pool = VK_NULL_HANDLE;
            VkDescriptorSet set = VK_NULL_HANDLE;
            int resolution = 0, count = 0;
        } shadow;
        struct TransientPage
        {
            std::shared_ptr<MeshBuffers> buffers;
            VkDeviceSize vertexUsed = 0, indexUsed = 0;
        };
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence submitted = VK_NULL_HANDLE;
        VkQueryPool ssaoQueries = VK_NULL_HANDLE;
        VkQueryPool frameQueries = VK_NULL_HANDLE;
        bool frameTimestamped = false;
        bool taaTimestamped = false;
        bool ssaoTimestamped = false;
        VkQueryPool csmQueries = VK_NULL_HANDLE;
        bool csmTimestamped = false;
        std::vector<std::shared_ptr<MeshBuffers>> meshes;
        std::vector<std::shared_ptr<TextureImage>> textures;
        std::vector<TransientPage> transientPages;
        size_t transientPage = 0;
        struct UniformPage
        {
            BufferVK buffer;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            VkDescriptorSet set = VK_NULL_HANDLE;
            uint32_t used = 0;
        };
        std::vector<UniformPage> uniforms;
        size_t uniformPage = 0;
        struct UniformCache
        {
            ShapeLighting value{};
            VkDescriptorSet set = VK_NULL_HANDLE;
            uint32_t offset = 0;
        } nativeUniform, screenUniform;
    };
    static constexpr size_t FramesInFlight = 2;
    VkInstance _instance = VK_NULL_HANDLE;
    VkSurfaceKHR _surface = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    VkDevice _device = VK_NULL_HANDLE;
    QueueFamilies _families;
    uint32_t _timestampBits = 0;
    float _timestampPeriod = 0;
    VkQueue _graphics = VK_NULL_HANDLE;
    VkQueue _present = VK_NULL_HANDLE;
    VkCommandPool _pool = VK_NULL_HANDLE;
    std::array<Frame, FramesInFlight> _frames{};
    std::vector<std::weak_ptr<MeshBuffers>> _meshes;
    std::vector<std::weak_ptr<TextureImage>> _textures;
    std::shared_ptr<TextureImage> _whiteTexture;
    VkDescriptorSetLayout _textureLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _lightingLayout = VK_NULL_HANDLE;
    VkRenderPass _csmPass = VK_NULL_HANDLE;
    VkPipelineLayout _csmLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, 2> _csmPipelines{};
    VkSampler _csmSampler = VK_NULL_HANDLE;
    VkImageView _csmFallbackView = VK_NULL_HANDLE;
    VkDescriptorPool _csmFallbackPool = VK_NULL_HANDLE;
    VkDescriptorSet _csmFallbackSet = VK_NULL_HANDLE;
    ShadowLighting _csmState{};
    bool _csmActive = false;
    void CreateShadowResources();
    void CreateShadowTarget(Frame::ShadowTarget& target, int resolution, int count);
    void DestroyShadowTarget(Frame::ShadowTarget& target) noexcept;
    void DestroyShadowResources() noexcept;
    void CreateShadowFallback();
    uint32_t _uniformAlignment = 16;
    void BindLighting(const ShapeLighting& lighting, bool screen);
    std::array<float, 4> _fogColor{};
    std::array<float, 4> _eyeCoef{0, 0, 0, 1};
    std::array<VkSampler, 8> _textureSamplers{};
    float _textureAnisotropy = 1.f;
    void CreateTextureLayout();
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    VkRenderPass _renderPass = VK_NULL_HANDLE;
    VkRenderPass _resumePass = VK_NULL_HANDLE;
    struct DepthAttachment
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct AATarget
    {
        DepthAttachment color, depth, multisample, edges, weights, filtered, motion;
        VkDescriptorSet motionSet = VK_NULL_HANDLE;
        VkImageView sampledDepth = VK_NULL_HANDLE;
        VkDescriptorSet colorSet = VK_NULL_HANDLE, depthSet = VK_NULL_HANDLE;
        VkDescriptorSet edgesSet = VK_NULL_HANDLE, weightsSet = VK_NULL_HANDLE, filteredSet = VK_NULL_HANDLE;
        VkFramebuffer scene = VK_NULL_HANDLE, ao = VK_NULL_HANDLE;
        VkFramebuffer edgeFB = VK_NULL_HANDLE, weightFB = VK_NULL_HANDLE, filterFB = VK_NULL_HANDLE;
    };
    AAMode _aaMode = AAMode::Off, _requestedAA = AAMode::Off;
    int _renderScale = 100, _requestedScale = 100;
    VkSampleCountFlags _aaSampleSupport = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlagBits _worldSamples = VK_SAMPLE_COUNT_1_BIT;
    VkExtent2D _worldExtent{};
    bool _worldActive = false, _worldPassOpen = false;
    std::vector<AATarget> _aaTargets;
    VkRenderPass _worldPass = VK_NULL_HANDLE, _worldResume = VK_NULL_HANDLE, _aaPass = VK_NULL_HANDLE, _aaDataPass = VK_NULL_HANDLE;
    VkDescriptorPool _aaPool = VK_NULL_HANDLE;
    VkDescriptorSet _smaaAreaSet = VK_NULL_HANDLE, _smaaSearchSet = VK_NULL_HANDLE;
    VkPipelineLayout _aaLayout = VK_NULL_HANDLE;
    VkSampler _aaSampler = VK_NULL_HANDLE;
    // FXAA, SMAA edge/weights/neighborhood, world composite.
    std::array<VkPipeline, 6> _aaPipelines{};
    struct TemporalTarget
    {
        DepthAttachment color;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };
    std::array<TemporalTarget, 2> _taaHistory{};
    VkRenderPass _taaPass = VK_NULL_HANDLE;
    bool _taaValid = false, _motionDebug = false, _taaInitialized = false;
    unsigned _taaIndex = 0, _taaFrame = 0;
    std::array<float, 2> _taaJitter{};
    std::array<float, 4> _taaProjection{};
    std::shared_ptr<TextureImage> _smaaArea, _smaaSearch;
    std::array<VkPipeline, 8> _worldShapePipelines{};
    std::array<VkPipeline, 16> _worldScreenPipelines{};
    std::array<VkPipeline, 2> _worldShadowPipelines{};
    VkPipeline _ssaoMSPipeline = VK_NULL_HANDLE;
    VkExtent2D RenderExtent() const { return _worldActive ? _worldExtent : _extent; }
    VkFramebuffer SceneFramebuffer() const { return _worldActive ? _aaTargets[_image].scene : _framebuffers[_image]; }
    VkRenderPass ResumePass() const { return _worldActive ? _worldResume : _resumePass; }
    void CreateAAResources(VkFormat format);
    void DestroyAAResources() noexcept;
    void CreateAAImage(DepthAttachment& image, VkFormat format, VkImageUsageFlags usage,
                       VkSampleCountFlagBits samples, VkImageAspectFlags aspect);
    void DrawAAPass(unsigned index, VkFramebuffer framebuffer, VkDescriptorSet source, VkExtent2D extent);
    std::vector<DepthAttachment> _depth;
    std::vector<VkImageView> _sampledDepthViews; // Depth only; attachment views retain stencil for shadows.
    // Same format/extent as the swapchain: keep the existing blend/color-space
    // behavior. Each acquired image owns its scene color and final-pass set.
    struct GammaTarget
    {
        DepthAttachment color;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSet depthSet = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkFramebuffer ssaoFramebuffer = VK_NULL_HANDLE;
    };
    std::vector<GammaTarget> _gammaTargets;
    VkRenderPass _gammaPass = VK_NULL_HANDLE;
    VkDescriptorPool _gammaPool = VK_NULL_HANDLE;
    VkPipelineLayout _gammaLayout = VK_NULL_HANDLE;
    VkPipeline _gammaPipeline = VK_NULL_HANDLE;
    void CreateSceneColor(DepthAttachment& color, VkFormat format);
    void CreateGammaPass(VkFormat format);
    void DrawGammaPass();
    void DestroyGammaResources() noexcept;
    bool _ssaoEnabled = false;
    std::array<float, 4> _ssaoSettings{1.2f, 1.5f, 0.03f, 80.f}; // strength, radius/bias metres, fade metres
    VkRenderPass _ssaoPass = VK_NULL_HANDLE;
    VkPipelineLayout _ssaoLayout = VK_NULL_HANDLE;
    VkPipeline _ssaoPipeline = VK_NULL_HANDLE;
    void CreateSSAOResources(VkFormat format);
    void DestroySSAOResources() noexcept;
    VkFormat _depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout _shapeLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, 8> _shapePipelines{};
    std::array<VkPipeline, 16> _screenPipelines{};
    std::array<VkPipeline, 2> _shadowPipelines{}; // Native and software projected geometry.
    bool _shadowPass = false;
    bool _loggedShape = false;
    void CreateShapePipeline(bool blend = false, bool screen = false, bool depthTest = true, bool depthWrite = true,
                             bool shadow = false, bool additive = false);
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
