#pragma once
#include <vulkan/vulkan.h>
#include <string_view>
#include <cstdint>
#include <initializer_list>

namespace Poseidon::vk
{
enum class AAMode
{
    Off,
    FXAA,
    SMAA,
    MSAA2,
    MSAA4,
    MSAA8
};
inline const char* AAName(AAMode mode)
{
    switch (mode)
    {
        case AAMode::FXAA:
            return "fxaa";
        case AAMode::SMAA:
            return "smaa";
        case AAMode::MSAA2:
            return "msaa2";
        case AAMode::MSAA4:
            return "msaa4";
        case AAMode::MSAA8:
            return "msaa8";
        default:
            return "off";
    }
}
inline bool ParseAA(std::string_view name, AAMode& result)
{
    for (auto mode : {AAMode::Off, AAMode::FXAA, AAMode::SMAA, AAMode::MSAA2, AAMode::MSAA4, AAMode::MSAA8})
        if (name == AAName(mode))
        {
            result = mode;
            return true;
        }
    return false;
}
inline VkSampleCountFlagBits AASamples(AAMode mode)
{
    return mode == AAMode::MSAA2   ? VK_SAMPLE_COUNT_2_BIT
           : mode == AAMode::MSAA4 ? VK_SAMPLE_COUNT_4_BIT
           : mode == AAMode::MSAA8 ? VK_SAMPLE_COUNT_8_BIT
                                   : VK_SAMPLE_COUNT_1_BIT;
}
inline bool ValidRenderScale(int percent)
{
    return percent == 100 || percent == 125 || percent == 150 || percent == 200;
}
inline VkExtent2D ScaledExtent(VkExtent2D size, int percent)
{
    return {uint32_t((uint64_t(size.width) * percent + 99) / 100),
            uint32_t((uint64_t(size.height) * percent + 99) / 100)};
}
} // namespace Poseidon::vk
