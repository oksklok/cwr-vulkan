#include <PoseidonVK/EngineVK.hpp>
#include <PoseidonVK/ScreenGeometryVK.hpp>
#include <PoseidonVK/ShapeGeometryVK.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <algorithm>
#include <vector>

namespace Poseidon
{
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
    const int allowed = NoZBuf | NoZWrite | IsAlpha | IsTransparent | IsAlphaFog | ClampU | ClampV | NoClamp |
                        PointSampling | BestMipmap | FogDisabled;
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
        packed.push_back(vk::ScreenGeometry(vertices[i], _width, _height));
    for (int i = 2; i < n; ++i)
    {
        indices.push_back(0);
        indices.push_back(i - 1);
        indices.push_back(i);
    }
    auto buffer = _vk.UploadMesh(packed.data(), packed.size() * sizeof(packed[0]), indices.data(),
                                 indices.size() * sizeof(indices[0]));
    std::shared_ptr<vk::TextureImage> image;
    if (mip._texture)
    {
        auto* texture = dynamic_cast<TextureVK*>(mip._texture);
        if (!texture)
            throw std::logic_error("Vulkan 2D received a foreign texture");
        image = texture->Image(_vk);
    }
    const bool depth = (flags & NoZBuf) == 0;
    _vk.DrawMesh(buffer, 0, indices.size(), false, {}, {1, 1, 1, 1}, image,
                 vk::ShapeSampler(render::SplitLegacy(flags)), 0, true, true, depth, &scissor);
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
