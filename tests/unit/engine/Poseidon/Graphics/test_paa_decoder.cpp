#include <catch2/catch_test_macros.hpp>
#include <BuildConfig.h>
#if CWR_HAS_VULKAN
#include <PoseidonVK/TextureVK.hpp>
#endif
#include <catch2/catch_approx.hpp>
#include <fstream>
#include <iterator>
#include <vector>
#include "test_fixtures.hpp"
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <cstdlib>
#include <stddef.h>
#include <string>
#include <vector>

using namespace Poseidon;

#if CWR_HAS_VULKAN
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

TEST_CASE("PAADecoder: stock bank mip chains agree with the existing top decoder", "[Graphics][PAADecoder][.stock-mips]")
{
    const char* root = std::getenv("CWR_STOCK_DATA");
    if (!root)
        SKIP("Set CWR_STOCK_DATA to the authorized stock game directory");
    struct Banks
    {
        bool previous = GUseFileBanks;
        ~Banks() { QIFStreamB::ClearBanks(); GUseFileBanks = previous; }
    } banks;
    GUseFileBanks = true;
    const std::string directory = std::string(root) + "/dta/";
    GFileBanks.Load(directory.c_str(), "", "data", true);
    GFileBanks.Load(directory.c_str(), "", "abel", true);
    for (const char* name : {"data\\domek1_front_okna.pac", "data\\domek2_side.paa", "data\\detail_dx.paa", "abel\\rwn.paa", "abel\\s3.paa"})
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
        for (size_t i = 1; i < chain.size(); ++i)
        {
            REQUIRE(chain[i].width == std::max(1, chain[i - 1].width / 2));
            REQUIRE(chain[i].height == std::max(1, chain[i - 1].height / 2));
            REQUIRE(chain[i].rgba.size() == size_t(chain[i].width * chain[i].height * 4));
        }
#if CWR_HAS_VULKAN
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
