// Exercise the real DrawMesh binding path without an instance, device or driver.
#include <PoseidonVK/VulkanContext.hpp>
#include <stdexcept>
#include <type_traits>
#include <cstring>

namespace
{
std::array<unsigned, 8> calls{};
unsigned draws = 0;
unsigned cascadeBindings = 0;
unsigned finalDraws = 0, passBegins = 0, passEnds = 0;
bool finalTriangleValid = true;
float inverseGamma = 0;
VkRenderPassBeginInfo finalPass{};
VkViewport finalViewport{};
VkRect2D finalScissor{};
template<class T> T Handle(uintptr_t value)
{
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) { ++calls[0]; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout,
    uint32_t first, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*)
{
    if (first == 3) ++cascadeBindings;
    else ++calls[first == 2 ? 2 : 1];
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*) { ++calls[3]; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType) { ++calls[4]; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport* value) { ++calls[5]; finalViewport = *value; }
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D* value) { ++calls[6]; finalScissor = *value; }
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t size, const void* value) { ++calls[7]; if (size == sizeof(float)) std::memcpy(&inverseGamma, value, size); }
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { ++draws; }
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo* pass, VkSubpassContents) { ++passBegins; finalPass = *pass; }
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer) { ++passEnds; }
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t firstInstance)
{
    finalTriangleValid &= vertices == 3 && instances == 1 && first == 0 && firstInstance == 0;
    ++finalDraws;
}

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
        context._csmFallbackSet = Handle<VkDescriptorSet>(60);
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
        if (cascadeBindings != 1) throw std::runtime_error("CSM fallback must bind once across identical draws");
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
        context.SetGamma(2); draw(); expect({}, "framebuffer gamma must not alter ordinary draw constants");
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
        context._gammaTargets.resize(2);
        context._image = 1;
        context._gammaPass = Handle<VkRenderPass>(60);
        context._gammaPipeline = Handle<VkPipeline>(61);
        context._gammaLayout = Handle<VkPipelineLayout>(62);
        context._gammaTargets[1].set = Handle<VkDescriptorSet>(63);
        context._gammaTargets[1].framebuffer = Handle<VkFramebuffer>(64);
        for (const auto gamma : {1.f, 0.6f, 1.6f, 1.0005f})
        {
            context.SetGamma(gamma);
            context.DrawGammaPass();
            expect({1,1,0,0,0,1,1,1}, "final pass must bind independent fullscreen state without geometry uploads");
            ++checks;
            if (inverseGamma != (gamma == 1.0005f ? 1.f : 1.f / gamma))
                throw std::runtime_error("final pass must use the latest gamma and GL33 identity tolerance");
            ++checks;
            if (finalPass.framebuffer != context._gammaTargets[1].framebuffer || finalPass.renderPass != context._gammaPass ||
                finalPass.renderArea.extent.width != 900 || finalPass.renderArea.extent.height != 600 ||
                finalViewport.width != 900 || finalViewport.height != 600 ||
                finalScissor.offset.x != 0 || finalScissor.extent.width != 900 || finalScissor.extent.height != 600)
                throw std::runtime_error("final pass must select acquired image and overwrite full extent, ignoring scene clipping");
            draw(); expect({1,1,1,1,1,1,1,1}, "gamma layout must invalidate all cached scene state");
        }
        ++checks;
        if (!finalTriangleValid || finalDraws != 4 || passBegins != 4 || passEnds != 4)
            throw std::runtime_error("each final composition must have exactly one complete render pass");
        return checks;
    }
};
}
int TestVulkanCommands() { return Poseidon::vk::VulkanCommandStateTest::Run(); }
