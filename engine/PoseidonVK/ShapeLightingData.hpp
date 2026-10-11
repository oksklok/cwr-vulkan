#pragma once
#include <array>

namespace Poseidon::vk
{
struct ShapeLocalLight
{
    std::array<float, 4> position{}, direction{}, diffuse{}, ambient{};
};
// std140, shared with shape_uniforms.glsl. Outside the 128-byte push limit.
struct ShadowLighting
{
    std::array<float, 64> matrices{};
    std::array<float, 4> splits{}, control{}, forward{}, strength{};
};
struct alignas(16) ShapeLighting
{
    std::array<float, 16> world{};
    std::array<float, 12> normal{};
    std::array<float, 4> sunDirection{};
    std::array<float, 4> ambient{}, diffuse{}, emissive{}, specular{};
    std::array<float, 4> fogParams{}, fogColor{};
    std::array<float, 4> eyeCoef{0, 0, 0, 1}, localCount{};
    std::array<ShapeLocalLight, 8> localLights{};
    std::array<float, 4> shadowReceiver{}; // enabled, software projection X/Y
    ShadowLighting shadow{};
    std::array<float, 16> previousMVP{};
    std::array<float, 4> temporal{}; // jitter NDC xy, history valid, reactive
    std::array<float, 4> temporalExtent{}; // inverse width/height
};
} // namespace Poseidon::vk
