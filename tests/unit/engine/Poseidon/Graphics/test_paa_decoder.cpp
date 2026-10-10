#include <catch2/catch_test_macros.hpp>
#include <BuildConfig.h>
#if CWR_HAS_VULKAN
#include <PoseidonVK/TextureVK.hpp>
#endif
#include <catch2/catch_approx.hpp>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iterator>
#include <memory>
#include <vector>
#include "test_fixtures.hpp"
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <array>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <cstdlib>
#include <stddef.h>
#include <string>
#include <vector>

using namespace Poseidon;

TEST_CASE("PAADecoder: interpolation-only DXT1 decode matches legacy blocks", "[Graphics][PAADecoder]")
{
    // Four-color, three-color/transparent and equal-endpoint blocks, all selectors.
    for (auto endpoints : {std::array<uint16_t, 2>{0x1000, 0}, {0x003f, 0xf800},
                           {0x1234, 0x1234}, {0xffff, 0x0801}})
        for (int width : {2, 8})
        {
            const std::array<uint16_t, 4> block{endpoints[0], endpoints[1], 0xe4e4, 0xe4e4};
            uint16_t reference[16];
            PacLevelMem::DecompressDXT1(reference, block.data(), 4, 4);
            std::vector<uint8_t> bytes{1, 255, 0, 0, uint8_t(width), 0, 2, 0,
                                       uint8_t(((width + 3) / 4) * 8), 0, 0};
            for (int x = 0; x < width; x += 4)
                for (uint16_t word : block)
                {
                    bytes.push_back(uint8_t(word));
                    bytes.push_back(uint8_t(word >> 8));
                }
            const auto normal = DecodePAABuffer(bytes.data(), bytes.size(), true);
            const auto image = DecodePAAInterpolationBuffer(bytes.data(), bytes.size(), true);
            REQUIRE(image.valid());
            REQUIRE(image.width == width);
            REQUIRE(image.height == 2);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const uint16_t pixel = reference[y * 4 + x % 4];
                    for (int c = 0; c < 3; ++c)
                    {
                        const int value = (pixel >> (10 - c * 5)) & 31;
                        REQUIRE(image.rgba[(y * width + x) * 4 + c] == ((value << 3) | (value >> 2)));
                    }
                    REQUIRE(image.rgba[(y * width + x) * 4 + 3] == ((pixel & 0x8000) ? 255 : 0));
                }
            REQUIRE(DecodePAABuffer(bytes.data(), bytes.size(), true).rgba == normal.rgba);
            bytes.pop_back();
            REQUIRE_FALSE(DecodePAAInterpolationBuffer(bytes.data(), bytes.size(), true).valid());
        }
}

TEST_CASE("PAADecoder: AI88 preserves all intensity and alpha bits in every stored mip", "[Graphics][PAADecoder]")
{
    std::vector<uint8_t> bytes{0x80, 0x80, 0, 0};
    auto append = [&](uint32_t value, int count)
    {
        for (int i = 0; i < count; ++i)
            bytes.push_back(uint8_t(value >> (8 * i)));
    };
    std::vector<std::vector<uint8_t>> expected;
    for (int size : {16, 8, 4, 2})
    {
        append(size, 2);
        append(size, 2);
        append(size * size * 2 + size * size / 4 + 4, 3);
        std::vector<uint8_t> rgba;
        int checksum = 0;
        for (int i = 0; i < size * size; ++i)
        {
            // Literal-only engine LZ stream: eight bytes per flag byte.
            if (i % 4 == 0)
                bytes.push_back(255);
            const uint8_t intensity = uint8_t(i + 3), alpha = uint8_t(255 - i);
            bytes.push_back(intensity);
            bytes.push_back(alpha);
            checksum += int8_t(intensity) + int8_t(alpha);
            rgba.insert(rgba.end(), {intensity, intensity, intensity, alpha});
        }
        append(uint32_t(checksum), 4);
        expected.push_back(std::move(rgba));
    }
    append(0, 4);
    const auto chain = DecodePAAMipChainBuffer(bytes.data(), bytes.size(), true);
    REQUIRE(chain.size() == expected.size());
    for (size_t i = 0; i < chain.size(); ++i)
        REQUIRE(chain[i].rgba == expected[i]);
    REQUIRE(DecodePAABuffer(bytes.data(), bytes.size(), true).rgba == expected.front());
    REQUIRE(ClassifyAlpha(chain[0].rgba.data(), 256).kind == AlphaStats::Blend);

    struct TemporaryFile
    {
        std::filesystem::path path;
        ~TemporaryFile() { std::error_code ec; std::filesystem::remove(path, ec); }
    } file{std::filesystem::temp_directory_path() /
           ("cwr-ai88-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".paa")};
    {
        std::ofstream out(file.path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        REQUIRE(out.good());
    }
    for (size_t i = 0; i < expected.size(); ++i)
        REQUIRE(DecodePAAFileMip(file.path.string(), int(i)).rgba == expected[i]);
}

#if CWR_HAS_VULKAN
TEST_CASE("Vulkan DXT1 sky interpolation preserves legacy source quantization", "[Graphics][PAADecoder]")
{
    struct TemporarySky
    {
        std::filesystem::path path;
        ~TemporarySky() { std::error_code ec; std::filesystem::remove(path, ec); }
    } first{std::filesystem::temp_directory_path() /
            ("cwr-sky-first-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".paa")},
      second{first.path.string() + "-second.paa"};
    // One 4x4 DXT1 block, endpoints red=2 and black, selecting color 2.
    // Modern RGBA8 decode gives red 11; GL33's legacy 1555 decode gives 2/31.
    std::vector<uint8_t> bytes{1, 255, 0, 0, 4, 0, 4, 0, 8, 0, 0,
                               0, 16, 0, 0, 170, 170, 170, 170, 0, 0, 0, 0};
    auto write = [&](const std::filesystem::path& path)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        REQUIRE(out.good());
    };
    write(first.path);
    bytes[12] = 0;
    write(second.path);
    vk::VulkanContext context; // CPU texture path only; no Vulkan device needed.
    TextBankVK bank(context);
    const auto a = bank.Load(first.path.string().c_str());
    REQUIRE(static_cast<TextureVK*>(a.GetRef())->Pixels().rgba[0] == 11);
    const auto result = bank.LoadInterpolated(first.path.string().c_str(), second.path.string().c_str(), 0.5f);
    REQUIRE(static_cast<TextureVK*>(result.GetRef())->Pixels().rgba[0] == 8);
    // CPU sky/fog lookup and ordinary sampling must retain their original colors.
    REQUIRE(result->GetPixel(0, 0, 0).R() == Catch::Approx(5.5f / 255));
    REQUIRE(static_cast<TextureVK*>(a.GetRef())->Pixels().rgba[0] == 11);
    REQUIRE(bank.LoadInterpolated(first.path.string().c_str(), second.path.string().c_str(), 0.5f) == result);
}

TEST_CASE("Vulkan CPU pixel lookup clamps like PacLevelMem including the fog horizon sample", "[Graphics][PAADecoder]")
{
    const uint8_t rgba[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 128, 64, 32, 255};
    TextureVK texture("clamped-pixels", 2, 2, rgba, sizeof(rgba));
    REQUIRE(texture.GetPixel(0, 1, 1).R() == Catch::Approx(128.f / 255));
    REQUIRE(texture.GetPixel(0, 1, 1).B() == Catch::Approx(32.f / 255));
    REQUIRE(texture.GetPixel(0, 2, 2).G() == Catch::Approx(64.f / 255));
    REQUIRE(texture.GetPixel(0, -0.5f, -0.5f).R() == 1);
    REQUIRE(texture.GetPixel(0, 0.5f, 0).G() == 1);
}
#endif

TEST_CASE("PAADecoder: original memory mip chain preserves sizes pixels alpha and shorter chains", "[Graphics][PAADecoder]")
{
    // Raw ARGB8888 uses the same PacLevelMem header walk as compressed stock assets.
    std::vector<uint8_t> bytes{0x88, 0x88, 0, 0};
    auto append = [&](unsigned value, int count)
    {
        for (int i = 0; i < count; ++i)
            bytes.push_back(uint8_t(value >> (8 * i)));
    };
    for (int size : {8, 4, 2})
    {
        append(size, 2);
        append(size, 2);
        append(size * size * 4, 3);
        for (int i = 0; i < size * size; ++i)
        {
            bytes.push_back(3);
            bytes.push_back(2);
            bytes.push_back(uint8_t(size));
            bytes.push_back(uint8_t(size * 10));
        }
    }
    append(0, 4);
    const auto levels = DecodePAAMipChainBuffer(bytes.data(), bytes.size(), true);
    REQUIRE(levels.size() == 3);
    for (size_t i = 0; i < levels.size(); ++i)
    {
        const auto& image = levels[i];
        REQUIRE(image.width == (8 >> i));
        REQUIRE(image.height == image.width);
        REQUIRE(image.rgba.size() == size_t(image.width * image.height * 4));
        REQUIRE(image.rgba[0] == image.width);
        REQUIRE(image.rgba[1] == 2);
        REQUIRE(image.rgba[2] == 3);
        REQUIRE(image.rgba[3] == image.width * 10);
    }
    REQUIRE(DecodePAABuffer(bytes.data(), bytes.size(), true).rgba == levels[0].rgba);
    // Stop after a valid 8x8 top level: no fabricated levels needed to reach 1x1.
    bytes.resize(4 + 7 + 8 * 8 * 4);
    append(0, 4);
    REQUIRE(DecodePAAMipChainBuffer(bytes.data(), bytes.size(), true).size() == 1);
    REQUIRE(DecodePAAMipChainBuffer(nullptr, 0, true).empty());
    bytes.resize(bytes.size() - 10);
    REQUIRE(DecodePAAMipChainBuffer(bytes.data(), bytes.size(), true).empty());
}

TEST_CASE("PAADecoder: stored one-pixel tails retain supported fixture levels", "[Graphics][PAADecoder]")
{
    for (const char* name : {"texture/paa/synthetic_dxt1.paa", "paa/synthetic_dxt5.paa", "texture/pac/synthetic_default.pac"})
    {
        INFO(name);
        std::ifstream file(GET_FIXTURE(name), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
        const bool paa = std::string(name).ends_with(".paa");
        const auto top = DecodePAABuffer(bytes.data(), bytes.size(), paa);
        const auto levels = DecodePAAMipChainBuffer(bytes.data(), bytes.size(), paa);
        REQUIRE(top.valid());
        REQUIRE(levels.size() == (top.width == 64 ? 6 : 5));
        REQUIRE(levels.front().rgba == top.rgba);
        REQUIRE(levels.back().width == 2);
        REQUIRE(levels.back().height == 2);
    }
    // Valid raw 2x2 + 1x1 tail, then a truncated payload and malformed dimensions.
    std::vector<uint8_t> bytes{0x88, 0x88, 0, 0, 2, 0, 2, 0, 16, 0, 0};
    bytes.resize(27, 255);
    bytes.insert(bytes.end(), {1, 0, 1, 0, 4, 0, 0, 255, 255, 255, 255});
    REQUIRE(DecodePAAMipChainBuffer(bytes.data(), bytes.size(), true).size() == 1);
    REQUIRE(DecodePAAMipChainBuffer(bytes.data(), bytes.size() - 1, true).empty());
    bytes[29] = 2;
    REQUIRE(DecodePAAMipChainBuffer(bytes.data(), bytes.size(), true).empty());
}

TEST_CASE("PAADecoder: stock bank mip chains agree with the existing top decoder", "[Graphics][PAADecoder][.stock-mips]")
{
    const char* root = std::getenv("CWR_STOCK_DATA");
    if (!root)
        SKIP("Set CWR_STOCK_DATA to the authorized stock game directory");
    struct Banks
    {
        bool previous = GUseFileBanks;
        Ref<FileServer> previousServer = GFileServer;
        ~Banks() { GFileServer = previousServer; QIFStreamB::ClearBanks(); GUseFileBanks = previous; }
    } banks;
    class BankFileServer final : public FileServer
    {
      public:
        void Open(QIFStream& stream, const char* name) override
        {
            QIFStreamB bank;
            bank.AutoOpen(name);
            stream = bank;
        }
        void Request(const char*, float, int, int) override {}
        void CancelRequest(const char*, int, int) override {}
        void Start() override {}
        void Stop() override {}
        void FlushBank(QFBank*) override {}
    };
    GFileServer = new BankFileServer;
    GUseFileBanks = true;
    const std::string directory = std::string(root) + "/dta/";
    GFileBanks.Load(directory.c_str(), "", "data", true);
    GFileBanks.Load(directory.c_str(), "", "abel", true);
    for (const char* name : {"data\\domek1_front_okna.pac", "data\\domek2_side.paa", "data\\detail_dx.paa", "abel\\rwn.paa", "abel\\s3.paa",
                             "data\\more_anim.03.pac", "data\\specular_dx.paa", "data\\silnice.paa",
                             "data\\mrak_war_1.paa", "data\\mrak_war_3.paa", "data\\mrak_war_4.paa", "data\\mrak_war_5.paa",
                             "data\\jablon_renovace.pac", "data\\jablon.pac", "data\\n_strom_13.pac", "data\\krovi6.pac"})
    {
        INFO(name);
        QIFStreamB source;
        source.AutoOpen(name);
        REQUIRE_FALSE(source.fail());
        REQUIRE(source.rest() > 0);
        const bool paa = std::string(name).ends_with(".paa");
        const auto chain = DecodePAAMipChainBuffer(source.act(), source.rest(), paa);
        REQUIRE(chain.size() > 1);
        const auto top = DecodePAABuffer(source.act(), source.rest(), paa);
        REQUIRE(chain[0].rgba == top.rgba);
        const std::string textureName(name);
        const bool cloud = textureName.find("mrak_war_") != std::string::npos;
        const bool foliage = textureName.find("jablon") != std::string::npos ||
                             textureName == "data\\n_strom_13.pac" || textureName == "data\\krovi6.pac";
        if (cloud || foliage)
        {
            // Residency is not decoding: keep the authored base and lower mips,
            // with translucent clouds distinct from binary-alpha foliage.
            REQUIRE(top.width == 256);
            REQUIRE(chain.size() >= 3);
            REQUIRE(chain[2].width == 64);
            REQUIRE(ClassifyAlpha(top.rgba.data(), size_t(top.width * top.height)).kind ==
                    (cloud ? AlphaStats::Blend : AlphaStats::Cutout));
        }
        if (uint8_t(source.act()[0]) == 0x80 && uint8_t(source.act()[1]) == 0x80)
            REQUIRE(ClassifyAlpha(top.rgba.data(), size_t(top.width * top.height)).kind == AlphaStats::Blend);
        for (size_t i = 1; i < chain.size(); ++i)
        {
            REQUIRE(chain[i].width == std::max(1, chain[i - 1].width / 2));
            REQUIRE(chain[i].height == std::max(1, chain[i - 1].height / 2));
            REQUIRE(chain[i].rgba.size() == size_t(chain[i].width * chain[i].height * 4));
        }
        if (foliage)
        {
            // Compare every stored foliage alpha mask with the legacy source
            // loader. RGB is deliberately excluded: GL33 uploads these DXT1
            // textures compressed, not through the RGB555 conversion below.
            PacLevelMem legacyMips[16];
            auto* factory = SelectTextureSourceFactory(name);
            REQUIRE(factory != nullptr);
            std::unique_ptr<ITextureSource> legacy(factory->Create(name, legacyMips, 16));
            REQUIRE(legacy != nullptr);
            REQUIRE(legacy->GetFormat() == PacDXT1);
            REQUIRE(legacy->GetMipmapCount() > 0);
            REQUIRE(legacy->GetMipmapCount() <= chain.size());
            for (int i = 0; i < legacy->GetMipmapCount(); ++i)
            {
                auto& mip = legacyMips[i];
                INFO("mip " << i);
                REQUIRE(mip._w == chain[i].width);
                REQUIRE(mip._h == chain[i].height);
                mip.SetDestFormat(PacARGB1555, 8);
                std::vector<uint8_t> packed(mip.Size());
                REQUIRE(legacy->GetMipmapData(packed.data(), mip, int(i)));
                size_t differentAlpha = 0;
                for (int y = 0; y < mip._h; ++y)
                    for (int x = 0; x < mip._w; ++x)
                    {
                        const auto offset = y * mip.Pitch() + x * 2;
                        const uint16_t pixel = packed[offset] | (uint16_t(packed[offset + 1]) << 8);
                        const auto out = (y * mip._w + x) * 4;
                        if (chain[i].rgba[out + 3] != ((pixel & 0x8000) ? 255 : 0))
                            ++differentAlpha;
                    }
                REQUIRE(differentAlpha == 0);
            }
        }
#if CWR_HAS_VULKAN
        std::vector<vk::TextureMip> sampled;
        for (const auto& mip : chain)
            sampled.push_back({uint32_t(mip.width), uint32_t(mip.height), mip.rgba.data()});
        const auto count = vk::TextureSampledMipCount(sampled);
        REQUIRE(count > 0);
        REQUIRE(count <= chain.size());
        if (std::string(name) == "data\\detail_dx.paa") REQUIRE(count == 5);
        if (std::string(name) == "data\\more_anim.03.pac") REQUIRE(count == 7);
        if (std::string(name) == "data\\specular_dx.paa") REQUIRE(count == 6);
        if (std::string(name) == "data\\silnice.paa") REQUIRE(count == 4);
        // Weather restricts a stored sky chain to its top level. Exercise the
        // same contract without a GPU; invalid growth must not invent mips.
        TextureVK texture(name);
        REQUIRE(texture.ANMipmaps() == chain.size());
        REQUIRE_NOTHROW(texture.ASetNMipmaps(2));
        REQUIRE(texture.AHeight(1) == chain[1].height);
        REQUIRE_NOTHROW(texture.ASetNMipmaps(1));
        REQUIRE(texture.ANMipmaps() == 1);
        REQUIRE(texture.Pixels().rgba == top.rgba);
        REQUIRE_NOTHROW(texture.ASetNMipmaps(1));
        REQUIRE_THROWS_AS(texture.ASetNMipmaps(0), std::out_of_range);
        REQUIRE_THROWS_AS(texture.ASetNMipmaps(2), std::out_of_range);
#endif
    }
}

// Reproducers the fuzz_paa libFuzzer harness found. Pre-fix
// each was a heap-buffer-overflow in DecodePAABuffer; the dimension/payload guards
// (PAADecoder) and the LZW match bound (Pactext) now reject them.
TEST_CASE("PAADecoder: fuzz reproducers decode without out-of-bounds access", "[Graphics][PAADecoder][fuzz]")
{
    auto readAll = [](const std::string& p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };

    SECTION("truncated DXT payload yields an empty image, not an OOB read")
    {
        std::vector<char> bytes = readAll(GET_FIXTURE("texture/paa/fuzz_dxt_oob.paa"));
        REQUIRE(!bytes.empty());
        DecodedImage img = DecodePAABuffer(bytes.data(), bytes.size(), true);
        // Pre-fix the DXT1 decoder read blocks past the short payload into a
        // full-size rgba; the size guard now returns an empty image instead.
        REQUIRE(img.rgba.empty());
    }

    SECTION("over-long LZW match is bounded, not written past the buffer")
    {
        std::vector<char> bytes = readAll(GET_FIXTURE("texture/paa/fuzz_lzw_overflow.paa"));
        REQUIRE(!bytes.empty());
        DecodePAABuffer(bytes.data(), bytes.size(), true); // must not overflow the output
        SUCCEED("LZW reproducer decoded without an out-of-bounds write");
    }

    SECTION("ARGB8888 payload shorter than width*height*4 yields an empty image, not an OOB read")
    {
        std::vector<char> bytes = readAll(GET_FIXTURE("texture/paa/fuzz_argb8888_oob.paa"));
        REQUIRE(!bytes.empty());
        DecodedImage img = DecodePAABuffer(bytes.data(), bytes.size(), true);
        // Pre-fix the ARGB->RGBA conversion loop read width*height*4 bytes from a
        // rawData sized to the (smaller) declared payload; the size guard now returns
        // an empty image instead of reading past rawData.
        REQUIRE(img.rgba.empty());
    }
}

// --- ReadPAAInfo tests ---

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_ai88.paa header", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/synthetic_ai88.paa"), info));
    REQUIRE(info.isPaa);
    REQUIRE(info.magic == 0x8080);
    REQUIRE(info.width == 64);
    REQUIRE(info.height == 64);
    REQUIRE(std::string(info.formatName) == "AI88");
}

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_argb4444.paa header", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/synthetic_argb4444.paa"), info));
    REQUIRE(info.isPaa);
    REQUIRE(info.magic == 0x4444);
    REQUIRE(info.width == 32);
    REQUIRE(info.height == 32);
    REQUIRE(std::string(info.formatName) == "ARGB4444");
}

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_dxt1.paa (DXT1 64x64)", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"), info));
    REQUIRE(info.isPaa);
    REQUIRE(info.magic == 0xFF01);
    REQUIRE(info.width == 64);
    REQUIRE(info.height == 64);
    REQUIRE(std::string(info.formatName) == "DXT1");
}

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_dxt5.paa (DXT5 32x32)", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("paa/synthetic_dxt5.paa"), info));
    REQUIRE(info.isPaa);
    REQUIRE(info.magic == 0xFF05);
    REQUIRE(info.width == 32);
    REQUIRE(info.height == 32);
    REQUIRE(std::string(info.formatName) == "DXT5");
}

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_default.pac (DXT5 64x64)", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/pac/synthetic_default.pac"), info));
    REQUIRE_FALSE(info.isPaa);
    REQUIRE(info.magic == 0xFF05);
    REQUIRE(info.width == 64);
    REQUIRE(info.height == 64);
    REQUIRE(std::string(info.formatName) == "DXT5");
}

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_dark.pac (DXT5 64x64)", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/pac/synthetic_dark.pac"), info));
    REQUIRE_FALSE(info.isPaa);
    REQUIRE(info.magic == 0xFF05);
    REQUIRE(info.width == 64);
    REQUIRE(info.height == 64);
}

TEST_CASE("PAADecoder: ReadPAAInfo returns false for nonexistent file", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE_FALSE(ReadPAAInfo("nonexistent_file.paa", info));
}

// --- DecodePAAFile tests ---

TEST_CASE("PAADecoder: Decode synthetic_dxt5.paa produces 32x32 RGBA", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("texture/paa/synthetic_dxt5.paa"));
    REQUIRE(img.valid());
    REQUIRE(img.width == 32);
    REQUIRE(img.height == 32);
    REQUIRE(img.rgba.size() == 32 * 32 * 4);
}

TEST_CASE("PAADecoder: Decode synthetic_dxt1.paa (DXT1) produces 64x64 RGBA", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(img.valid());
    REQUIRE(img.width == 64);
    REQUIRE(img.height == 64);
    REQUIRE(img.rgba.size() == 64 * 64 * 4);
}

TEST_CASE("PAADecoder: Decode synthetic_dxt5.paa (DXT5) produces RGBA", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("paa/synthetic_dxt5.paa"));
    REQUIRE(img.valid());
    REQUIRE(img.width == 32);
    REQUIRE(img.height == 32);
    REQUIRE(img.rgba.size() == 32 * 32 * 4);
}

TEST_CASE("PAADecoder: Decode synthetic_default.pac (DXT5) produces 64x64 RGBA", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("texture/pac/synthetic_default.pac"));
    REQUIRE(img.valid());
    REQUIRE(img.width == 64);
    REQUIRE(img.height == 64);
    REQUIRE(img.rgba.size() == 64 * 64 * 4);
}

TEST_CASE("PAADecoder: Decode synthetic_dark.pac (DXT5) produces 64x64 RGBA", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("texture/pac/synthetic_dark.pac"));
    REQUIRE(img.valid());
    REQUIRE(img.width == 64);
    REQUIRE(img.height == 64);
    REQUIRE(img.rgba.size() == 64 * 64 * 4);
}

TEST_CASE("PAADecoder: Decode nonexistent file returns invalid", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile("nonexistent_file.paa");
    REQUIRE_FALSE(img.valid());
}

// --- Pixel spot-checks ---

TEST_CASE("PAADecoder: synthetic DXT1 pixels decode", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(img.valid());
    REQUIRE_FALSE(img.rgba.empty());
}

TEST_CASE("PAADecoder: DecodePAABuffer works with file data", "[Graphics][PAADecoder]")
{
    std::string path = GET_FIXTURE("texture/paa/synthetic_dxt1.paa");
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    size_t size = f.tellg();
    f.seekg(0);
    std::vector<char> buf(size);
    f.read(buf.data(), size);

    auto img = DecodePAABuffer(buf.data(), size, true);
    REQUIRE(img.valid());
    REQUIRE(img.width == 64);
    REQUIRE(img.height == 64);
}
