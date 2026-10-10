#pragma once
#include <PoseidonVK/ShapeTransformVK.hpp>
#include <PoseidonVK/ShapeLightingData.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>

namespace Poseidon::vk
{
inline void ShapeWorld(ShapeLighting& out, Matrix4Val relativeWorld)
{
    GfxMatrix matrix{};
    ConvertMatrix(matrix, relativeWorld);
    std::copy_n(&matrix._11, 16, out.world.begin());
    const auto inverse = relativeWorld.Orientation().InverseGeneral();
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            out.normal[col * 4 + row] = inverse(col, row);
}
inline void ShapeMaterial(ShapeLighting& out, const TLMaterial& material, const LightSun& sun, bool enabled)
{
    const auto ambient = sun.Ambient() * material.ambient + sun.Diffuse() * material.forcedDiffuse;
    const auto diffuse = sun.Diffuse() * material.diffuse;
    out.ambient = {ambient.R(), ambient.G(), ambient.B(), enabled ? 1.f : 0.f};
    out.diffuse = {diffuse.R(), diffuse.G(), diffuse.B(), 0};
    out.emissive = {material.emmisive.R(), material.emmisive.G(), material.emmisive.B(), 0};
    const auto direction = sun.Direction();
    out.sunDirection = {direction.X(), direction.Y(), direction.Z(), 0};
}
inline void ShapeFog(ShapeLighting& out, float start, float end, ColorVal color, bool enabled)
{
    out.fogParams = {start, end > start ? 1.f / (end - start) : 0, enabled ? 1.f : 0.f, 0};
    out.fogColor = {color.R(), color.G(), color.B(), 1};
}
inline void ShapeLight(ShapeLocalLight& out, const LightDescription& light, const TLMaterial& material,
                       Vector3Par camera, float night)
{
    const auto position = light.pos - camera;
    const auto direction = light.dir.Normalized();
    const auto diffuse = light.diffuse * material.diffuse * night;
    const auto ambient = light.ambient * material.ambient * night;
    out.position = {position.X(), position.Y(), position.Z(), light.startAtten};
    out.direction = {direction.X(), direction.Y(), direction.Z(), light.type == LTSpotLight ? 1.f : 0.f};
    out.diffuse = {diffuse.R(), diffuse.G(), diffuse.B(), 0};
    out.ambient = {ambient.R(), ambient.G(), ambient.B(), 0};
}
} // namespace Poseidon::vk
