#include <PoseidonVK/EngineVK.hpp>
#include <PoseidonVK/ShapeTransformVK.hpp>

#include <Poseidon/Graphics/Shared/WindowPlacement.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <stdexcept>
#include <cstring>

namespace Poseidon
{
namespace
{
void RequireSDL(bool result, const char* operation)
{
    if (!result)
        throw std::runtime_error(std::string("Vulkan SDL3: ") + operation + ": " + SDL_GetError());
}

bool ReadMode(const SDL_DisplayMode* mode, int& width, int& height, int& refresh)
{
    if (!mode)
        return false;
    width = mode->w;
    height = mode->h;
    refresh = static_cast<int>(mode->refresh_rate + 0.5f);
    return true;
}
} // namespace

EngineVK::EngineVK(const GraphicsEngineParams& params) : _textures(_vk)
{
    try
    {
        RequireSDL(SDL_InitSubSystem(SDL_INIT_VIDEO), "initialize video");
        _videoInitialized = true;
        int desktopWidth = 0, desktopHeight = 0, refresh = 0;
        ReadMode(SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()), desktopWidth, desktopHeight, refresh);
        DisplayPlacementInput config;
        config.width = params.width;
        config.height = params.height;
        config.displayMode = params.useWindow ? "windowed" : params.displayMode;
        const auto placement = ResolveWindowPlacement(config, desktopWidth, desktopHeight, refresh);
        const auto& smoke = AppConfig::Instance().VulkanSmoke();
        _window = SDL_CreateWindow(smoke == "shape"      ? "CWRR [Vulkan: engine Shape smoke]"
                                   : smoke == "triangle" ? "CWRR [Vulkan: indexed triangle smoke]"
                                                         : "CWRR [Vulkan: clear/present]",
                                   placement.width, placement.height,
                                   SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
        RequireSDL(_window != nullptr, "create Vulkan window");
        _width = params.width;
        _height = params.height;
        RequireSDL(SetWindowMode(placement.mode), "set initial window mode");
        RequireSDL(SDL_GetWindowSizeInPixels(_window, &_width, &_height), "get drawable size");
        uint32_t extensionCount = 0;
        const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        RequireSDL(extensions != nullptr, "get Vulkan instance extensions");
        _vk.CreateInstance(extensions, extensionCount, AppConfig::Instance().VulkanValidation());
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        RequireSDL(SDL_Vulkan_CreateSurface(_window, _vk.Instance(), nullptr, &surface), "create Vulkan surface");
        _vk.CreateDevice(surface);
        _drawableWidth = _width;
        _drawableHeight = _height;
        // No acquire/present at construction; a zero drawable defers creation.
        _vk.PrepareSwapchain(static_cast<uint32_t>(std::max(_width, 0)), static_cast<uint32_t>(std::max(_height, 0)));
        _events.Attach(_window, _width, _height);
        LOG_INFO(Graphics, "Vulkan: initialized {} (SDL3), {}x{}, diagnostic renderer", _vk.DeviceName(), _width,
                 _height);
    }
    catch (...)
    {
        Shutdown();
        throw;
    }
}

EngineVK::~EngineVK()
{
    Shutdown();
}

unsigned EngineVK::Shutdown() noexcept
{
    _activeShape = nullptr;
    _meshPrepared = false;
    _softwareMesh = nullptr;
    _softwareVertices.clear();
    _softwareMip = MipInfo();
    _events.Detach();
    const unsigned errors = _vk.Shutdown(); // Device/swapchain and surface must die before the SDL window.
    if (_window)
        SDL_DestroyWindow(_window);
    _window = nullptr;
    if (_videoInitialized)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    _videoInitialized = false;
    return errors;
}

void EngineVK::StopAfterFailure(const std::exception& error)
{
    LOG_ERROR(Graphics, "Vulkan: backend stopped: {}", error.what());
    _failed = true;
    // Do not reuse fences/semaphores following failed recording or submission.
    Shutdown();
}

RString EngineVK::GetDebugName() const
{
    return RString(("PoseidonVK / " + _vk.DeviceName() +
                    (AppConfig::Instance().VulkanSmoke() == "shape"      ? " / engine Shape smoke"
                     : AppConfig::Instance().VulkanSmoke() == "triangle" ? " / indexed triangle smoke"
                                                                         : " / clear-present"))
                       .c_str());
}

RString EngineVK::GetRendererName() const
{
    return "Vulkan (SDL3, experimental)";
}

void EngineVK::InitDraw(bool clear, PackedColor color)
{
    if (!_temporalWorldDrawn) ResetTemporalHistory();
    _temporalWorldDrawn = false;
    if (!IsAbleToDraw() || _vk.FrameOpen())
        return;
    try
    {
        int width = 0, height = 0;
        RequireSDL(SDL_GetWindowSizeInPixels(_window, &width, &height), "get frame drawable size");
        if (SDL_GetWindowFlags(_window) & SDL_WINDOW_MINIMIZED)
            return;
        if (width != _drawableWidth || height != _drawableHeight)
            OnWindowResized(width, height);
        if (!_vk.BeginFrame(static_cast<uint32_t>(std::max(width, 0)), static_cast<uint32_t>(std::max(height, 0))))
            return;
        const auto extent = _vk.Extent();
        if (_width != static_cast<int>(extent.width) || _height != static_cast<int>(extent.height))
        {
            _width = static_cast<int>(extent.width);
            _height = static_cast<int>(extent.height);
            FireResizePostHook(_width, _height);
        }
        Engine::InitDraw(clear, color);
        _worldEffectsPending = false;
        _shadowWorld = false;
        _vk.ResetShadowState();
        if (clear)
            Clear(false, true, color);
    }
    catch (const std::exception& error)
    {
        StopAfterFailure(error);
    }
}

void EngineVK::Clear(bool clearZ, bool clear, PackedColor color)
{
    if (clearZ)
    {
        // Vehicle/weapon interior views replace world depth with a short-range
        // projection. Composite the world before that destructive clear.
        FinishWorldEffects();
        _vk.ClearDepth();
    }
    if (clear)
        _vk.Clear(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, 1.0f);
}

void EngineVK::BeginWorldEffects(bool enabled)
{
    _shadowWorld = enabled && _vk.FrameOpen();
    _worldEffectsPending = enabled && _vk.FrameOpen();
    if (_worldEffectsPending && GScene && GScene->GetCamera())
    {
        const auto& p = GScene->GetCamera()->ProjectionNormal();
        _worldProjection = {p(0, 0), p(1, 1), p(2, 2), p.Position().Z()};
        _vk.BeginAAWorld();
        BeginTemporalWorld();
    }
    else
    {
        ResetTemporalHistory();
        _worldEffectsPending = false;
    }
}

void EngineVK::FinishWorldEffects()
{
    _shadowWorld = false;
    if (!_worldEffectsPending)
        return;
    _worldEffectsPending = false;
    if (_vk.TemporalEnabled() && GScene && GScene->GetCamera())
    {
        const auto* camera = GScene->GetCamera();
        _previousWorldView = GScene->ScaledInvTransform();
        _previousProjection = camera->ProjectionNormal();
        _previousSoftwareProjection = vk::SoftwareProjection(camera->Projection(), _width, _height);
        _previousCameraPosition = camera->Position();
        _previousCameraDirection = camera->Direction();
        _previousJitter = _vk.TemporalJitter();
        _temporalCameraValid = true;
    }
    _vk.FinishAAWorld(_worldProjection);
}

bool EngineVK::SetSSAO(bool enabled, float strength, float radius, float bias, float fade)
{
    return _vk.SetSSAO(enabled, strength, radius, bias, fade);
}

void EngineVK::FinishDraw()
{
    if (!_vk.FrameOpen() || _failed)
        return;
    try
    {
        FinishWorldEffects();
        _vk.EndFrame();
        Engine::FinishDraw();
    }
    catch (const std::exception& error)
    {
        StopAfterFailure(error);
    }
}

void EngineVK::Pause()
{
    FinishDraw(); // Consume a successful acquire before pausing.
    _paused = true;
    StopAll();
}

void EngineVK::DrawTestPattern(const char* name)
{
    if (AppConfig::Instance().VulkanSmoke() != "triangle" || !name || std::strcmp(name, "triangle") != 0)
        Unsupported("non-diagnostic test-pattern drawing");
    _vk.DrawDiagnosticTriangle();
}

void EngineVK::Restore()
{
    _paused = false;
    _vk.RequestRecreation();
}

void EngineVK::StopAll()
{
    if (_failed)
        return;
    try
    {
        FinishDraw();
        _vk.WaitIdle();
    }
    catch (const std::exception& error)
    {
        StopAfterFailure(error);
    }
}

void EngineVK::HandleEvents()
{
    if (_window)
        _events.HandleEvents();
}

void EngineVK::OnWindowResized(int width, int height)
{
    _drawableWidth = width;
    _drawableHeight = height;
    _width = width;
    _height = height;
    _vk.RequestRecreation(); // Deferred until next acquire, after any open frame is submitted.
    if (width > 0 && height > 0)
        FireResizePostHook(width, height);
}

void EngineVK::OnFullscreenChanged(bool windowed)
{
    _mode = windowed ? WindowMode::Windowed
                     : (SDL_GetWindowFullscreenMode(_window) ? WindowMode::Fullscreen : WindowMode::Borderless);
    _vk.RequestRecreation();
}

bool EngineVK::SetWindowMode(WindowMode mode)
{
    if (!_window)
        return false;
    if (mode == WindowMode::Fullscreen)
    {
        SDL_DisplayMode closest{};
        if (!SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(_window), std::max(_width, 1),
                                                 std::max(_height, 1), 0, true, &closest) ||
            !SDL_SetWindowFullscreenMode(_window, &closest))
            return false;
    }
    else if (!SDL_SetWindowFullscreenMode(_window, nullptr))
        return false;
    if (!SDL_SetWindowFullscreen(_window, mode != WindowMode::Windowed))
        return false;
    _mode = mode;
    _vk.RequestRecreation();
    return true;
}

bool EngineVK::SwitchRes(int width, int height, int bpp)
{
    if (!_window || width <= 0 || height <= 0 || (bpp != 16 && bpp != 32))
        return false;
    if (_mode == WindowMode::Windowed)
        return SDL_SetWindowSize(_window, width, height); // Pixel-size event updates the swapchain.
    if (_mode == WindowMode::Borderless)
        return false; // Desktop extent is controlled by SDL, not a render-scale setting.
    SDL_DisplayMode closest{};
    if (!SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(_window), width, height, 0, true, &closest) ||
        !SDL_SetWindowFullscreenMode(_window, &closest))
        return false;
    _vk.RequestRecreation();
    return true;
}

bool EngineVK::SwitchRefreshRate(int refresh)
{
    if (!_window || _mode != WindowMode::Fullscreen || refresh <= 0)
        return false;
    SDL_DisplayMode closest{};
    if (!SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(_window), _width, _height,
                                             static_cast<float>(refresh), true, &closest) ||
        !SDL_SetWindowFullscreenMode(_window, &closest))
        return false;
    _vk.RequestRecreation();
    return true;
}

void EngineVK::ListResolutions(FindArray<ResolutionInfo>& ret)
{
    ret.Clear();
    int count = 0;
    auto modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(_window), &count);
    if (!modes)
        return;
    for (int i = 0; i < count; ++i)
        if (modes[i])
            ret.AddUnique(ResolutionInfo{modes[i]->w, modes[i]->h, 32});
    SDL_free(modes);
}

void EngineVK::ListRefreshRates(FindArray<int>& ret)
{
    ret.Clear();
    int count = 0;
    auto modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(_window), &count);
    if (!modes)
        return;
    for (int i = 0; i < count; ++i)
        if (modes[i] && modes[i]->w == _width && modes[i]->h == _height)
            ret.AddUnique(static_cast<int>(modes[i]->refresh_rate + 0.5f));
    SDL_free(modes);
}

bool EngineVK::GetDesktopDisplayMode(int& width, int& height, int& refresh) const
{
    return _window && ReadMode(SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(_window)), width, height, refresh);
}

void EngineVK::ListMonitors(FindArray<MonitorInfo>& ret)
{
    ret.Clear();
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays)
        return;
    for (int i = 0; i < count; ++i)
    {
        MonitorInfo info{};
        info.index = i;
        const char* name = SDL_GetDisplayName(displays[i]);
        info.name = name ? name : "SDL display";
        ReadMode(SDL_GetDesktopDisplayMode(displays[i]), info.w, info.h, info.refresh);
        ret.Add(info);
    }
    SDL_free(displays);
}

int EngineVK::GetCurrentMonitor() const
{
    if (!_window)
        return 0;
    int count = 0, index = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays)
        return 0;
    const auto current = SDL_GetDisplayForWindow(_window);
    for (int i = 0; i < count; ++i)
        if (displays[i] == current)
            index = i;
    SDL_free(displays);
    return index;
}

bool EngineVK::GetCurrentDisplayMode(int& width, int& height, int& refresh) const
{
    return _window && ReadMode(SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(_window)), width, height, refresh);
}

bool EngineVK::GetRequestedFullscreenMode(int& width, int& height, int& refresh) const
{
    return _window && ReadMode(SDL_GetWindowFullscreenMode(_window), width, height, refresh);
}

int EngineVK::RefreshRate() const
{
    int width = 0, height = 0, refresh = 0;
    GetCurrentDisplayMode(width, height, refresh);
    return refresh;
}

bool EngineVK::SetSwapInterval(int interval)
{
    if (_failed || interval < -1 || interval > 1)
        return false;
    _vk.SetSwapInterval(interval);
    return true;
}

void EngineVK::StartTextInput()
{
    if (_window)
        SDL_StartTextInput(_window);
}
void EngineVK::StopTextInput()
{
    if (_window)
        SDL_StopTextInput(_window);
}
bool EngineVK::IsTextInputActive() const
{
    return _window && SDL_TextInputActive(_window);
}
} // namespace Poseidon
