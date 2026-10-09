#pragma once

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Rendering/RenderFlags.hpp>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace Poseidon::vk
{
inline bool SupportedShapeSpec(const render::LegacySpec& spec)
{
    const auto backend = render::Backend::IsAlpha | render::Backend::IsTransparent | render::Backend::PointSampling |
                         render::Backend::NoClamp | render::Backend::ClampU | render::Backend::ClampV |
                         render::Backend::DetailTexture | render::Backend::SpecularTexture |
                         render::Backend::ZBiasStep | render::Backend::ZBiasMaskHi;
    // IsAnimated denotes engine-selected texture frames, not dynamic vertices.
    const auto material = render::Material::DisableSun | render::Material::BestMipmap | render::Material::IsAnimated;
    const auto routing = render::Routing::IsColored | render::Routing::IsAlphaOrdered | render::Routing::NoShadow |
                         render::Routing::ShadowDisabled | render::Routing::FogDisabled | render::Routing::NoDropdown;
    return (spec.backend & ~backend) == render::Backend::None &&
           (spec.material & ~material) == render::Material::None && (spec.routing & ~routing) == render::Routing::None;
}
inline unsigned ShapeSampler(const render::LegacySpec& spec)
{
    const unsigned point = (spec.backend & render::Backend::PointSampling) != render::Backend::None ? 4 : 0;
    if ((spec.backend & render::Backend::NoClamp) != render::Backend::None)
        return point;
    return point | ((spec.backend & render::Backend::ClampU) != render::Backend::None ? 1 : 0) |
           ((spec.backend & render::Backend::ClampV) != render::Backend::None ? 2 : 0);
}
inline void RequireImmutableShape(bool dynamic, bool dirty)
{
    if (dynamic || dirty)
        throw std::logic_error("Vulkan Shape: immutable buffer was modified; dynamic mesh updates are unsupported");
}
// Same position/negated-normal/UV layout and polygon fans as GL33's SVertex path.
struct ShapeVertex
{
    float position[3];
    float normal[3];
    float uv[2];
};
struct SectionRange
{
    uint32_t begin, end;
};
struct ShapeGeometry
{
    std::vector<ShapeVertex> vertices;
    std::vector<VertexIndex> indices;
    std::vector<SectionRange> sections;
};

inline ShapeGeometry ExtractShapeGeometry(const Shape& shape)
{
    if (shape.NVertex() <= 0 || shape.NNorm() != shape.NVertex())
        throw std::invalid_argument("Vulkan Shape: empty vertices or missing normals");
    ShapeGeometry out;
    out.vertices.reserve(shape.NVertex());
    for (int i = 0; i < shape.NVertex(); ++i)
    {
        const auto& p = shape.Pos(i);
        const auto& n = shape.Norm(i);
        const auto& uv = shape.UV(i);
        out.vertices.push_back({{p.X(), p.Y(), p.Z()}, {-n.X(), -n.Y(), -n.Z()}, {uv.u, uv.v}});
    }
    // Sections cover the face stream in order, as in GL33. Reject malformed ranges
    // rather than uploading indices that cannot be addressed by DrawSectionTL.
    Offset next = shape.BeginFaces();
    for (int s = 0; s < shape.NSections(); ++s)
    {
        const auto& section = shape.GetSection(s);
        if (section.beg != next || section.end < section.beg || section.end > shape.EndFaces())
            throw std::invalid_argument("Vulkan Shape: non-contiguous section face range");
        const auto begin = static_cast<uint32_t>(out.indices.size());
        while (next < section.end)
        {
            const auto& face = shape.Face(next);
            if (face.N() < 3)
                throw std::invalid_argument("Vulkan Shape: face has fewer than three vertices");
            for (int v = 0; v < face.N(); ++v)
                if (static_cast<unsigned>(face.GetVertex(v)) >= out.vertices.size())
                    throw std::invalid_argument("Vulkan Shape: face vertex index out of bounds");
            for (int v = 2; v < face.N(); ++v)
            {
                out.indices.push_back(face.GetVertex(0));
                out.indices.push_back(face.GetVertex(v - 1));
                out.indices.push_back(face.GetVertex(v));
            }
            shape.NextFace(next);
        }
        if (next != section.end)
            throw std::invalid_argument("Vulkan Shape: section ends inside a face");
        out.sections.push_back({begin, static_cast<uint32_t>(out.indices.size())});
    }
    if (next != shape.EndFaces() || out.indices.empty())
        throw std::invalid_argument("Vulkan Shape: faces must have nonempty section coverage");
    return out;
}

inline SectionRange SelectSections(const std::vector<SectionRange>& sections, int begin, int end)
{
    if (begin < 0 || end <= begin || static_cast<size_t>(end) > sections.size())
        throw std::out_of_range("Vulkan Shape: invalid section draw range");
    return {sections[begin].begin, sections[end - 1].end};
}
} // namespace Poseidon::vk
