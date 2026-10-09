#include <PoseidonVK/TextureVK.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

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
    RefreshMetadata();
    LOG_INFO(Graphics, "Vulkan texture decoded: {} {}x{} {}", key, _pixels.width, _pixels.height,
             AlphaKindName(_alpha));
}
TextureVK::TextureVK(RStringB name, int width, int height, const void* rgba, uint32_t size) : _dynamic(true)
{
    if (width <= 0 || height <= 0 || size_t(width) * height * 4 != size || !rgba)
        throw std::invalid_argument("Vulkan dynamic texture requires complete RGBA pixels");
    SetName(name);
    _pixels.width = width;
    _pixels.height = height;
    _pixels.rgba.resize(size);
    UpdateRGBA(rgba, size);
}
void TextureVK::UpdateRGBA(const void* rgba, uint32_t size)
{
    if (!_dynamic || !rgba || size != _pixels.rgba.size())
        throw std::invalid_argument("Vulkan texture update requires matching dynamic RGBA pixels");
    std::memcpy(_pixels.rgba.data(), rgba, size);
    RefreshMetadata();
    // Old images remain in every referencing frame until its fence completes.
    _image.reset();
}
void TextureVK::RefreshMetadata()
{
    _alpha = ClassifyAlpha(_pixels.rgba.data(), size_t(_pixels.width) * _pixels.height).kind;
    _level._w = _pixels.width;
    _level._h = _pixels.height;
    _level.SetDestFormat(PacARGB8888, 1);
    double sum[4]{};
    for (size_t i = 0; i < _pixels.rgba.size(); ++i)
        sum[i % 4] += _pixels.rgba[i];
    const double scale = 4.0 / (255.0 * _pixels.rgba.size());
    _average = Color(sum[0] * scale, sum[1] * scale, sum[2] * scale, sum[3] * scale);
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
    if (!QIFStreamB::FileExist(key.c_str()))
    {
        // Stock configs contain dangling icon/texture references. Match the
        // engine/GL33 nullable-load contract, not a fabricated successful image.
        LOG_WARN(Graphics, "Vulkan: Cannot load texture {}", key);
        return nullptr;
    }
    Ref<TextureVK> texture = new TextureVK(key.c_str());
    _cache.emplace(key, texture);
    return texture.GetRef();
}
Ref<Texture> TextBankVK::LoadInterpolated(RStringB first, RStringB second, float factor)
{
    if (!std::isfinite(factor))
        throw std::invalid_argument("Vulkan interpolated texture factor must be finite");
    if (factor <= 1.f / 256)
        return Load(first);
    if (factor >= 1 - 1.f / 256)
        return Load(second);
    const auto key = "interpolate:" + TextureKey(first) + ":" + TextureKey(second);
    auto found = _cache.find(key);
    if (found != _cache.end() && std::abs(_interpolationFactors.at(key) - factor) <= 1.f / 64)
        return found->second.GetRef();
    Ref<Texture> a = Load(first), b = Load(second);
    if (!a || !b)
        throw std::invalid_argument("Vulkan interpolation requires two textures");
    const auto& p = static_cast<TextureVK*>(a.GetRef())->Pixels();
    const auto& q = static_cast<TextureVK*>(b.GetRef())->Pixels();
    std::vector<uint8_t> rgba(p.rgba.size());
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            for (int c = 0; c < 4; ++c)
            {
                const size_t i = (size_t(y) * p.width + x) * 4 + c;
                const size_t j = (size_t(y * q.height / p.height) * q.width + x * q.width / p.width) * 4 + c;
                rgba[i] = uint8_t(std::lround(p.rgba[i] * (1 - factor) + q.rgba[j] * factor));
            }
    if (found == _cache.end())
        found = _cache.emplace(key, new TextureVK(key.c_str(), p.width, p.height, rgba.data(), rgba.size())).first;
    else
        found->second->UpdateRGBA(rgba.data(), rgba.size());
    _interpolationFactors[key] = factor;
    return found->second.GetRef();
}
Texture* TextBankVK::CreateDynamic(int width, int height, const void* rgba, uint32_t size, bool mipmap)
{
    if (mipmap)
        throw std::logic_error("Vulkan dynamic texture mip generation is unsupported");
    const auto key = "dynamic:" + std::to_string(++_dynamicSerial);
    Ref<TextureVK> texture = new TextureVK(key.c_str(), width, height, rgba, size);
    _cache.emplace(key, texture);
    return texture.GetRef();
}
void TextBankVK::UpdateDynamic(Texture* texture, const void* rgba, uint32_t size)
{
    auto* vkTexture = dynamic_cast<TextureVK*>(texture);
    if (!vkTexture || !vkTexture->IsDynamic())
        throw std::invalid_argument("Vulkan dynamic update requires a dynamic Vulkan texture");
    vkTexture->UpdateRGBA(rgba, size);
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
