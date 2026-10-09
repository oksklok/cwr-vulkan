#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace Poseidon::vk
{
// Backend-private diagnostic layout; engine VertexIndex is also uint16_t.
// No material, texture, scene-plan or Shape ownership is implied.
struct TriangleVertex
{
    float position[2];
    float color[3];
};
static_assert(sizeof(TriangleVertex) == 20 && offsetof(TriangleVertex, color) == 8);
inline constexpr std::array<TriangleVertex, 3> TriangleVertices{{
    {{0.0f, -0.65f}, {1.0f, 0.0f, 0.0f}},
    {{0.65f, 0.65f}, {0.0f, 1.0f, 0.0f}},
    {{-0.65f, 0.65f}, {0.0f, 0.0f, 1.0f}},
}};
// Non-sequential order exercises uint16 index-buffer fetches.
inline constexpr std::array<uint16_t, 3> TriangleIndices{2, 0, 1};
} // namespace Poseidon::vk
