#include <PoseidonVK/TextureVK.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace Poseidon
{
namespace
{
void RequireTop(int level)
{
    if (level != 0)
        throw std::out_of_range("Vulkan textures expose only the original top mip");
}
std::string TextureKey(const char* name)
{
    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return c == '/' ? '\\' : std::tolower(c); });
    return key;
}
} // namespace
TextureVK::TextureVK(RStringB name)
{
    SetName(name);
    QIFStreamB file;
    file.AutoOpen(name);
    const auto key = TextureKey(name);
    if (file.fail() || file.rest() == 0)
        throw std::runtime_error("Vulkan texture: cannot open " + key);
    _pixels = DecodePAABuffer(file.act(), file.rest(), key.ends_with(".paa"));
    if (!_pixels.valid())
        throw std::runtime_error("Vulkan texture: existing PAA/PAC decoder failed for " + key);
    _alpha = ClassifyAlpha(_pixels.rgba.data(), size_t(_pixels.width) * _pixels.height).kind;
    _level._w = _pixels.width;
    _level._h = _pixels.height;
    _level.SetDestFormat(PacARGB8888, 1);
    double sum[4]{};
    for (size_t i = 0; i < _pixels.rgba.size(); ++i)
        sum[i % 4] += _pixels.rgba[i];
    const double scale = 4.0 / (255.0 * _pixels.rgba.size());
    _average = Color(sum[0] * scale, sum[1] * scale, sum[2] * scale, sum[3] * scale);
    LOG_INFO(Graphics, "Vulkan texture decoded: {} {}x{} {}", key, _pixels.width, _pixels.height,
             AlphaKindName(_alpha));
}
int TextureVK::AWidth(int level) const
{
    RequireTop(level);
    return _pixels.width;
}
int TextureVK::AHeight(int level) const
{
    RequireTop(level);
    return _pixels.height;
}
void TextureVK::ASetNMipmaps(int n)
{
    if (n != 1)
        throw std::logic_error("Vulkan texture mip-chain mutation is unsupported");
}
AbstractMipmapLevel& TextureVK::AMipmap(int level)
{
    RequireTop(level);
    return _level;
}
const AbstractMipmapLevel& TextureVK::AMipmap(int level) const
{
    RequireTop(level);
    return _level;
}
Color TextureVK::GetPixel(int level, float u, float v) const
{
    RequireTop(level);
    const int x = int((u - std::floor(u)) * _pixels.width);
    const int y = int((v - std::floor(v)) * _pixels.height);
    const auto* pixel = _pixels.rgba.data() + (y * _pixels.width + x) * 4;
    return Color(pixel[0] / 255.f, pixel[1] / 255.f, pixel[2] / 255.f, pixel[3] / 255.f);
}
std::shared_ptr<vk::TextureImage> TextureVK::Image(vk::VulkanContext& context)
{
    if (!_image)
    {
        _image = context.UploadTexture(_pixels.width, _pixels.height, _pixels.rgba.data());
        LOG_INFO(Graphics, "Vulkan texture uploaded once: {}", Name());
    }
    return _image;
}
TextBankVK::~TextBankVK()
{
    UnlockAllTextures();
    DeleteAllAnimated();
    _cache.clear();
}
Ref<Texture> TextBankVK::Load(RStringB name)
{
    if (name.GetLength() == 0)
        return nullptr;
    auto key = TextureKey(name);
    auto found = _cache.find(key);
    if (found != _cache.end())
        return found->second.GetRef();
    Ref<TextureVK> texture = new TextureVK(key.c_str());
    _cache.emplace(key, texture);
    return texture.GetRef();
}
Texture* TextBankVK::GetTexture(int i) const
{
    if (i < 0 || i >= NTextures())
        throw std::out_of_range("Vulkan texture cache index");
    auto entry = _cache.begin();
    std::advance(entry, i);
    return entry->second.GetRef();
}
MipInfo TextBankVK::UseMipmap(Texture* texture, int, int)
{
    if (texture)
    {
        auto* vkTexture = dynamic_cast<TextureVK*>(texture);
        if (!vkTexture)
            throw std::logic_error("Vulkan bank received a foreign texture");
        vkTexture->Image(_context);
    }
    return MipInfo(texture, 0);
}
void TextBankVK::Preload()
{
    for (auto& [key, texture] : _cache)
        texture->Image(_context);
}
void TextBankVK::FlushTextures()
{
    for (auto& [key, texture] : _cache)
        texture->ReleaseImage();
}
} // namespace Poseidon
