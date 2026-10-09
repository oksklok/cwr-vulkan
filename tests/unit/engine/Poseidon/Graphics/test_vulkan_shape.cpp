#include <catch2/catch_test_macros.hpp>
#include <PoseidonVK/ShapeGeometryVK.hpp>

using namespace Poseidon;

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
