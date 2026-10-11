#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Poseidon::vk
{
struct TextureImage;
struct ScreenVertex
{
    float position[4], uv[2], color[4];
    float fog = 1;
    float previousClip[4]{};
};
struct ScreenState
{
    struct Clip
    {
        int32_t x = 0, y = 0;
        uint32_t width = 0, height = 0;
        bool operator==(const Clip&) const = default;
    };
    // Hold the exact immutable image version, not a mutable engine Texture.
    std::shared_ptr<TextureImage> image;
    unsigned sampler = 0;
    float cutoff = 0;
    bool blend = false, depthTest = true, depthWrite = true, additive = false;
    Clip clip;
    bool operator==(const ScreenState&) const = default;
};
// One consecutive run only. No sorting, resource lookup or GPU ownership here.
struct ScreenBatch
{
    static constexpr size_t MaxVertices = 65536, MaxIndices = 196608;
    ScreenState state;
    std::vector<ScreenVertex> vertices;
    std::vector<uint32_t> indices;
    bool NeedsFlush(const ScreenState& next, size_t count) const
    {
        return !indices.empty() && (!(state == next) || vertices.size() + count > MaxVertices ||
                                    indices.size() + (count - 2) * 3 > MaxIndices);
    }
    void Append(std::span<const ScreenVertex> polygon)
    {
        const auto base = uint32_t(vertices.size());
        vertices.insert(vertices.end(), polygon.begin(), polygon.end());
        for (uint32_t i = 2; i < polygon.size(); ++i)
            indices.insert(indices.end(), {base, base + i - 1, base + i});
    }
    void Clear()
    {
        vertices.clear();
        indices.clear();
        state.image.reset();
    }
};
} // namespace Poseidon::vk
