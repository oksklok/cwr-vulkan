#pragma once
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <stdexcept>

namespace Poseidon
{
// Native section preparation also requests a mip for untextured geometry.
class TextBankVK final : public AbstractTextBank
{
  public:
    Ref<Texture> Load(RStringB) override { throw std::logic_error("Vulkan texture loading is unsupported"); }
    Ref<Texture> LoadInterpolated(RStringB, RStringB, float) override
    {
        throw std::logic_error("Vulkan interpolated textures are unsupported");
    }
    int NTextures() const override { return 0; }
    Texture* GetTexture(int) const override { throw std::out_of_range("Vulkan texture bank is empty"); }
    MipInfo UseMipmap(Texture* texture, int, int) override
    {
        if (texture)
            throw std::logic_error("Vulkan textured sections are unsupported");
        return MipInfo(nullptr, 0);
    }
    void Compact() override {}
    void Preload() override {}
    void FlushTextures() override {}
    void FlushBank(QFBank*) override {}
};
} // namespace Poseidon
