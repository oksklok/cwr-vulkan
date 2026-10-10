#pragma once

#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/GraphicsEngineFactory.hpp>
#include <Poseidon/Graphics/Shared/SDLEventWindow.hpp>
#include <PoseidonVK/VulkanContext.hpp>
#include <PoseidonVK/TextureVK.hpp>
#include <PoseidonVK/ScreenGeometryVK.hpp>
#include <Poseidon/Graphics/Core/ZBiasMath.hpp>

namespace Poseidon
{
// Opt-in experimental diffuse Shape, software TL and textured 2D rendering.
// Inherits Engine directly: Dummy's successful no-op draw paths are not used.
class EngineVK final : public Engine
{
  public:
    explicit EngineVK(const GraphicsEngineParams& params);
    ~EngineVK() override;
    unsigned Shutdown() noexcept; // Terminal shutdown; returns the final smoke-test validation count.
    EngineVK(const EngineVK&) = delete;
    EngineVK& operator=(const EngineVK&) = delete;

    RString GetDebugName() const override;
    RString GetRendererName() const override;
    unsigned int GetDebugErrorCount() const override { return _vk.ValidationErrors(); }
    bool HasFailed() const noexcept { return _failed; }
    void InitDraw(bool clear = false, PackedColor color = PackedColor(0)) override;
    void FinishDraw() override;
    bool InitDrawDone() override { return _vk.FrameOpen(); }
    bool IsAbleToDraw() override { return !_failed && !_paused && IsOpen(); }
    void Clear(bool clearZ = true, bool clear = true, PackedColor color = PackedColor(0)) override;
    void Pause() override;
    void Restore() override;
    void StopAll() override;
    void FogColorChanged(ColorVal color) override
    {
        _fogColor = color;
        _vk.SetFogColor({color.R(), color.G(), color.B(), 1});
    }
    void HandleEvents() override;
    bool IsOpen() const override { return !_failed && _events.IsOpen(); }
    void SetMouseGrab(bool grab) override { _events.SetMouseGrab(grab); }
    bool IsMouseGrabbed() const override { return _events.IsMouseGrabbed(); }
    void StartTextInput() override;
    void StopTextInput() override;
    bool IsTextInputActive() const override;
    bool SwitchRes(int width, int height, int bpp) override;
    bool SwitchRefreshRate(int refresh) override;
    bool SetWindowMode(WindowMode mode) override;
    WindowMode GetCurrentWindowMode() const override { return _mode; }
    void OnFullscreenChanged(bool windowed) override;
    void OnWindowResized(int width, int height) override;
    void ListResolutions(FindArray<ResolutionInfo>& ret) override;
    void ListRefreshRates(FindArray<int>& ret) override;
    void ListMonitors(FindArray<MonitorInfo>& ret) override;
    int GetCurrentMonitor() const override;
    bool GetDesktopDisplayMode(int& width, int& height, int& refresh) const override;
    bool GetCurrentDisplayMode(int& width, int& height, int& refresh) const override;
    bool GetRequestedFullscreenMode(int& width, int& height, int& refresh) const override;
    bool SetSwapInterval(int interval) override;
    int GetSwapInterval() const override { return _vk.SwapInterval(); }
    int Width() const override { return _width; }
    int Height() const override { return _height; }
    int PixelSize() const override { return 32; }
    int RefreshRate() const override;
    bool CanBeWindowed() const override { return true; }
    bool IsWindowed() const override { return _mode == WindowMode::Windowed; }
    bool IsResizable() const override { return IsWindowed(); }
    int AFrameTime() const override { return static_cast<int>(GetLastFrameDuration()); }

    // Unsupported resource/emission entry points fail in EngineVK_Unsupported.cpp.
    using Engine::Draw2D;
    using Engine::DrawLine;
    void PrepareTriangle(const MipInfo&, int) override;
    void PrepareTriangleTL(const MipInfo&, const render::LegacySpec&) override;
    void DrawPolygon(const VertexIndex*, int) override;
    void DrawSection(const FaceArray&, Offset, Offset) override;
    void DrawDecal(Vector3Par, float, float, float, PackedColor, const MipInfo&, int) override;
    void Draw2D(const Draw2DPars&, const Rect2DAbs&, const Rect2DAbs&) override;
    void DrawPoly(const MipInfo&, const Vertex2DAbs*, int, const Rect2DAbs&, int) override;
    void DrawPoly(const MipInfo&, const Vertex2DPixel*, int, const Rect2DPixel&, int) override;
    void DrawLine(const Line2DAbs&, PackedColor, PackedColor, const Rect2DAbs&) override;
    void DrawLine(int, int) override;
    void DrawPoints(int, int) override;
    void PrepareMesh(const render::LegacySpec&) override;
    void BeginMesh(TLVertexTable&, const render::LegacySpec&) override;
    void EndMesh(TLVertexTable&) override;
    void PrepareMeshTL(const LightList&, const Matrix4&, const render::LegacySpec&) override;
    void BeginMeshTL(const Shape&, int, bool) override;
    void EndMeshTL(const Shape&) override;
    bool GetTL() const override { return true; }
    void DrawSectionTL(const Shape&, int, int) override;
    VertexBuffer* CreateVertexBuffer(const Shape&, VBType) override;
    void EmitDraw(const render::frame::Draw&) override;
    void DrawTestPattern(const char*) override;
    void SetMaterial(const TLMaterial&, const LightList&, const render::LegacySpec&) override;
    void BeginShadowPass() override;
    void EndShadowPass() override;
    bool SupportsProjectedShadows() const override { return false; }
    void SetShadowMapsEnabled(bool enabled) override;
    AbstractTextBank* TextBank() override;
    void TextureDestroyed(Texture*) override;
    void SetGamma(float gamma) override;
    float GetGamma() const override { return _vk.Gamma(); }
    float ZShadowEpsilon() const override { return 0; }
    float ZRoadEpsilon() const override { return 0; }
    float ObjMipmapCoef() const override { return 1; }
    void GetZCoefs(float& add, float& mult) override
    {
        const auto coefs = render::zbias::SoftwareCoefs(_bias);
        add = coefs.zAdd;
        mult = coefs.zMult;
    }
    int GetBias() override { return _bias; }
    void SetBias(int value) override;
    bool CanZBias() const override { return false; }
    bool ZBiasExclusion() const override { return false; }

  private:
    SDL_Window* _window = nullptr;
    SDLEventWindow _events;
    vk::VulkanContext _vk;
    TextBankVK _textures;
    WindowMode _mode = WindowMode::Windowed;
    int _width = 0, _height = 0;
    int _drawableWidth = 0, _drawableHeight = 0;
    bool _videoInitialized = false;
    bool _paused = false;
    bool _failed = false;
    bool _meshPrepared = false;
    int _bias = 0;
    Matrix4 _shapeModelView;
    vk::ShapeLighting _lighting;
    bool _sunEnabled = true;
    bool _shapeFog = true;
    const Shape* _activeShape = nullptr;
    std::array<float, 16> _shapeMVP{};
    std::array<float, 4> _shapeColor{1, 1, 1, 1};
    std::array<float, 4> _materialColor{1, 1, 1, 1};
    std::shared_ptr<vk::TextureImage> _sectionTexture;
    std::shared_ptr<vk::TextureImage> _sectionDetail;
    float _secondaryMode = 1;
    std::array<float, 3> _bumpLight{0, -1, 0};
    unsigned _sectionSampler = 0;
    float _sectionAlphaCutoff = 0;
    bool _sectionBlend = false;
    TLVertexTable* _softwareMesh = nullptr;
    MipInfo _softwareMip;
    int _softwareFlags = 0;
    std::vector<vk::ScreenVertex> _softwareVertices;
    void SubmitSoftware(const std::vector<uint32_t>& indices);
    void SubmitScreen(const MipInfo&, const Vertex2DAbs*, int, const Rect2DAbs&, int, float fog = 1);
    void StopAfterFailure(const std::exception& error);
    [[noreturn]] static void Unsupported(const char* feature);
};
} // namespace Poseidon
