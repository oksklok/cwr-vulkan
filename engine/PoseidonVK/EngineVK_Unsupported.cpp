#include <PoseidonVK/EngineVK.hpp>
#include <stdexcept>

namespace Poseidon
{
[[noreturn]] void EngineVK::Unsupported(const char* feature)
{
    throw std::logic_error(std::string("PoseidonVK experimental backend: ") + feature +
                           " is not implemented; use --render gl33 for game rendering");
}

void EngineVK::PrepareTriangle(const MipInfo&, int)
{
    Unsupported("triangle/texture preparation");
}
void EngineVK::DrawPolygon(const VertexIndex*, int)
{
    Unsupported("polygon drawing");
}
void EngineVK::DrawSection(const FaceArray&, Offset, Offset)
{
    Unsupported("face drawing");
}
void EngineVK::DrawDecal(Vector3Par, float, float, float, PackedColor, const MipInfo&, int)
{
    Unsupported("decals");
}
void EngineVK::Draw2D(const Draw2DPars&, const Rect2DAbs&, const Rect2DAbs&)
{
    Unsupported("2D drawing");
}
void EngineVK::DrawPoly(const MipInfo&, const Vertex2DAbs*, int, const Rect2DAbs&, int)
{
    Unsupported("2D polygons");
}
void EngineVK::DrawPoly(const MipInfo&, const Vertex2DPixel*, int, const Rect2DPixel&, int)
{
    Unsupported("2D polygons");
}
void EngineVK::DrawLine(const Line2DAbs&, PackedColor, PackedColor, const Rect2DAbs&)
{
    Unsupported("2D lines");
}
void EngineVK::DrawLine(int, int)
{
    Unsupported("3D lines");
}
void EngineVK::DrawPoints(int, int)
{
    Unsupported("points");
}
void EngineVK::PrepareMesh(const render::LegacySpec&)
{
    Unsupported("mesh preparation");
}
void EngineVK::BeginMesh(TLVertexTable&, const render::LegacySpec&)
{
    Unsupported("mesh upload");
}
void EngineVK::EndMesh(TLVertexTable&)
{
    Unsupported("mesh completion");
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
    if (gamma != 1.0f)
        Unsupported("gamma correction");
}
void EngineVK::SetBias(int value)
{
    if (value != 0)
        Unsupported("depth bias");
}
} // namespace Poseidon
