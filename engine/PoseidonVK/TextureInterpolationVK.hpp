#pragma once
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <cstdint>

namespace Poseidon::vk
{
inline bool InterpolatesRGB555(PacFormat source)
{
    return source == PacARGB1555 || source == PacP8 || (source >= PacDXT1 && source <= PacDXT5);
}
// PacLevelMem::Interpolate's packed RGB555 arithmetic, used by GL33 sky
// uploads. Inputs are the decoded original texels; output is normalized RGBA8.
inline uint8_t InterpolateRGB555(uint8_t first, uint8_t second, int coefficient)
{
    const int value = ((first >> 3) * (255 - coefficient) + (second >> 3) * coefficient + 128) >> 8;
    return uint8_t((value * 255 + 15) / 31);
}
} // namespace Poseidon::vk
