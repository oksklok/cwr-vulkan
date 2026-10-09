#include <PoseidonVK/EngineVK.hpp>
#include <PoseidonVK/VertexBufferVK.hpp>
#include <PoseidonVK/ShapeTransformVK.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>

namespace Poseidon
{
void EngineVK::PrepareMeshTL(const LightList&, const Matrix4& modelToWorld, const render::LegacySpec& spec)
{
    if (!_vk.FrameOpen() || !GScene || !GScene->GetCamera())
        throw std::logic_error("Vulkan Shape: mesh preparation needs an open frame and scene camera");
    // This stage is strictly opaque, untextured, unlit geometry. Do not accept
    // blend/shadow/depth overrides or animated materials as successful draws.
    if (spec.backend != render::Backend::None ||
        (spec.material != render::Material::None && spec.material != render::Material::DisableSun) ||
        (spec.routing != render::Routing::None && spec.routing != render::Routing::IsColored))
        Unsupported("Shape render flags outside opaque untextured/IsColored");
    const auto* camera = GScene->GetCamera();
    Matrix4 relative = modelToWorld;
    relative.SetPosition(modelToWorld.Position() - camera->Position());
    Matrix4 view = camera->InverseScaled();
    view.SetPosition(VZero);
    _shapeMVP = vk::ShapeMVP(view * relative, camera->ProjectionNormal());
    _shapeColor = {1, 1, 1, 1};
    if (spec.routing == render::Routing::IsColored)
    {
        const auto color = GScene->GetConstantColor();
        if (color.A() != 1)
            Unsupported("translucent Shape color");
        _shapeColor = {color.R(), color.G(), color.B(), 1};
    }
    _meshPrepared = true;
}

void EngineVK::BeginMeshTL(const Shape& shape, int spec, bool dynamic)
{
    if (!_meshPrepared || _activeShape)
        throw std::logic_error("Vulkan Shape: BeginMeshTL requires preparation and no active mesh");
    if (spec != 0)
        Unsupported("BeginMeshTL flags");
    auto* buffer = dynamic_cast<VertexBufferVK*>(shape.GetVertexBuffer());
    if (!buffer)
        throw std::logic_error("Vulkan Shape: mesh has no Vulkan vertex buffer");
    buffer->Update(shape, dynamic);
    _activeShape = &shape;
}

void EngineVK::DrawSectionTL(const Shape& shape, int begin, int end)
{
    if (!_meshPrepared || _activeShape != &shape)
        throw std::logic_error("Vulkan Shape: DrawSectionTL requires the active prepared mesh");
    auto* buffer = dynamic_cast<VertexBufferVK*>(shape.GetVertexBuffer());
    if (!buffer)
        throw std::logic_error("Vulkan Shape: vertex buffer was released during drawing");
    const auto range = vk::SelectSections(buffer->Sections(), begin, end);
    for (int i = begin; i < end; ++i)
    {
        const auto& section = shape.GetSection(i);
        if (section.properties.GetTexture() || section.surfMat || section.properties.Special() != 0 ||
            section.material != 0)
            Unsupported("textured/material Shape sections");
    }
    _vk.DrawMesh(buffer->Buffers(), range.begin, range.end - range.begin, sizeof(VertexIndex) == 2, _shapeMVP,
                 _shapeColor);
}

void EngineVK::EndMeshTL(const Shape& shape)
{
    if (_activeShape != &shape)
        throw std::logic_error("Vulkan Shape: EndMeshTL must match BeginMeshTL");
    _activeShape = nullptr;
    _meshPrepared = false;
}
} // namespace Poseidon
