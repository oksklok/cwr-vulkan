#include <PoseidonVK/EngineVK.hpp>
#include <PoseidonVK/ShapeTransformVK.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/World.hpp>
#include <algorithm>
#include <cmath>

namespace Poseidon
{
namespace
{
std::array<float, 4> Project(const std::array<float, 16>& m, Vector3Par p, const std::array<float, 2>& jitter)
{
    std::array<float, 4> c{};
    for (int k = 0; k < 4; ++k)
        c[k] = m[k] * p.X() + m[4 + k] * p.Y() + m[8 + k] * p.Z() + m[12 + k];
    c[0] += jitter[0] * c[3];
    c[1] += jitter[1] * c[3];
    return c;
}
std::vector<uint64_t> MeshKey(std::vector<uint64_t> path, const Shape& shape, bool software)
{
    path.push_back(shape.RenderIdentity());
    path.push_back(software);
    // Topology changes invalidate correspondence even when vertex counts agree.
    uint64_t topology = 1469598103934665603ull;
    for (Offset f = shape.BeginFaces(); software && f < shape.EndFaces(); shape.NextFace(f))
    {
        const auto& face = shape.Face(f);
        topology = (topology ^ uint64_t(face.N())) * 1099511628211ull;
        for (int i = 0; i < face.N(); ++i)
            topology = (topology ^ uint64_t(face.GetVertex(i))) * 1099511628211ull;
    }
    path.push_back(topology);
    return path;
}
} // namespace
void EngineVK::ResetTemporalHistory()
{
    _vk.ResetTemporalHistory();
    _temporalCameraValid = false;
    _previousMotion.clear();
    _currentMotion.clear();
}
void EngineVK::BeginTemporalWorld()
{
    _temporalWorldDrawn = true;
    if (!_vk.TemporalEnabled())
    {
        ResetTemporalHistory();
        return;
    }
    const auto* camera = GScene->GetCamera();
    const auto& projection = camera->ProjectionNormal();
    const int cameraType = GWorld ? int(GWorld->GetCameraType()) : -1;
    if (!_temporalCameraValid || cameraType != _temporalCameraType || _previousNightVision != _nightVision ||
        camera->Position().Distance2(_previousCameraPosition) > 100.f ||
        camera->Direction().DotProduct(_previousCameraDirection) < 0.7f ||
        std::abs(projection(0, 0) - _previousProjection(0, 0)) > 0.1f ||
        std::abs(projection(1, 1) - _previousProjection(1, 1)) > 0.1f)
        ResetTemporalHistory();
    _previousMotion.swap(_currentMotion);
    _currentMotion.clear();
    _temporalCameraType = cameraType;
    _previousNightVision = _nightVision;
}
void EngineVK::CaptureTemporalNative(const Shape& shape)
{
    _lighting.previousMVP = {};
    _lighting.temporal = {};
    if (_vk.TemporalEnabled() && _temporalTerrain && _vk.TemporalHistoryValid())
    {
        Matrix4 projection = _previousProjection;
        projection(0, 0) = projection(1, 1) = 1; // previous view already includes FOV
        Matrix4 relative = _shapeModelWorld;
        relative.SetPosition(relative.Position() - _previousCameraPosition);
        Matrix4 view = _previousWorldView;
        view.SetPosition(VZero);
        auto mvp = vk::ShapeMVP(view * relative, projection, _bias);
        for (int col = 0; col < 4; ++col)
        {
            mvp[col * 4] += _previousJitter[0] * mvp[col * 4 + 3];
            mvp[col * 4 + 1] += _previousJitter[1] * mvp[col * 4 + 3];
        }
        _lighting.previousMVP = mvp;
        _lighting.temporal[2] = 1;
        _lighting.temporal[3] = -1; // stationary terrain-fitted surface may not write depth
        return;
    }
    if (!_vk.TemporalEnabled() || _renderPath.empty() || (_shapeFlags & (IsShadow | NoDropdown)))
        return;
    auto key = MeshKey(_renderPath, shape, false);
    const auto previous = _previousMotion.find(key);
    if (_vk.TemporalHistoryValid() && previous != _previousMotion.end() && !previous->second.ambiguous)
    {
        _lighting.previousMVP = previous->second.mvp;
        _lighting.temporal[2] = 1;
    }
    auto mvp = _shapeMVP;
    const auto jitter = _vk.TemporalJitter();
    for (int col = 0; col < 4; ++col)
    {
        mvp[col * 4] += jitter[0] * mvp[col * 4 + 3];
        mvp[col * 4 + 1] += jitter[1] * mvp[col * 4 + 3];
    }
    const auto [entry, inserted] = _currentMotion.try_emplace(std::move(key));
    if (!inserted && entry->second.mvp != mvp)
        entry->second.ambiguous = true;
    if (entry->second.ambiguous)
        _lighting.temporal[2] = 0;
    entry->second.mvp = mvp;
}
void EngineVK::CaptureTemporalMesh(const Shape& shape, TLVertexTable& table, int spec)
{
    if (_temporalTerrain)
    {
        CaptureTemporalTerrain(table, false);
        return;
    }
    table.previousClip.clear();
    if (!_vk.TemporalEnabled() || _renderPath.empty() || (spec & (OnSurface | IsOnSurface | IsShadow | NoDropdown)))
        return;
    auto key = MeshKey(_renderPath, shape, true);
    const auto previous = _previousMotion.find(key);
    table.previousClip.resize(table.NVertex());
    if (_vk.TemporalHistoryValid() && previous != _previousMotion.end() && !previous->second.ambiguous &&
        previous->second.clip.size() == table.previousClip.size())
        table.previousClip = previous->second.clip;
    const auto mvp = vk::SoftwareProjection(GScene->GetCamera()->Projection(), _width, _height);
    MotionHistory current;
    current.clip.resize(table.NVertex());
    for (int i = 0; i < table.NVertex(); ++i)
        current.clip[i] = Project(mvp, table.TransPosA(i), _vk.TemporalJitter());
    const auto entry = _currentMotion.find(key);
    if (entry != _currentMotion.end() && (entry->second.ambiguous || entry->second.clip != current.clip))
    {
        current.ambiguous = true;
        std::fill(table.previousClip.begin(), table.previousClip.end(), std::array<float, 4>{});
    }
    _currentMotion[std::move(key)] = std::move(current);
}
void EngineVK::CaptureTemporalTerrain(TLVertexTable& table, bool water)
{
    table.previousClip.clear();
    if (!_vk.TemporalEnabled() || !_vk.TemporalHistoryValid() || water)
        return;
    // Terrain is explicitly stationary world geometry. Includes vertices generated
    // by CPU clipping; no transient segment identity or index matching is needed.
    const auto toPrevious = _previousWorldView * GScene->CamInvTrans();
    table.previousClip.resize(table.NVertex());
    for (int i = 0; i < table.NVertex(); ++i)
        table.previousClip[i] = Project(_previousSoftwareProjection,
                                        Vector3(VFastTransform, toPrevious, table.TransPosA(i)), _previousJitter);
}
} // namespace Poseidon
