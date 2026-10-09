#pragma once
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/IO/FileServerMT.hpp>
#include <SDL3/SDL.h>

namespace Poseidon
{
// Real-content integration fixture. All model parsing, conversion, materials,
// section classification and mesh drawing use existing engine interfaces.
class VulkanModelSmoke
{
  public:
    explicit VulkanModelSmoke(Engine& engine) : _engine(engine), _previousScene(GScene), _previousFiles(GFileServer)
    {
        GUseFileBanks = true;
        GFileBanks.Load("dta\\", "dta\\", "data", true);
        GFileBanks.Load("dta\\", "dta\\", "data3d", true);
        GFileServer = new FileServerST(8 * 1024 * 1024);
        GFileServer->Start();
        _scene = std::make_unique<Scene>();
        GScene = _scene.get();
        Add("data3d\\dum_mesto.p3d", Vector3(2, 0, 5), 0.3f);
        Add("data3d\\jeep.p3d", Vector3(3, 0, -8), -0.65f);
        _camera.SetPosition(Vector3(19, 8, -25));
        _camera.SetOrient(Vector3(-17, -5, 30), VUp);
        LOG_INFO(Graphics, "Model smoke: stock building and Jeep; native Object materials and Shape::Draw. Arrow keys "
                           "move, PageUp/Down elevate, A/D turn.");
    }
    ~VulkanModelSmoke()
    {
        _objects.clear();
        _scene.reset();
        GScene = _previousScene;
        GFileServer->Stop();
        GFileServer = _previousFiles;
    }
    void Draw(float seconds)
    {
        const float dt = std::clamp(seconds - _lastTime, 0.f, 0.05f);
        _lastTime = seconds;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        Vector3 movement = _camera.Direction() * float(keys[SDL_SCANCODE_UP] - keys[SDL_SCANCODE_DOWN]) +
                           _camera.DirectionAside() * float(keys[SDL_SCANCODE_RIGHT] - keys[SDL_SCANCODE_LEFT]) +
                           VUp * float(keys[SDL_SCANCODE_PAGEUP] - keys[SDL_SCANCODE_PAGEDOWN]);
        _camera.SetPosition(_camera.Position() + movement * (dt * 10));
        const float yaw = float(keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]) * dt;
        if (yaw != 0)
            _camera.SetOrient(Matrix3(MRotationY, yaw) * _camera.Orientation());
        _camera.SetPerspective(0.1f, 500, float(_engine.Width()) / _engine.Height() * 0.55f, 0.55f);
        _camera.Adjust(&_engine);
        _scene->SetCamera(_camera);
        struct RestoreFilter
        {
            SectionClassFilter previous = GSectionFilter;
            ~RestoreFilter() { GSectionFilter = previous; }
        } restore;
        GSectionFilter = SectionClassFilter::OpaqueAndCutout;
        for (auto& object : _objects)
            DrawObject(*object);
        // Engine routing is per-section; translucent objects are back-to-front.
        std::vector<Object*> sorted;
        for (auto& object : _objects)
            sorted.push_back(object.get());
        std::sort(
            sorted.begin(), sorted.end(), [this](const Object* a, const Object* b)
            { return a->Position().Distance2(_camera.Position()) > b->Position().Distance2(_camera.Position()); });
        GSectionFilter = SectionClassFilter::BlendOnly;
        for (auto* object : sorted)
            DrawObject(*object);
    }

  private:
    void Add(const char* name, Vector3 position, float yaw)
    {
        auto model = _models.load(name);
        if (!model)
            throw std::runtime_error(std::string("Stock model load failed: ") + name);
        Ref<LODShapeWithShadow> lod = Model::ShapeAdapter::convertToLODShape(*model);
        if (!lod || !lod->NLevels() || !lod->Level(0))
            throw std::runtime_error("Stock model has no display LOD");
        Shape* shape = lod->Level(0);
        shape->ConvertToVBuffer(VBStatic);
        if (!shape->GetVertexBuffer())
            throw std::runtime_error("Stock model GPU upload failed");
        LOG_INFO(Graphics, "Stock model: {} format={} vertices={} sections={} boundsY={}..{}", name,
                 model->sourceFormat, shape->NVertex(), shape->NSections(), lod->Min().Y(), lod->Max().Y());
        for (int i = 0; i < shape->NSections(); ++i)
            LOG_INFO(Graphics, "Stock section {}: flags=0x{:08x}, material={}, texture={}", i,
                     unsigned(shape->GetSection(i).properties.Special()), shape->GetSection(i).material,
                     shape->GetSection(i).properties.GetTexture() ? shape->GetSection(i).properties.GetTexture()->Name()
                                                                  : "none");
        auto object = std::make_unique<Object>(lod, -1);
        Matrix4 transform(MRotationY, yaw);
        position[1] -= lod->Min().Y();
        transform.SetPosition(position);
        object->SetTransform(transform);
        _objects.push_back(std::move(object));
    }
    void DrawObject(Object& object)
    {
        object.GetShape()->Level(0)->Draw(&object, _lights, 0, 0, object.Transform(),
                                          object.Transform().InverseScaled());
    }
    Engine& _engine;
    Scene* _previousScene;
    Ref<FileServer> _previousFiles;
    std::unique_ptr<Scene> _scene;
    Camera _camera;
    LightList _lights;
    ModelCache _models;
    std::vector<std::unique_ptr<Object>> _objects;
    float _lastTime = 0;
};
} // namespace Poseidon
