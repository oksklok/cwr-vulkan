#include <PoseidonVK/EngineVK.hpp>
#include <PoseidonVK/VertexBufferVK.hpp>
#include <PoseidonVK/ShapeTransformVK.hpp>
#include <PoseidonVK/ShapeLightingVK.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>

namespace Poseidon
{
void EngineVK::SetBias(int value)
{
    _bias = value;
    if (_meshPrepared && GScene && GScene->GetCamera())
        _shapeMVP = vk::ShapeMVP(_shapeModelView, GScene->GetCamera()->ProjectionNormal(), _bias);
}
void EngineVK::SetMaterial(const TLMaterial& mat, const LightList& lights, const render::LegacySpec& spec)
{
    if (!_activeShape || !vk::SupportedShapeSpec(spec) || !vk::SupportedShapeSpec(render::SplitLegacy(mat.specFlags)))
        Unsupported("Shape material outside basic unlit diffuse");
    _materialColor = {1, 1, 1, mat.diffuse.A()};
    vk::ShapeMaterial(_lighting, mat, *GScene->MainLight(),
                      _sunEnabled && !render::Has(spec.material, render::Material::DisableSun));
}

void EngineVK::PrepareTriangleTL(const MipInfo& mip, const render::LegacySpec& spec)
{
    if (!_activeShape || !vk::SupportedShapeSpec(spec))
    {
        LOG_ERROR(Graphics, "Vulkan TL section unsupported flags: 0x{:x}", render::MergeLegacy(spec));
        Unsupported("TL section preparation outside opaque geometry");
    }
    _sectionTexture.reset();
    _sectionDetail.reset();
    const bool bump = (spec.backend & render::Backend::SpecularTexture) != render::Backend::None;
    if (bump || (spec.backend & render::Backend::DetailTexture) != render::Backend::None)
    {
        const auto texture = _textures.Load(Remaster >> "CfgDetailTextures" >> (bump ? "specular" : "detail"));
        if (!texture)
            throw std::runtime_error("Vulkan terrain secondary texture is missing");
        _sectionDetail = static_cast<TextureVK*>(texture.GetRef())->Image(_vk);
        _secondaryMode = bump ? 2 : 1;
        if (bump && GScene->MainLight())
        {
            const auto direction = GScene->MainLight()->SunDirection();
            _bumpLight = {direction.X(), direction.Y(), direction.Z()};
        }
    }
    _sectionSampler = vk::ShapeSampler(spec);
    vk::ShapeFog(_lighting, GScene->GetFogMinRange(), GScene->GetFogMaxRange(), _fogColor,
                 _shapeFog && !render::Has(spec.routing, render::Routing::FogDisabled | render::Routing::NoDropdown));
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
    if (!GScene || !GScene->GetCamera())
        throw std::logic_error("Vulkan Shape: mesh preparation needs a scene camera");
    if (!vk::SupportedShapeSpec(spec))
    {
        LOG_ERROR(Graphics, "Vulkan TL mesh unsupported flags: 0x{:x}", render::MergeLegacy(spec));
        Unsupported("Shape render flags outside opaque untextured/IsColored");
    }
    const auto* camera = GScene->GetCamera();
    Matrix4 relative = modelToWorld;
    relative.SetPosition(modelToWorld.Position() - camera->Position());
    vk::ShapeWorld(_lighting, relative);
    _sunEnabled = !render::Has(spec.material, render::Material::DisableSun);
    _shapeFog = (spec.routing & (render::Routing::FogDisabled | render::Routing::NoDropdown)) == render::Routing::None;
    Matrix4 view = camera->InverseScaled();
    view.SetPosition(VZero);
    _shapeModelView = view * relative;
    _shapeMVP = vk::ShapeMVP(_shapeModelView, camera->ProjectionNormal(), _bias);
    _shapeColor = {1, 1, 1, 1};
    if ((spec.routing & render::Routing::IsColored) != render::Routing::None)
    {
        const auto color = GScene->GetConstantColor();
        _shapeColor = {color.R(), color.G(), color.B(), color.A()};
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
    if (!_vk.FrameOpen())
        return; // Minimized drawable: no acquired image, but unsupported states above still fail.
    _vk.DrawMesh(buffer->Buffers(), range.begin, range.end - range.begin, sizeof(VertexIndex) == 2, _shapeMVP, color,
                 _sectionTexture, _sectionSampler, _sectionAlphaCutoff, _sectionBlend, false, true, nullptr,
                 _sectionDetail, _secondaryMode, _bumpLight, 0, 0, true, &_lighting);
}

void EngineVK::EndMeshTL(const Shape& shape)
{
    if (_activeShape != &shape)
        throw std::logic_error("Vulkan Shape: EndMeshTL must match BeginMeshTL");
    _activeShape = nullptr;
    _meshPrepared = false;
}
} // namespace Poseidon
