#include <PoseidonVK/EngineVK.hpp>
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace Poseidon
{
[[noreturn]] void EngineVK::Unsupported(const char* feature)
{
    throw std::logic_error(std::string("PoseidonVK experimental backend: ") + feature +
                           " is not implemented; use --render gl33 for game rendering");
}

void EngineVK::DrawPoints(int, int)
{
    Unsupported("points");
}
void EngineVK::EmitDraw(const render::frame::Draw&)
{
    Unsupported("indexed draw emission");
}
void EngineVK::BeginShadowPass()
{
    Unsupported("shadow passes");
}
void EngineVK::EndShadowPass()
{
    Unsupported("shadow passes");
}
void EngineVK::SetShadowMapsEnabled(bool enabled)
{
    if (enabled)
        Unsupported("shadow maps");
}
AbstractTextBank* EngineVK::TextBank()
{
    return &_textures;
}
// Frame-held shared images retire at their fences; no address-based GPU handle cache exists.
void EngineVK::TextureDestroyed(Texture*) {}
void EngineVK::SetGamma(float gamma)
{
    if (!std::isfinite(gamma))
        throw std::invalid_argument("Vulkan gamma must be finite");
    _vk.SetGamma(std::clamp(gamma, 1e-3f, 1e3f));
}
} // namespace Poseidon
