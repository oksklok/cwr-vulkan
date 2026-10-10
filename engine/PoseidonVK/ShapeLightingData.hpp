#pragma once
#include <array>

namespace Poseidon::vk
{
struct ShapeLocalLight
{
    std::array<float, 4> position{}, direction{}, diffuse{}, ambient{};
};
// std140, shared with shape_uniforms.glsl. Outside the 128-byte push limit.
struct alignas(16) ShapeLighting
{
    std::array<float, 16> world{};
    std::array<float, 12> normal{};
    std::array<float, 4> sunDirection{};
    std::array<float, 4> ambient{}, diffuse{}, emissive{};
    std::array<float, 4> fogParams{}, fogColor{};
    std::array<float, 4> eyeCoef{0, 0, 0, 1}, localCount{};
    std::array<ShapeLocalLight, 8> localLights{};
};
} // namespace Poseidon::vk
