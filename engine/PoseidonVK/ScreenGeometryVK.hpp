#pragma once
#include <PoseidonVK/ScreenBatchVK.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <cmath>
#include <stdexcept>

namespace Poseidon::vk
{
// Preserve engine reciprocal-W, UV and packed ARGB colors.
inline ScreenVertex ScreenGeometry(const Vertex2DAbs& vertex, int width, int height)
{
    if (width <= 0 || height <= 0 || !std::isfinite(vertex.w) || vertex.w <= 0)
        throw std::invalid_argument("Vulkan screen vertex requires an extent and positive reciprocal-W");
    const float w = 1 / vertex.w;
    return {{(2 * vertex.x / width - 1) * w, (2 * vertex.y / height - 1) * w, vertex.z * w, w},
            {vertex.u, vertex.v},
            {float((vertex.color >> 16) & 255) / 255, float((vertex.color >> 8) & 255) / 255,
             float(vertex.color & 255) / 255, float((vertex.color >> 24) & 255) / 255}};
}
} // namespace Poseidon::vk
