#include <PoseidonVK/EngineVK.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>

namespace Poseidon
{
namespace
{
Engine* CreateVK(const GraphicsEngineParams& params)
{
    try
    {
        return new EngineVK(params);
    }
    catch (const std::exception& error)
    {
        LOG_ERROR(Graphics, "Vulkan backend creation failed: {}", error.what());
        return nullptr;
    }
}
} // namespace

void RegisterVKGraphicsBackend()
{
    // Deliberately opt-in only: below Dummy's priority so Auto cannot select an
    // unfinished game renderer when GL33 initialization fails. Explicit vk still works.
    GraphicsEngineFactory::Register({"vk", "Vulkan (SDL3, experimental)", -100, &CreateVK, nullptr});
}
} // namespace Poseidon
