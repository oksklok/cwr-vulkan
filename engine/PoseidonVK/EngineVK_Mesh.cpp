#include <PoseidonVK/EngineVK.hpp>
#include <PoseidonVK/VertexBufferVK.hpp>
#include <PoseidonVK/ShapeTransformVK.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>

namespace Poseidon
{
void EngineVK::SetMaterial(const TLMaterial& mat, const LightList& lights, const render::LegacySpec& spec)
{
    if (!_activeShape || !vk::SupportedShapeSpec(spec) || lights.Size() != 0 ||
        !vk::SupportedShapeSpec(render::SplitLegacy(mat.specFlags)))
        Unsupported("Shape material outside basic unlit diffuse");
    _materialColor = {mat.diffuse.R() + mat.emmisive.R(), mat.diffuse.G() + mat.emmisive.G(),
                      mat.diffuse.B() + mat.emmisive.B(), mat.diffuse.A()};
    // This is explicitly an unlit diffuse/emissive approximation: no specular
    // lobe is evaluated, including for stock glass materials.
}

void EngineVK::PrepareTriangleTL(const MipInfo& mip, const render::LegacySpec& spec)
{
    if (!_activeShape || !vk::SupportedShapeSpec(spec))
        Unsupported("TL section preparation outside opaque geometry");
    _sectionTexture.reset();
    _sectionSampler = vk::ShapeSampler(spec);
    _sectionAlphaCutoff = 0;
    _sectionBlend = _materialColor[3] < 1 || _shapeColor[3] < 1;
    if (mip._texture)
    {
        auto* texture = dynamic_cast<TextureVK*>(mip._texture);
        if (!texture)
            throw std::logic_error("Vulkan section received a foreign texture");
        _sectionAlphaCutoff = texture->GetAlphaClass() == AlphaStats::Cutout ? 0.5f : 0;
        _sectionBlend |= texture->GetAlphaClass() == AlphaStats::Blend;
        _sectionTexture = texture->Image(_vk);
    }
}

void EngineVK::PrepareMeshTL(const LightList& lights, const Matrix4& modelToWorld, const render::LegacySpec& spec)
{
    if (!_vk.FrameOpen() || !GScene || !GScene->GetCamera())
        throw std::logic_error("Vulkan Shape: mesh preparation needs an open frame and scene camera");
    // This stage is strictly opaque, untextured, unlit geometry. Do not accept
    // blend/shadow/depth overrides or animated materials as successful draws.
    if (!vk::SupportedShapeSpec(spec))
        Unsupported("Shape render flags outside opaque untextured/IsColored");
    if (lights.Size() != 0)
        Unsupported("Shape lighting");
    const auto* camera = GScene->GetCamera();
    Matrix4 relative = modelToWorld;
    relative.SetPosition(modelToWorld.Position() - camera->Position());
    Matrix4 view = camera->InverseScaled();
    view.SetPosition(VZero);
    _shapeMVP = vk::ShapeMVP(view * relative, camera->ProjectionNormal());
    _shapeColor = {1, 1, 1, 1};
    if ((spec.routing & render::Routing::IsColored) != render::Routing::None)
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
    if (!vk::SupportedShapeSpec(render::SplitLegacy(spec)))
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
        if (!vk::SupportedShapeSpec(render::SplitLegacy(section.properties.Special())))
            Unsupported("textured/material Shape sections");
    }
    auto color = _shapeColor;
    for (int i = 0; i < 4; ++i)
        color[i] *= _materialColor[i];
    _vk.DrawMesh(buffer->Buffers(), range.begin, range.end - range.begin, sizeof(VertexIndex) == 2, _shapeMVP, color,
                 _sectionTexture, _sectionSampler, _sectionAlphaCutoff, _sectionBlend);
}

void EngineVK::EndMeshTL(const Shape& shape)
{
    if (_activeShape != &shape)
        throw std::logic_error("Vulkan Shape: EndMeshTL must match BeginMeshTL");
    _activeShape = nullptr;
    _meshPrepared = false;
}
} // namespace Poseidon
