#pragma once

namespace Poseidon::vk
{
// Depth testing and blending are independent immutable pipeline states.
constexpr unsigned ScreenPipelineIndex(bool depthTest, bool blend)
{
    return (depthTest ? 2u : 0u) | (blend ? 1u : 0u);
}
} // namespace Poseidon::vk
