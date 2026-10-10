#pragma once
#include <array>

namespace Poseidon::vk
{
// std140, shared with shape_uniforms.glsl. Outside the 128-byte push limit.
struct alignas(16) ShapeLighting
{
    std::array<float, 16> world{};
    std::array<float, 12> normal{};
    std::array<float, 4> sunDirection{};
    std::array<float, 4> ambient{}, diffuse{}, emissive{};
    std::array<float, 4> fogParams{}, fogColor{};
};
} // namespace Poseidon::vk
