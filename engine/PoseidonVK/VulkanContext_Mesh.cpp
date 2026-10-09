#include <PoseidonVK/VulkanContext.hpp>
#include <algorithm>
#include <stdexcept>

namespace Poseidon::vk
{
MeshBuffers::~MeshBuffers()
{
    if (device)
        vkDeviceWaitIdle(device);
    Destroy();
}
void MeshBuffers::Destroy() noexcept
{
    if (device)
    {
        DestroyBuffer(device, indices);
        DestroyBuffer(device, vertices);
    }
    device = VK_NULL_HANDLE;
}

std::shared_ptr<MeshBuffers> VulkanContext::UploadMesh(const void* vertices, size_t vertexBytes, const void* indices,
                                                       size_t indexBytes)
{
    if (!_device || !vertices || !indices || !vertexBytes || !indexBytes)
        throw std::invalid_argument("Vulkan Shape: upload needs a live device and nonempty geometry");
    auto mesh = std::make_shared<MeshBuffers>();
    mesh->device = _device;
    const auto vertexResult =
        CreateHostVisibleBuffer(_physical, _device, vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mesh->vertices);
    if (vertexResult != VK_SUCCESS)
        throw std::runtime_error("Vulkan Shape: vertex buffer allocation failed (" + std::to_string(vertexResult) +
                                 ")");
    const auto indexResult =
        CreateHostVisibleBuffer(_physical, _device, indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh->indices);
    if (indexResult != VK_SUCCESS)
        throw std::runtime_error("Vulkan Shape: index buffer allocation failed (" + std::to_string(indexResult) + ")");
    UploadMappedBuffer(mesh->vertices, vertices, vertexBytes);
    UploadMappedBuffer(mesh->indices, indices, indexBytes);
    std::erase_if(_meshes, [](const auto& entry) { return entry.expired(); });
    _meshes.push_back(mesh);
    return mesh;
}
} // namespace Poseidon::vk
