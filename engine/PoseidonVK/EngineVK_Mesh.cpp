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
void EngineVK::BeginShadowPass()
{
    if (_activeShape || _softwareMesh)
        throw std::logic_error("Vulkan shadow pass requires completed mesh submission");
    _vk.BeginShadowPass();
}
void EngineVK::EndShadowPass()
{
    if (_activeShape || _softwareMesh)
        throw std::logic_error("Vulkan shadow pass ended with an active mesh");
    _vk.EndShadowPass();
}
void EngineVK::SetBias(int value)
{
    _bias = value;
    if (_meshPrepared && GScene && GScene->GetCamera())
        _shapeMVP = vk::ShapeMVP(_shapeModelView, GScene->GetCamera()->ProjectionNormal(), _bias);
}
void EngineVK::SetMaterial(const TLMaterial& mat, const LightList& lights, const render::LegacySpec& spec)
{
    if (!_meshPrepared || !vk::SupportedShapeSpec(spec) || !vk::SupportedShapeSpec(render::SplitLegacy(mat.specFlags)))
        Unsupported("Shape material outside basic diffuse rendering");
    _materialColor = {1, 1, 1, mat.diffuse.A()};
    if (_shapeFlags & IsShadow)
        return; // Object::DrawShadow sets the unlit opacity before BeginMeshTL.
    vk::ShapeMaterial(_lighting, mat, *GScene->MainLight(),
                      _sunEnabled && !render::Has(spec.material, render::Material::DisableSun));
    _lighting.localLights = {};
    int count = 0;
    const float night =
        render::Has(spec.material, render::Material::DisableSun) ? 1.f : GScene->MainLight()->NightEffect();
    if (night > 0)
        for (int i = 0; i < lights.Size() && count < int(_lighting.localLights.size()); ++i)
        {
            if (!lights[i])
                continue;
            LightDescription light;
            lights[i]->GetDescription(light);
            if (light.type != LTPoint && light.type != LTSpotLight)
                continue;
            vk::ShapeLight(_lighting.localLights[count++], light, mat, GScene->GetCamera()->Position(), night);
        }
    _lighting.localCount[0] = float(count);
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
        _secondaryMode = vk::ShapeSecondaryMode(spec);
        if (bump && GScene->MainLight())
        {
            const auto direction = GScene->MainLight()->SunDirection();
            _bumpLight = {direction.X(), direction.Y(), direction.Z()};
        }
    }
    _sectionSampler = vk::ShapeSampler(spec);
    _sectionShadow = ((_shapeFlags | render::MergeLegacy(spec)) & IsShadow) != 0;
    vk::ShapeFog(_lighting, _sectionShadow ? GScene->GetShadowFogMinRange() : GScene->GetFogMinRange(),
                 _sectionShadow ? GScene->GetShadowFogMaxRange() : GScene->GetFogMaxRange(), _fogColor,
                 _shapeFog && !render::Has(spec.routing, render::Routing::FogDisabled | render::Routing::NoDropdown));
    _lighting.eyeCoef = _vk.EyeCoef();
    auto alpha = AlphaStats::Opaque;
    if (mip._texture)
    {
        auto* texture = dynamic_cast<TextureVK*>(mip._texture);
        if (!texture)
            throw std::logic_error("Vulkan section received a foreign texture");
        alpha = texture->GetAlphaClass();
        _sectionTexture = texture->Image(_vk);
    }
    const auto state = vk::ShapeAlpha(render::SplitLegacy(render::MergeLegacy(spec) |
                                       (_shapeFlags & (IsAlpha | IsTransparent | NoZBuf | NoZWrite))),
                                       alpha, _materialColor[3] * _shapeColor[3]);
    _sectionBlend = state.blend;
    _sectionAlphaCutoff = state.cutoff;
    _sectionDepthTest = state.depthTest;
    _sectionDepthWrite = state.depthWrite;
    if (_sectionShadow)
    {
        _lighting.fogParams[3] = GScene->GetShadowFogMaxRange();
        _sectionAlphaCutoff = std::max(1, (GetShadowFactor() * 7) >> 4) / 255.f;
        _sectionDepthTest = true;
        _sectionDepthWrite = false;
    }
}

void EngineVK::PrepareMeshTL(const LightList& lights, const Matrix4& modelToWorld, const render::LegacySpec& spec)
{
    if (!GScene || !GScene->GetCamera() || !GScene->MainLight())
        throw std::logic_error("Vulkan Shape: mesh preparation needs a scene camera and sun");
    if (!vk::SupportedShapeSpec(spec))
    {
        LOG_ERROR(Graphics, "Vulkan TL mesh unsupported flags: 0x{:x}", render::MergeLegacy(spec));
        Unsupported("Shape render flags outside opaque untextured/IsColored");
    }
    const auto* camera = GScene->GetCamera();
    _shapeFlags = render::MergeLegacy(spec);
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
                 _sectionTexture, _sectionSampler, _sectionAlphaCutoff, _sectionBlend, false, _sectionDepthTest, nullptr,
                 _sectionDetail, _secondaryMode, _bumpLight, 0, 0, _sectionDepthWrite, &_lighting, _sectionShadow);
}

void EngineVK::EndMeshTL(const Shape& shape)
{
    if (_activeShape != &shape)
        throw std::logic_error("Vulkan Shape: EndMeshTL must match BeginMeshTL");
    _activeShape = nullptr;
    _meshPrepared = false;
}
} // namespace Poseidon
