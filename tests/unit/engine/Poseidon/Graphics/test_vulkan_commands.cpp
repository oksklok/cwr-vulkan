// Exercise the real DrawMesh binding path without an instance, device or driver.
#include <PoseidonVK/VulkanContext.hpp>
#include <stdexcept>
#include <type_traits>

namespace
{
std::array<unsigned, 8> calls{};
unsigned draws = 0;
template<class T> T Handle(uintptr_t value)
{
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) { ++calls[0]; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout,
    uint32_t first, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) { ++calls[first == 2 ? 2 : 1]; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*) { ++calls[3]; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType) { ++calls[4]; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*) { ++calls[5]; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*) { ++calls[6]; }
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*) { ++calls[7]; }
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { ++draws; }

namespace Poseidon::vk
{
struct VulkanCommandStateTest
{
    static int Run()
    {
        VulkanContext context;
        auto mesh = std::make_shared<MeshBuffers>();
        auto texture = std::make_shared<TextureImage>();
        // Clean fake handles even if an assertion throws; no driver destruction.
        struct Cleanup
        {
            VulkanContext& context;
            MeshBuffers& mesh;
            TextureImage& texture;
            ~Cleanup() { context._device = mesh.device = texture.device = VK_NULL_HANDLE; }
        } cleanup{context, *mesh, *texture};
        context._device = mesh->device = texture->device = Handle<VkDevice>(1);
        context._frameOpen = true;
        context._loggedShape = true;
        context._extent = {800, 600};
        context._whiteTexture = texture;
        context._shapeLayout = Handle<VkPipelineLayout>(2);
        context._shapePipelines.fill(Handle<VkPipeline>(3));
        context._screenPipelines.fill(Handle<VkPipeline>(4));
        context._shadowPipelines.fill(Handle<VkPipeline>(5));
        for (size_t i = 0; i < texture->descriptors.size(); ++i)
            texture->descriptors[i] = Handle<VkDescriptorSet>(10 + i);
        mesh->vertices.buffer = Handle<VkBuffer>(20);
        mesh->indices.buffer = Handle<VkBuffer>(21);
        mesh->vertices.size = mesh->indices.size = 4096;
        auto& frame = context._frames[0];
        frame.screenUniform.set = Handle<VkDescriptorSet>(30);
        frame.nativeUniform = frame.screenUniform;
        frame.nativeUniform.offset = 256;
        std::array<float, 16> matrix{};
        std::array<float, 4> color{1, 1, 1, 1};
        unsigned sampler = 0;
        VkDeviceSize vertexOffset = 0, indexOffset = 0;
        bool index16 = true, screen = true, shadow = false;
        VkRect2D clip{{0, 0}, {800, 600}};
        const ShapeLighting* lighting = nullptr;
        auto draw = [&] {
            context.DrawMesh(mesh, 0, 3, index16, matrix, color, texture, sampler, 0, false, screen, true,
                             &clip, {}, 1, {0, -1, 0}, vertexOffset, indexOffset, true, lighting, shadow);
        };
        int checks = 0;
        auto expect = [&](std::array<unsigned, 8> expected, const char* message) {
            ++checks;
            if (calls != expected) throw std::runtime_error(message);
            calls = {};
        };
        draws = 0;
        draw(); expect({1,1,1,1,1,1,1,1}, "first draw must bind every state, including all-zero matrix");
        draw(); expect({}, "identical draw must not repeat state commands");
        vertexOffset = 44; draw(); expect({0,0,0,1,0,0,0,0}, "vertex offset is binding state");
        indexOffset = 12; draw(); expect({0,0,0,0,1,0,0,0}, "index offset is binding state");
        index16 = false; draw(); expect({0,0,0,0,1,0,0,0}, "index type is binding state");
        sampler = 1; draw(); expect({0,1,0,0,0,0,0,0}, "sampler descriptor change must bind");
        texture->descriptors[1] = Handle<VkDescriptorSet>(40);
        draw(); expect({0,1,0,0,0,0,0,0}, "replacement image descriptor must bind");
        clip.offset.x = 2; draw(); expect({0,0,0,0,0,0,1,0}, "clip changes must bind");
        context._extent.width = 900; draw(); expect({0,0,0,0,0,1,0,0}, "viewport changes must bind");
        matrix[15] = 1; draw(); expect({0,0,0,0,0,0,0,1}, "matrix bytes must match");
        color[3] = 0.5f; draw(); expect({0,0,0,0,0,0,0,1}, "color bytes must match");
        context._gamma = 2; draw(); expect({0,0,0,0,0,0,0,1}, "gamma bytes must match");
        matrix[0] = -0.f; draw(); expect({0,0,0,0,0,0,0,1}, "push comparison must use complete bytes, not float equality");
        screen = false; lighting = &frame.nativeUniform.value;
        draw(); expect({1,0,1,0,0,0,0,0}, "native geometry must restore dynamic lighting offset");
        screen = true; lighting = nullptr;
        draw(); expect({1,0,1,0,0,0,0,0}, "screen geometry must restore unlit lighting offset");
        frame.screenUniform.set = Handle<VkDescriptorSet>(31);
        draw(); expect({0,0,1,0,0,0,0,0}, "lighting page descriptor is binding state");
        context._shadowPass = shadow = true;
        draw(); expect({1,0,0,0,0,0,0,1}, "shadow pipeline and constants must bind");
        context._shadowPass = shadow = false;
        draw(); expect({1,0,0,0,0,0,0,1}, "ordinary draw must restore shadow state");
        // Diagnostic's real path invalidates the cache before using its own layout.
        context._trianglePipeline = Handle<VkPipeline>(50);
        context._triangleVertices.buffer = Handle<VkBuffer>(51);
        context._triangleIndices.buffer = Handle<VkBuffer>(52);
        context._loggedTriangle = true;
        context.DrawDiagnosticTriangle();
        expect({1,0,0,1,1,1,1,1}, "diagnostic must record independent state");
        draw(); expect({1,1,1,1,1,1,1,1}, "production must restore all state after diagnostic layout");
        context._commands = {}; // Same invalidation used after command buffer reset.
        draw(); expect({1,1,1,1,1,1,1,1}, "new command buffer cannot inherit state");
        ++checks;
        if (draws != 21) throw std::runtime_error("state caching must never eliminate draws");
        return checks;
    }
};
}
int TestVulkanCommands() { return Poseidon::vk::VulkanCommandStateTest::Run(); }
