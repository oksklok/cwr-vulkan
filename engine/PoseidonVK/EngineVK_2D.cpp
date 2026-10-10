#include <PoseidonVK/EngineVK.hpp>
#include <PoseidonVK/ScreenGeometryVK.hpp>
#include <PoseidonVK/ShapeGeometryVK.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <algorithm>
#include <vector>

namespace Poseidon
{
void EngineVK::DrawDecal(Vector3Par screen, float rhw, float sizeX, float sizeY, PackedColor color, const MipInfo& mip,
                         int flags)
{
    if (sizeX <= 0 || sizeY <= 0)
        return; // Empty billboard.
    // Object/Scene has already projected, lit and colored this billboard.
    // Keep IsLight: its remaining backend meaning is additive blending.
    const int prepared = IsColored | DisableSun | NoShadow | ShadowDisabled | NoDropdown | IsAlphaOrdered | IsAnimated |
                         OnSurface | IsOnSurface;
    Vertex2DAbs vertices[4];
    const float corner[][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i)
    {
        vertices[i].x = screen.X() + (2 * corner[i][0] - 1) * sizeX;
        vertices[i].y = screen.Y() + (2 * corner[i][1] - 1) * sizeY;
        vertices[i].z = screen.Z();
        vertices[i].w = rhw;
        vertices[i].u = corner[i][0];
        vertices[i].v = corner[i][1];
        // Non-alpha-fog decals encode fog, not transparency, in color.a.
        vertices[i].color = flags & IsAlphaFog ? color : PackedColor(color | 0xff000000);
    }
    const float fog = flags & (FogDisabled | NoDropdown | IsAlphaFog | IsLight) ? 1.f : 1.f - float(color >> 24) / 255;
    SubmitScreen(mip, vertices, 4, Rect2DAbs(0, 0, _width, _height), flags & ~prepared, fog);
}

void EngineVK::DrawLine(int begin, int end)
{
    if (!_softwareMesh || begin < 0 || end < 0 || begin >= _softwareMesh->NVertex() || end >= _softwareMesh->NVertex())
        throw std::out_of_range("Vulkan 3D line requires two transformed mesh vertices");
    const auto& a = _softwareMesh->GetVertex(begin);
    const auto& b = _softwareMesh->GetVertex(end);
    const float dx = b.pos.X() - a.pos.X(), dy = b.pos.Y() - a.pos.Y();
    const float length = std::hypot(dx, dy);
    if (length == 0)
        return;
    // GL33's screen-space textured three-pixel ribbon, with endpoint depth,
    // reciprocal-W and engine-computed colors retained (e.g. bullet tracers).
    const float x = dy / length * 1.5f, y = -dx / length * 1.5f;
    Vertex2DAbs vertices[4];
    for (int i = 0; i < 4; ++i)
    {
        const auto& source = i < 2 ? a : b;
        const bool side = i == 1 || i == 2;
        vertices[i].x = source.pos.X() + (side ? x : -x);
        vertices[i].y = source.pos.Y() + (side ? y : -y);
        vertices[i].z = source.pos.Z();
        vertices[i].w = source.rhw;
        vertices[i].u = i < 2 ? 0 : length;
        vertices[i].v = side ? 1 : 0.25f;
        vertices[i].color = source.color;
    }
    Texture* texture = GPreloadedTextures.New(TextureLine);
    DrawPoly(TextBank()->UseMipmap(texture, 1, 1), vertices, 4, Rect2DAbs(0, 0, _width, _height),
             NoZWrite | IsAlpha | ClampU | ClampV | IsAlphaFog);
}

void EngineVK::DrawPoints(int begin, int end)
{
    if (!_softwareMesh || begin < 0 || end < begin || end > _softwareMesh->NVertex())
        throw std::out_of_range("Vulkan points require a transformed mesh range");
    // GL33's subpixel-weighted two-pixel quads (stars and laser dots).
    // Reuse transient pages and the screen constants; these are already lit.
    const float corners[][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const int prepared = DisableSun | IsColored | NoShadow | ShadowDisabled | NoDropdown | IsAnimated | ZBiasMask |
                         SpecLighting;
    for (int index = begin; index < end; ++index)
    {
        if (_softwareMesh->Clip(index) & ClipAll)
            continue;
        const auto& point = _softwareMesh->GetVertex(index);
        if (point.color.A8() < 8)
            continue;
        const float x = std::floor(point.pos.X()), y = std::floor(point.pos.Y());
        const float fx = point.pos.X() - x, fy = point.pos.Y() - y;
        Vertex2DAbs vertices[4];
        for (int i = 0; i < 4; ++i)
        {
            vertices[i].x = x + 0.5f + 2 * corners[i][0];
            vertices[i].y = y + 0.5f + 2 * corners[i][1];
            vertices[i].z = point.pos.Z();
            vertices[i].w = point.rhw;
            vertices[i].u = 0.0f;
            vertices[i].v = 0.0f;
            const float alpha = (corners[i][0] ? fx : 1 - fx) * (corners[i][1] ? fy : 1 - fy) * point.color.A8();
            vertices[i].color = PackedColorRGB(point.color, std::clamp(toInt(alpha), 0, 255));
        }
        SubmitScreen(_softwareMip, vertices, 4, Rect2DAbs(0, 0, _width, _height),
                     (_softwareFlags & ~prepared) | FogDisabled);
    }
}

void EngineVK::DrawLine(const Line2DAbs& line, PackedColor first, PackedColor second, const Rect2DAbs& clip)
{
    const float dx = line.end.x - line.beg.x, dy = line.end.y - line.beg.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length == 0)
        return; // Empty line, no primitive to rasterize.
    const float x = dy / length * 1.5f, y = -dx / length * 1.5f;
    Vertex2DAbs vertices[4];
    vertices[0].x = line.beg.x - x;
    vertices[0].y = line.beg.y - y;
    vertices[1].x = line.beg.x + x;
    vertices[1].y = line.beg.y + y;
    vertices[2].x = line.end.x + x;
    vertices[2].y = line.end.y + y;
    vertices[3].x = line.end.x - x;
    vertices[3].y = line.end.y - y;
    for (int i = 0; i < 4; ++i)
    {
        vertices[i].u = i >= 2 ? 0.1f : 0;
        vertices[i].v = i == 1 || i == 2 ? 1 : 0.25f;
        vertices[i].color = i < 2 ? first : second;
    }
    Texture* texture = GPreloadedTextures.New(TextureLine);
    DrawPoly(TextBank()->UseMipmap(texture, 0, 0), vertices, 4, clip, NoZBuf | IsAlpha | ClampU | ClampV | IsAlphaFog);
}
void EngineVK::Draw2D(const Draw2DPars& pars, const Rect2DAbs& rect, const Rect2DAbs& clip)
{
    Vertex2DAbs vertices[4];
    const float xy[][2] = {
        {rect.x, rect.y}, {rect.x + rect.w, rect.y}, {rect.x + rect.w, rect.y + rect.h}, {rect.x, rect.y + rect.h}};
    const float uv[][2] = {{pars.uTL, pars.vTL}, {pars.uTR, pars.vTR}, {pars.uBR, pars.vBR}, {pars.uBL, pars.vBL}};
    const PackedColor colors[] = {pars.colorTL, pars.colorTR, pars.colorBR, pars.colorBL};
    for (int i = 0; i < 4; ++i)
    {
        vertices[i].x = xy[i][0];
        vertices[i].y = xy[i][1];
        vertices[i].u = uv[i][0];
        vertices[i].v = uv[i][1];
        vertices[i].color = colors[i];
    }
    DrawPoly(pars.mip, vertices, 4, clip, pars.spec);
}
void EngineVK::DrawPoly(const MipInfo& mip, const Vertex2DAbs* vertices, int n, const Rect2DAbs& clip, int flags)
{
    SubmitScreen(mip, vertices, n, clip, flags);
}
void EngineVK::SubmitScreen(const MipInfo& mip, const Vertex2DAbs* vertices, int n, const Rect2DAbs& clip, int flags,
                            float fog)
{
    const int allowed = NoZBuf | NoZWrite | IsAlpha | IsTransparent | IsAlphaFog | ClampU | ClampV | NoClamp |
                        PointSampling | BestMipmap | FogDisabled | IsLight;
    if (flags & ~allowed)
        Unsupported("2D primitive flags outside diffuse alpha sampling");
    if (!_vk.FrameOpen())
        return; // Minimized/paused drawable has no acquired frame.
    if (!mip.IsOK() || !vertices || n < 3)
        throw std::invalid_argument("Vulkan 2D primitive has invalid vertices or mip");
    const int x = std::clamp(int(std::floor(clip.x)), 0, _width);
    const int y = std::clamp(int(std::floor(clip.y)), 0, _height);
    const int right = std::clamp(int(std::ceil(clip.x + clip.w)), x, _width);
    const int bottom = std::clamp(int(std::ceil(clip.y + clip.h)), y, _height);
    if (right == x || bottom == y)
        return;
    const VkRect2D scissor{{x, y}, {unsigned(right - x), unsigned(bottom - y)}};
    std::vector<vk::ScreenVertex> packed;
    std::vector<uint32_t> indices;
    for (int i = 0; i < n; ++i)
    {
        packed.push_back(vk::ScreenGeometry(vertices[i], _width, _height));
        packed.back().fog = fog;
    }
    for (int i = 2; i < n; ++i)
    {
        indices.push_back(0);
        indices.push_back(i - 1);
        indices.push_back(i);
    }
    auto buffer = _vk.UploadTransientMesh(packed.data(), packed.size() * sizeof(packed[0]), indices.data(),
                                          indices.size() * sizeof(indices[0]));
    std::shared_ptr<vk::TextureImage> image;
    auto alpha = AlphaStats::Opaque;
    if (mip._texture)
    {
        auto* texture = dynamic_cast<TextureVK*>(mip._texture);
        if (!texture)
            throw std::logic_error("Vulkan 2D received a foreign texture");
        image = texture->Image(_vk);
        alpha = texture->GetAlphaClass();
    }
    const bool depth = (flags & NoZBuf) == 0;
    const bool blend = alpha == AlphaStats::Blend || (flags & (IsAlpha | IsAlphaFog | IsLight)) != 0;
    // Alpha-fog/transparent effects must fade, not disappear at the opaque
    // cutout threshold. GL33 rejects only near-zero alpha on blended draws.
    const float cutoff = blend ? 1.f / 255 : alpha == AlphaStats::Cutout ? 0.5f : 0;
    _vk.DrawMesh(buffer.buffers, 0, indices.size(), false, {}, {1, 1, 1, 1}, image,
                 vk::ShapeSampler(render::SplitLegacy(flags)), cutoff, blend, true, depth, &scissor, {}, 1, {0, -1, 0},
                 buffer.vertexOffset, buffer.indexOffset, (flags & NoZWrite) == 0, nullptr, false, (flags & IsLight) != 0);
}
void EngineVK::DrawPoly(const MipInfo& mip, const Vertex2DPixel* vertices, int n, const Rect2DPixel& clip, int flags)
{
    if (!vertices || n < 3)
        throw std::invalid_argument("Vulkan 2D pixel polygon is invalid");
    std::vector<Vertex2DAbs> converted(n);
    for (int i = 0; i < n; ++i)
    {
        converted[i].x = vertices[i].x + Left2D();
        converted[i].y = vertices[i].y + Top2D();
        converted[i].z = vertices[i].z;
        converted[i].w = vertices[i].w;
        converted[i].u = vertices[i].u;
        converted[i].v = vertices[i].v;
        converted[i].color = vertices[i].color;
    }
    DrawPoly(mip, converted.data(), n, Rect2DAbs(clip.x + Left2D(), clip.y + Top2D(), clip.w, clip.h), flags);
}
} // namespace Poseidon
