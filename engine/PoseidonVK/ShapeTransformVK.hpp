#pragma once
#include <Poseidon/Graphics/Core/MatrixConversion.hpp>
#include <array>

namespace Poseidon::vk
{
// Software TL positions already include Camera::ScaleMatrix. Its pixel
// projection also contains world-rectangle offsets, unlike ProjectionNormal.
inline std::array<float, 16> SoftwareProjection(Matrix4Val p, int width, int height)
{
    std::array<float, 16> m{};
    m[0] = 2 * p(0, 0) / width;
    m[5] = 2 * p(1, 1) / height;
    m[8] = 2 * p(0, 2) / width - 1;
    m[9] = 2 * p(1, 2) / height - 1;
    m[10] = p(2, 2);
    m[11] = 1;
    m[14] = p.Position().Z();
    return m;
}
// ConvertMatrix's row storage is column-major GLSL storage. Poseidon already
// projects to Vulkan's 0..1 depth range; only the framebuffer Y axis is flipped.
inline std::array<float, 16> ShapeMVP(Matrix4Val modelToView, Matrix4Val projection, int bias = 0)
{
    GfxMatrix model{}, proj{};
    ConvertMatrix(model, modelToView);
    ConvertProjectionMatrix(proj, projection, bias);
    std::array<float, 16> result{};
    const auto* m = &model._11;
    const auto* p = &proj._11;
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                result[col * 4 + row] += p[k * 4 + row] * m[col * 4 + k];
    for (int col = 0; col < 4; ++col)
        result[col * 4 + 1] = -result[col * 4 + 1];
    return result;
}
} // namespace Poseidon::vk
