#include <PoseidonVK/VertexBufferVK.hpp>
#include <PoseidonVK/EngineVK.hpp>

namespace Poseidon
{
namespace
{
bool ImmutableBuffer(const Shape& shape, VBType type)
{
    // RemmemberShadow builds these once after terrain fitting, and retires
    // them when its cache entry changes. Other small/dynamic buffers still TL.
    return type == VBStatic || type == VBBigDiscardable ||
           (type == VBSmallDiscardable && (shape.Special() & IsShadow));
}
}
VertexBufferVK::VertexBufferVK(vk::VulkanContext& context, const Shape& shape, VBType type)
{
    if (!ImmutableBuffer(shape, type))
        throw std::logic_error("Vulkan Shape: only immutable geometry and cached shadow buffers are supported");
    auto geometry = vk::ExtractShapeGeometry(shape);
    _sections = std::move(geometry.sections);
    _buffers = context.UploadMesh(geometry.vertices.data(), geometry.vertices.size() * sizeof(vk::ShapeVertex),
                                  geometry.indices.data(), geometry.indices.size() * sizeof(VertexIndex));
}

void VertexBufferVK::Update(const Shape&, bool dynamic)
{
    vk::RequireImmutableShape(dynamic, bufferDirty);
}

VertexBuffer* EngineVK::CreateVertexBuffer(const Shape& shape, VBType type)
{
    if (!ImmutableBuffer(shape, type))
    {
        LOG_DEBUG(Graphics,
                  "Vulkan: declining dynamic/discardable vertex buffer type {} ({} vertices); use engine software "
                  "transformation",
                  int(type), shape.NVertex());
        return nullptr; // Engine contract: unavailable hardware buffer, not a successful draw.
    }
    return new VertexBufferVK(_vk, shape, type);
}
} // namespace Poseidon
