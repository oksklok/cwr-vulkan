#pragma once
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <PoseidonVK/VulkanContext.hpp>
#include <map>
#include <stdexcept>

namespace Poseidon
{
class TextureVK final : public Texture
{
  public:
    explicit TextureVK(RStringB name);
    int AWidth(int level = 0) const override;
    int AHeight(int level = 0) const override;
    int ANMipmaps() const override { return 1; }
    int AMaxSize() const override { return std::max(_pixels.width, _pixels.height); }
    void SetMaxSize(int) override {} // Resolution hint; immutable path loads the original top mip.
    void ASetNMipmaps(int n) override;
    AbstractMipmapLevel& AMipmap(int level) override;
    const AbstractMipmapLevel& AMipmap(int level) const override;
    Color GetPixel(int level, float u, float v) const override;
    Color GetColor() override { return _average; }
    bool IsTransparent() const override { return _alpha != AlphaStats::Opaque; }
    bool IsAlpha() const override { return _alpha == AlphaStats::Blend; }
    AlphaStats::Kind GetAlphaClass() override { return _alpha; }
    bool VerifyChecksum(const MipInfo& mip) const override { return mip._texture == this && mip._level == 0; }
    std::shared_ptr<vk::TextureImage> Image(vk::VulkanContext& context);
    void ReleaseImage() { _image.reset(); }

  private:
    DecodedImage _pixels;
    PacLevelMem _level;
    AlphaStats::Kind _alpha = AlphaStats::Opaque;
    Color _average;
    std::shared_ptr<vk::TextureImage> _image;
};
class TextBankVK final : public AbstractTextBank
{
  public:
    explicit TextBankVK(vk::VulkanContext& context) : _context(context) {}
    ~TextBankVK() override;
    Ref<Texture> Load(RStringB name) override;
    Ref<Texture> LoadInterpolated(RStringB, RStringB, float) override
    {
        throw std::logic_error("Vulkan interpolated textures are unsupported");
    }
    int NTextures() const override { return static_cast<int>(_cache.size()); }
    Texture* GetTexture(int i) const override;
    MipInfo UseMipmap(Texture* texture, int, int) override;
    void Compact() override {} // Only explicitly loaded assets are retained; no eviction policy yet.
    void Preload() override;
    void FlushTextures() override;
    void ReleaseAllTextures() override { _cache.clear(); }
    void FlushBank(QFBank*) override { ReleaseAllTextures(); }

  private:
    vk::VulkanContext& _context;
    std::map<std::string, Ref<TextureVK>> _cache;
};
} // namespace Poseidon
