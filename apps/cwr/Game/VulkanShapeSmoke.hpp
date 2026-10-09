#pragma once

#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

namespace Poseidon
{
// Integration fixture only: all geometry is an ordinary engine Shape; upload,
// camera transformation, section selection and drawing use production virtuals.
class VulkanShapeSmoke
{
  public:
    explicit VulkanShapeSmoke(Engine& engine, bool textured = false)
        : _engine(engine), _previousScene(GScene), _textured(textured)
    {
        const Vector3 points[] = {{-0.9f, -0.65f, -0.55f}, {0.9f, -0.65f, -0.55f}, {0.9f, 0.65f, -0.55f},
                                  {-0.9f, 0.65f, -0.55f},  {-0.9f, -0.65f, 0.55f}, {0.9f, -0.65f, 0.55f},
                                  {0.9f, 0.65f, 0.55f},    {-0.9f, 0.65f, 0.55f}};
        if (textured)
        {
            GUseFileBanks = true;
            GFileBanks.Load("dta\\", "dta\\", "data", true);
            _textures[0] = _engine.TextBank()->Load("data\\domek1_front_okna.pac");
            _textures[1] = _engine.TextBank()->Load("data\\domek2_side.paa");
            if (_engine.TextBank()->Load("DATA/domek2_side.paa").GetRef() != _textures[1].GetRef() ||
                _engine.TextBank()->NTextures() != 2)
                throw std::runtime_error("Vulkan texture smoke: canonical-name cache failed");
        }
        const int faces[][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 3, 7, 4}, {1, 5, 6, 2}, {3, 2, 6, 7}, {0, 4, 5, 1}};
        ShapeSection sections[6];
        for (int i = 0; i < 6; ++i)
        {
            Poly face;
            face.Init();
            face.SetN(4);
            const float uv[][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
            for (int v = 0; v < 4; ++v)
            {
                face.Set(v, i * 4 + v);
                _shape.AddVertexFast(points[faces[i][v]], VUp, 0, uv[v][0], uv[v][1]);
            }
            sections[i].properties.Init();
            sections[i].material = 0;
            sections[i].material = i < 2 ? 0 : i - 1;
            if (textured)
            {
                sections[i].material = 0;
                sections[i].properties.SetTexture(_textures[(i / 2) % 2]);
            }
            sections[i].beg = _shape.EndFaces();
            _shape.AddFace(face);
            sections[i].end = _shape.EndFaces();
        }
        _shape.Faces().SetSections(sections, 6);
        _shape.ConvertToVBuffer(VBStatic); // Invokes Engine::CreateVertexBuffer and retains engine ownership.
        if (!_shape.GetVertexBuffer())
            throw std::runtime_error("Vulkan Shape smoke: engine vertex buffer creation failed");
        _camera.SetPosition(Vector3(3, 2, -4));
        _camera.SetOrient(Vector3(0.15f, 0.08f, 1), VUp);
        _scene = std::make_unique<Scene>();
        GScene = _scene.get();
        LOG_INFO(Graphics, "Shape smoke: 24 engine vertices, 6 quad sections, 36 fan indices; two native Shape draws, "
                           "non-origin camera");
    }

    ~VulkanShapeSmoke()
    {
        // Scene and its redirected profile are cleaned up while GEngine is alive.
        _scene.reset();
        GScene = _previousScene;
    }

    void Draw(float seconds)
    {
        if (!_shape.GetVertexBuffer())
            _shape.ConvertToVBuffer(VBStatic);
        const float aspect = float(_engine.Width()) / float(_engine.Height());
        _camera.SetPerspective(0.1f, 100, aspect * 0.55f, 0.55f);
        _camera.Adjust(&_engine);
        _scene->SetCamera(_camera);
        // Front object submitted first, farther object second: visibility relies
        // on the depth buffer, rather than painter ordering.
        Matrix4 front(MRotationY, 0.45f + seconds * 0.25f);
        front = front * Matrix4(MRotationX, -0.22f);
        front.SetPosition(Vector3(2.65f, 2.3f, 2.5f));
        DrawObject(front, false);
        Matrix4 back(MRotationY, -0.5f - seconds * 0.16f);
        back.SetPosition(Vector3(3.65f, 1.85f, 4.5f));
        DrawObject(back, true);
        // Exercise Shape ownership while this frame is still being recorded.
        // The production context must retain both buffers through submission.
        if (!_releasedDuringFrame && seconds > 1)
        {
            _shape.ReleaseVBuffer();
            _releasedDuringFrame = true;
            LOG_INFO(Graphics, "Shape smoke: released vertex-buffer owner before submission; re-upload next frame");
        }
    }

  private:
    void DrawObject(const Matrix4& model, bool back)
    {
        class StaticMaterials final : public IAnimator
        {
          public:
            bool textured = false;
            void DoTransform(TLVertexTable&, const Shape&, const Matrix4&, int, int) const override
            {
                throw std::logic_error("Shape smoke unexpectedly used software transformation");
            }
            void DoLight(TLVertexTable&, const Shape&, const Matrix4&, const LightList&, int, int, int,
                         int) const override
            {
                throw std::logic_error("Shape smoke unexpectedly used software lighting");
            }
            bool GetAnimated(const Shape&) const override { return false; }
            void GetMaterial(TLMaterial& mat, int index) const override
            {
                const Color colors[] = {Color(0.12f, 0.75f, 0.9f, 1), Color(0.22f, 0.35f, 0.9f, 1),
                                        Color(1, 0.35f, 0.12f, 1), Color(1, 0.8f, 0.18f, 1),
                                        Color(0.2f, 0.8f, 0.35f, 1)};
                CreateMaterialNormal(mat, textured ? Color(1, 1, 1, 1) : colors[index]);
            }
        } materials;
        materials.textured = _textured;
        _scene->SetConstantColor(!_textured && back ? Color(0.7f, 0.7f, 0.7f, 1) : Color(1, 1, 1, 1));
        _shape.Draw(&materials, _lights, 0, IsColored, model, model.InverseScaled());
    }

    Engine& _engine;
    Shape _shape;
    Camera _camera;
    LightList _lights;
    Scene* _previousScene;
    std::unique_ptr<Scene> _scene;
    bool _releasedDuringFrame = false;
    bool _textured = false;
    Ref<Texture> _textures[2];
};
} // namespace Poseidon
