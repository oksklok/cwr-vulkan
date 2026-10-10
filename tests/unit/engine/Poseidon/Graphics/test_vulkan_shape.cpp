#include <catch2/catch_test_macros.hpp>
#include <PoseidonVK/ShapeGeometryVK.hpp>
#include <PoseidonVK/ShapeTransformVK.hpp>
#include <PoseidonVK/ScreenGeometryVK.hpp>
#include <PoseidonVK/ScreenPipelineVK.hpp>
#include <catch2/catch_approx.hpp>

using namespace Poseidon;

TEST_CASE("Vulkan screen pipeline keys keep every depth blend combination independent", "[Graphics][vulkan-shape]")
{
    REQUIRE(vk::ScreenPipelineIndex(false, false) == 0);
    REQUIRE(vk::ScreenPipelineIndex(false, true) == 1);
    REQUIRE(vk::ScreenPipelineIndex(true, false) == 2);
    REQUIRE(vk::ScreenPipelineIndex(true, true) == 3);
}

TEST_CASE("Vulkan screen packing preserves pixels UV depth reciprocal W and ARGB", "[Graphics][vulkan-shape]")
{
    Vertex2DAbs vertex;
    vertex.x = 600;
    vertex.y = 150;
    vertex.z = 0.75f;
    vertex.w = 0.5f;
    vertex.u = 0.2f;
    vertex.v = 0.8f;
    vertex.color = PackedColor(0x80402010);
    const auto packed = vk::ScreenGeometry(vertex, 800, 600);
    REQUIRE(packed.position[0] == Catch::Approx(1));
    REQUIRE(packed.position[1] == Catch::Approx(-1));
    REQUIRE(packed.position[2] == Catch::Approx(1.5));
    REQUIRE(packed.position[3] == Catch::Approx(2));
    REQUIRE(packed.uv[0] == Catch::Approx(0.2));
    REQUIRE(packed.uv[1] == Catch::Approx(0.8));
    REQUIRE(packed.color[0] == Catch::Approx(64.f / 255));
    REQUIRE(packed.color[1] == Catch::Approx(32.f / 255));
    REQUIRE(packed.color[2] == Catch::Approx(16.f / 255));
    REQUIRE(packed.color[3] == Catch::Approx(128.f / 255));
    vertex.w = 0;
    REQUIRE_THROWS_AS(vk::ScreenGeometry(vertex, 800, 600), std::invalid_argument);
    vertex.w = 1;
    REQUIRE_THROWS_AS(vk::ScreenGeometry(vertex, 0, 600), std::invalid_argument);
}

TEST_CASE("Vulkan Shape extraction preserves polygon fans and section ranges", "[Graphics][vulkan-shape]")
{
    Shape shape;
    for (int i = 0; i < 5; ++i)
        shape.AddVertexFast(Vector3(float(i), 2, 3), Vector3(0, 1, 0), 0, 0.25f, 0.75f);
    Poly face;
    face.Init();
    face.SetN(4);
    for (int i = 0; i < 4; ++i)
        face.Set(i, i);
    const auto first = shape.EndFaces();
    shape.AddFace(face);
    const auto second = shape.EndFaces();
    face.SetN(3);
    face.Set(0, 4);
    face.Set(1, 2);
    face.Set(2, 0);
    shape.AddFace(face);
    ShapeSection section;
    section.properties.Init();
    section.material = 0;
    section.beg = first;
    section.end = second;
    shape.AddSection(section);
    section.beg = second;
    section.end = shape.EndFaces();
    shape.AddSection(section);
    auto data = vk::ExtractShapeGeometry(shape);
    REQUIRE(data.indices == std::vector<VertexIndex>{0, 1, 2, 0, 2, 3, 4, 2, 0});
    REQUIRE(data.vertices.size() == 5);
    REQUIRE(data.vertices[4].position[0] == 4);
    REQUIRE(data.vertices[0].normal[1] == -1);
    REQUIRE(data.vertices[0].uv[1] == 0.75f);
    REQUIRE(vk::SelectSections(data.sections, 0, 1).end == 6);
    REQUIRE(vk::SelectSections(data.sections, 1, 2).begin == 6);
    REQUIRE(vk::SelectSections(data.sections, 0, 2).end == 9);
    REQUIRE_THROWS_AS(vk::SelectSections(data.sections, -1, 1), std::out_of_range);
    REQUIRE_THROWS_AS(vk::SelectSections(data.sections, 0, 3), std::out_of_range);
    shape.Face(second).Set(0, 99);
    REQUIRE_THROWS_AS(vk::ExtractShapeGeometry(shape), std::invalid_argument);
}

TEST_CASE("Vulkan Shape extraction rejects empty and unsectioned shapes", "[Graphics][vulkan-shape]")
{
    Shape shape;
    REQUIRE_THROWS_AS(vk::ExtractShapeGeometry(shape), std::invalid_argument);
    for (int i = 0; i < 3; ++i)
        shape.AddVertexFast(Vector3(float(i), 0, 0), VUp, 0, 0, 0);
    Poly face;
    face.Init();
    face.SetN(3);
    face.Set(0, 0);
    face.Set(1, 1);
    face.Set(2, 2);
    shape.AddFace(face);
    REQUIRE_THROWS_AS(vk::ExtractShapeGeometry(shape), std::invalid_argument);
}

TEST_CASE("Vulkan Shape projection preserves transforms and 0..1 depth", "[Graphics][vulkan-shape]")
{
    Matrix4 model(MIdentity);
    model.SetDirectionAndUp(Vector3(1, 0, 0), VUp);
    model.SetPosition(Vector3(2, 3, 7));
    Matrix4 projection(MZero);
    projection(0, 0) = 2;
    projection(1, 1) = 3;
    const float near = 0.5f, far = 100;
    const float q = far / (far - near);
    projection(2, 2) = q;
    projection.SetPosition(Vector3(0, 0, -q * near));
    const auto mvp = vk::ShapeMVP(model, projection);
    const Vector3 point(1, 2, 3);
    const Vector3 view = model.FastTransform(point);
    float clip[4]{};
    for (int row = 0; row < 4; ++row)
        clip[row] = mvp[row] * point.X() + mvp[4 + row] * point.Y() + mvp[8 + row] * point.Z() + mvp[12 + row];
    REQUIRE(clip[0] == Catch::Approx(view.X() * 2));
    REQUIRE(clip[1] == Catch::Approx(-view.Y() * 3));
    REQUIRE(clip[2] == Catch::Approx(q * view.Z() - q * near));
    REQUIRE(clip[3] == Catch::Approx(view.Z()));
    const auto p = vk::ShapeMVP(MIdentity, projection);
    REQUIRE((p[10] * near + p[14]) / near == Catch::Approx(0).margin(1e-6));
    REQUIRE((p[10] * far + p[14]) / far == Catch::Approx(1));
}

TEST_CASE("Vulkan Shape accepts opaque color but rejects unfinished render states and updates",
          "[Graphics][vulkan-shape]")
{
    render::LegacySpec spec;
    REQUIRE(vk::SupportedShapeSpec(spec));
    spec.routing = render::Routing::IsColored;
    spec.material = render::Material::DisableSun;
    REQUIRE(vk::SupportedShapeSpec(spec));
    spec.backend = render::Backend::NoZWrite;
    REQUIRE_FALSE(vk::SupportedShapeSpec(spec));
    spec.backend = render::Backend::IsTransparent;
    REQUIRE(vk::SupportedShapeSpec(spec));
    spec.backend = render::Backend::ClampU | render::Backend::PointSampling;
    REQUIRE(vk::ShapeSampler(spec) == 5);
    spec.backend = spec.backend | render::Backend::NoClamp;
    REQUIRE(vk::ShapeSampler(spec) == 4);
    spec.backend = render::Backend::None;
    spec.material = render::Material::IsAnimated;
    REQUIRE(vk::SupportedShapeSpec(spec));
    spec.material = render::Material::None;
    spec.routing = render::Routing::OnSurface;
    REQUIRE_FALSE(vk::SupportedShapeSpec(spec));
    REQUIRE_NOTHROW(vk::RequireImmutableShape(false, false));
    REQUIRE_THROWS_AS(vk::RequireImmutableShape(true, false), std::logic_error);
    REQUIRE_THROWS_AS(vk::RequireImmutableShape(false, true), std::logic_error);
}
