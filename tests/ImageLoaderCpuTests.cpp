#include "renderer/ImageLoader.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

int gFailureCount = 0;

template <typename T, typename U>
void expectEqual(std::string_view context, const T& actual, const U& expected) {
    if (actual != expected) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}

template <typename F>
void expectNoThrow(std::string_view context, F&& callable) {
    try {
        callable();
    } catch (const std::exception& e) {
        std::cerr << "[FAILED] " << context << ": unexpected exception " << e.what() << '\n';
        ++gFailureCount;
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": unexpected non-std exception\n";
        ++gFailureCount;
    }
}

template <typename F>
void expectThrowsRuntimeError(std::string_view context, F&& callable) {
    try {
        callable();
        std::cerr << "[FAILED] " << context << ": expected runtime_error but no exception thrown\n";
        ++gFailureCount;
    } catch (const std::runtime_error&) {
        // expected
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": expected runtime_error but threw different exception\n";
        ++gFailureCount;
    }
}

void expectNearly(std::string_view context, const float actual, const float expected, const float epsilon = 1.0e-5f) {
    if (std::fabs(actual - expected) > epsilon) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << " (eps=" << epsilon << ")\n";
        ++gFailureCount;
    }
}

template <typename T, typename U>
void expectGreater(std::string_view context, const T& actual, const U& minimum) {
    if (!(actual > minimum)) {
        std::cerr << "[FAILED] " << context << ": expected value greater than " << minimum << ", got " << actual << '\n';
        ++gFailureCount;
    }
}

template <typename T, typename U>
void expectAbsLess(std::string_view context, const T& actual, const U& limit) {
    if (std::fabs(static_cast<float>(actual)) > static_cast<float>(limit)) {
        std::cerr << "[FAILED] " << context << ": expected |value| <= " << limit << ", got " << static_cast<float>(actual) << '\n';
        ++gFailureCount;
    }
}

struct DecodedNormal {
    float x = 0.0f;
    float y = 0.0f;
    float z = 1.0f;
};

[[nodiscard]] float decodeNormalChannel(const float encoded) {
    return encoded / 127.5f - 1.0f;
}

[[nodiscard]] float normalLength(const DecodedNormal& normal) {
    return std::sqrt((normal.x * normal.x) + (normal.y * normal.y) + (normal.z * normal.z));
}

[[nodiscard]] DecodedNormal decodeNormal(const float encodedX, const float encodedY, const float encodedZ) {
    return {
        decodeNormalChannel(encodedX),
        decodeNormalChannel(encodedY),
        decodeNormalChannel(encodedZ),
    };
}

[[nodiscard]] DecodedNormal decodeNormalFromImagePixel(const ve::LoadedImageRgba8& image, const std::size_t index) {
    const std::size_t offset = index * 4U;
    return decodeNormal(static_cast<float>(image.pixels.at(offset + 0U)),
                       static_cast<float>(image.pixels.at(offset + 1U)),
                       static_cast<float>(image.pixels.at(offset + 2U)));
}

void expectEqualBytes(std::string_view context, const std::vector<std::uint8_t>& actual, const std::vector<std::uint8_t>& expected) {
    if (actual.size() != expected.size()) {
        std::cerr << "[FAILED] " << context << ": expected " << expected.size() << " bytes, got " << actual.size() << '\n';
        ++gFailureCount;
        return;
    }

    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] != expected[index]) {
            std::cerr << "[FAILED] " << context << ": byte[" << index << "] expected " << static_cast<int>(expected[index])
                      << " but got " << static_cast<int>(actual[index]) << '\n';
            ++gFailureCount;
            return;
        }
    }
}

std::filesystem::path writePpmFixture(std::string_view name, std::string_view header, const std::vector<std::uint8_t>& pixelBytes) {
    const std::filesystem::path filePath = std::filesystem::temp_directory_path() / std::string(name);
    std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("Failed to create fixture " + filePath.string());
    }

    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!pixelBytes.empty()) {
        file.write(reinterpret_cast<const char*>(pixelBytes.data()), static_cast<std::streamsize>(pixelBytes.size()));
    }
    if (!file) {
        throw std::runtime_error("Failed to write fixture " + filePath.string());
    }
    return filePath;
}





std::filesystem::path writeBinaryFixture(std::string_view name, const std::vector<std::uint8_t>& bytes) {
    const std::filesystem::path filePath = std::filesystem::temp_directory_path() / std::string(name);
    std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("Failed to create binary fixture " + filePath.string());
    }

    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!file) {
        throw std::runtime_error("Failed to write binary fixture " + filePath.string());
    }
    return filePath;
}
void removeIfExists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace


int main() {
    std::vector<std::filesystem::path> fixtures;
    auto addFixture = [&](std::string_view name, std::string_view header, const std::vector<std::uint8_t>& pixelBytes) {
        const auto path = writePpmFixture(name, header, pixelBytes);
        fixtures.push_back(path);
        return path;
    };
    auto addBinaryFixture = [&](std::string_view name, const std::vector<std::uint8_t>& bytes) {
        const auto path = writeBinaryFixture(name, bytes);
        fixtures.push_back(path);
        return path;
    };


    const auto valid8BitPath = addFixture("image_loader_valid_8bit.ppm",
                                          "P6\n"
                                          "# comment in header\n"
                                          "\t 2 1\n"
                                          "# extra comment\n"
                                          "255\n",
                                          std::vector<std::uint8_t>{
                                              0xFFU, 0x00U, 0x00U,
                                              0x00U, 0x80U, 0xFFU});
    expectNoThrow("loadPpmRgba8 accepts P6 with comments and whitespace", [&] {
        const auto image = ve::loadPpmRgba8(valid8BitPath);
        expectEqual("commented 8-bit width", image.width, 2U);
        expectEqual("commented 8-bit height", image.height, 1U);
        expectEqualBytes("commented 8-bit alpha set to 255", image.pixels,
                         std::vector<std::uint8_t>{
                             255U, 0U, 0U, 255U,
                             0U, 128U, 255U, 255U});
    });
    const auto crlfPath = addFixture("image_loader_crlf.ppm",
                                     "P6\r\n"
                                     "1 1\r\n"
                                     "255\r\n",
                                     std::vector<std::uint8_t>{0x0AU, 0xBBU, 0xCCU});
    expectNoThrow("loadPpmRgba8 preserves the first pixel after a CRLF header", [&] {
        const auto image = ve::loadPpmRgba8(crlfPath);
        expectEqual("CRLF PPM width", image.width, 1U);
        expectEqual("CRLF PPM height", image.height, 1U);
        expectEqualBytes("CRLF PPM exact RGBA output", image.pixels,
                         std::vector<std::uint8_t>{0x0AU, 0xBBU, 0xCCU, 0xFFU});
    });

    const auto valid16BitPath = addFixture("image_loader_valid_16bit.ppm",
                                          "P6\n"
                                          "1 2\n"
                                          "65535\n",
                                          std::vector<std::uint8_t>{
                                              0x00U, 0x00U, // pixel 0 R -> 0
                                              0x01U, 0x01U, // pixel 0 G -> 1
                                              0xFFU, 0xFFU, // pixel 0 B -> 255
                                              0xFFU, 0xFEU, // pixel 1 R -> 255
                                              0xFFU, 0xFFU, // pixel 1 G -> 255
                                              0x00U, 0x00U  // pixel 1 B -> 0
                                          });
    expectNoThrow("loadPpmRgba8 accepts 16-bit and scales to u8", [&] {
        const auto image = ve::loadPpmRgba8(valid16BitPath);
        expectEqual("16-bit width", image.width, 1U);
        expectEqual("16-bit height", image.height, 2U);
        expectEqualBytes("16-bit output scaled to RGBA8", image.pixels,
                         std::vector<std::uint8_t>{
                             0U, 1U, 255U, 255U,
                             255U, 255U, 0U, 255U});
    });



    const auto tinyPngPath = addBinaryFixture("image_loader_tiny_rgba.png",
                                             std::vector<std::uint8_t>{
                                                 0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
                                                 0x00U, 0x00U, 0x00U, 0x0DU, 0x49U, 0x48U, 0x44U, 0x52U,
                                                 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
                                                 0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x1FU, 0x15U, 0xC4U,
                                                 0x89U, 0x00U, 0x00U, 0x00U, 0x0DU, 0x49U, 0x44U, 0x41U,
                                                 0x54U, 0x78U, 0xDAU, 0x63U, 0x10U, 0x50U, 0x30U, 0x68U,
                                                 0x00U, 0x00U, 0x01U, 0x85U, 0x00U, 0xE1U, 0x3DU, 0x9AU,
                                                 0x31U, 0x26U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U,
                                                 0x4EU, 0x44U, 0xAEU, 0x42U, 0x60U, 0x82U});
    expectNoThrow("loadImageRgba8 decodes tiny PNG with exact RGBA output", [&] {
        const auto image = ve::loadImageRgba8(tinyPngPath);
        expectEqual("tiny PNG width", image.width, 1U);
        expectEqual("tiny PNG height", image.height, 1U);
        expectEqualBytes("tiny PNG pixel data decodes to RGBA",
                         image.pixels,
                         std::vector<std::uint8_t>{16U, 32U, 48U, 128U});
    });

    expectNoThrow("loadImageRgba8 dispatches .ppm input to loadPpmRgba8 behavior", [&] {
        const auto ppmExpected = ve::loadPpmRgba8(valid8BitPath);
        const auto image = ve::loadImageRgba8(valid8BitPath);
        expectEqual("delegated PPM width", image.width, ppmExpected.width);
        expectEqual("delegated PPM height", image.height, ppmExpected.height);
        expectEqualBytes("delegated PPM pixels", image.pixels, ppmExpected.pixels);
    });

    const auto unsupportedNonPpmPath = addBinaryFixture("image_loader_corrupt_non_ppm.png",
                                                       std::vector<std::uint8_t>{'n', 'o', 't', ' ', 'a', 'n', ' ',
                                                                                 'i', 'm', 'a', 'g', 'e',
                                                                                 ' ', 'f', 'i', 'l', 'e', '!'});
    expectThrowsRuntimeError("loadImageRgba8 rejects corrupt non-PPM data", [&] {
        (void)ve::loadImageRgba8(unsupportedNonPpmPath);
    });
    const auto emptyImagePath = addBinaryFixture("image_loader_empty.png", {});
    expectThrowsRuntimeError("loadImageRgba8 rejects empty image files", [&] {
        (void)ve::loadImageRgba8(emptyImagePath);
    });

    const auto unsupportedMagicPath = addFixture("image_loader_unsupported_magic.ppm", "P3\n1 1\n255\n", {});
    expectThrowsRuntimeError("loadPpmRgba8 rejects unsupported magic", [&] {
        (void)ve::loadPpmRgba8(unsupportedMagicPath);
    });

    const auto earlyEofPath = addFixture("image_loader_early_eof.ppm",
                                        "P6\n1 1\n255\n",
                                        std::vector<std::uint8_t>{0xAAU});
    expectThrowsRuntimeError("loadPpmRgba8 rejects early EOF", [&] {
        (void)ve::loadPpmRgba8(earlyEofPath);
    });

    const auto tooLargeMaxPath = addFixture("image_loader_too_large_max.ppm", "P6\n1 1\n70000\n", {});
    const auto malformedDimensionTokenPath = addFixture("image_loader_malformed_dimension_token.ppm",
                                                     "P6\n"
                                                     "abc 1\n"
                                                     "255\n",
                                                     {});
    expectThrowsRuntimeError("loadPpmRgba8 rejects malformed width token", [&] {
        (void)ve::loadPpmRgba8(malformedDimensionTokenPath);
    });

    const auto oversizedDimensionPath = addFixture("image_loader_oversized_dimension.ppm",
                                                  "P6\n"
                                                  "4294967296 1\n"
                                                  "255\n",
                                                  {});
    expectThrowsRuntimeError("loadPpmRgba8 rejects out-of-range width dimension", [&] {
        (void)ve::loadPpmRgba8(oversizedDimensionPath);
    });

    expectThrowsRuntimeError("loadPpmRgba8 rejects max > 65535", [&] {
        (void)ve::loadPpmRgba8(tooLargeMaxPath);
    });

    const auto sampleAboveMaxPath = addFixture("image_loader_sample_above_max.ppm",
                                              "P6\n1 1\n1\n",
                                              std::vector<std::uint8_t>{0x02U, 0x00U, 0x00U});
    expectThrowsRuntimeError("loadPpmRgba8 rejects sample greater than max", [&] {
        (void)ve::loadPpmRgba8(sampleAboveMaxPath);
    });


    {
        const ve::LoadedImageRgba8 baseLevel{
            2U,
            2U,
            std::vector<std::uint8_t>{
                10U, 20U, 30U, 0U,
                30U, 40U, 50U, 64U,
                50U, 60U, 70U, 128U,
                70U, 80U, 90U, 255U}};
        std::vector<ve::LoadedImageRgba8> mipChain;
        expectNoThrow("buildAlbedoMipChainRgba8 alpha-weights non-sRGB RGB and averages alpha", [&] {
            mipChain = ve::buildAlbedoMipChainRgba8(baseLevel, false);
        });
        expectEqual("linear albedo chain has base and one mip level", mipChain.size(), static_cast<std::size_t>(2));
        if (mipChain.size() == 2U) {
            expectEqual("linear albedo mip width", mipChain[1U].width, 1U);
            expectEqual("linear albedo mip height", mipChain[1U].height, 1U);
            expectEqualBytes("linear albedo mip alpha-weights RGB and averages alpha", mipChain[1U].pixels,
                             std::vector<std::uint8_t>{59U, 69U, 79U, 112U});
        }
    }

    {
        const ve::LoadedImageRgba8 baseLevel{
            2U,
            2U,
            std::vector<std::uint8_t>{
                0U, 0U, 0U, 0U,
                255U, 255U, 255U, 64U,
                0U, 0U, 0U, 128U,
                255U, 255U, 255U, 255U}};
        std::vector<ve::LoadedImageRgba8> mipChain;
        expectNoThrow("buildAlbedoMipChainRgba8 alpha-weights sRGB RGB in linear space", [&] {
            mipChain = ve::buildAlbedoMipChainRgba8(baseLevel, true);
        });
        expectEqual("sRGB albedo chain has base and one mip level", mipChain.size(), static_cast<std::size_t>(2));
        if (mipChain.size() == 2U) {
            expectEqualBytes("sRGB albedo mip alpha-weights RGB in linear space and alpha linearly", mipChain[1U].pixels,
                             std::vector<std::uint8_t>{220U, 220U, 220U, 112U});
        }
    }

    {
        const ve::LoadedImageRgba8 baseLevel{
            2U,
            2U,
            std::vector<std::uint8_t>{
                255U, 0U, 0U, 255U,
                0U, 0U, 255U, 0U,
                0U, 0U, 255U, 0U,
                0U, 0U, 255U, 0U}};
        std::vector<ve::LoadedImageRgba8> mipChain;
        expectNoThrow("buildAlbedoMipChainRgba8 prevents transparent color bleed", [&] {
            mipChain = ve::buildAlbedoMipChainRgba8(baseLevel, false);
        });
        expectEqual("transparent bleed albedo chain has base and one mip level", mipChain.size(), static_cast<std::size_t>(2));
        if (mipChain.size() == 2U) {
            expectEqualBytes("transparent blue texels do not darken opaque red mip", mipChain[1U].pixels,
                             std::vector<std::uint8_t>{255U, 0U, 0U, 64U});
        }
    }

    {
        constexpr std::uint32_t npotWidth = 3U;
        constexpr std::uint32_t npotHeight = 5U;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(npotWidth) * npotHeight * 4U, 200U);
        const ve::LoadedImageRgba8 baseLevel{npotWidth, npotHeight, std::move(pixels)};
        std::vector<ve::LoadedImageRgba8> mipChain;
        expectNoThrow("buildAlbedoMipChainRgba8 handles NPOT dimensions", [&] {
            mipChain = ve::buildAlbedoMipChainRgba8(baseLevel, true);
        });
        expectEqual("NPOT albedo chain has three levels", mipChain.size(), static_cast<std::size_t>(3));
        if (mipChain.size() == 3U) {
            expectEqual("NPOT albedo base width", mipChain[0U].width, npotWidth);
            expectEqual("NPOT albedo base height", mipChain[0U].height, npotHeight);
            expectEqual("NPOT albedo mip one width", mipChain[1U].width, 1U);
            expectEqual("NPOT albedo mip one height", mipChain[1U].height, 2U);
            expectEqual("NPOT albedo final mip width", mipChain[2U].width, 1U);
            expectEqual("NPOT albedo final mip height", mipChain[2U].height, 1U);
            expectEqual("NPOT albedo final mip alpha", mipChain[2U].pixels.at(3U), 200U);
        }
    }

    expectThrowsRuntimeError("buildAlbedoMipChainRgba8 rejects zero-dimension width", [] {
        const ve::LoadedImageRgba8 invalidWidth{0U, 2U, std::vector<std::uint8_t>(8U, 255U)};
        (void)ve::buildAlbedoMipChainRgba8(invalidWidth, true);
    });
    expectThrowsRuntimeError("buildAlbedoMipChainRgba8 rejects wrong-size payload", [] {
        const ve::LoadedImageRgba8 badPayload{2U, 2U, std::vector<std::uint8_t>{255U, 0U, 255U, 255U}};
        (void)ve::buildAlbedoMipChainRgba8(badPayload, true);
    });

    {
        const ve::LoadedImageRgba8 baseLevel{
            2U,
            2U,
            std::vector<std::uint8_t>{
                10U, 20U, 30U, 0U,
                110U, 120U, 130U, 64U,
                210U, 220U, 230U, 128U,
                250U, 240U, 230U, 255U}};
        std::vector<ve::LoadedImageRgba8> mipChain;
        expectNoThrow("buildLinearMipChainRgba8 keeps scalar RGB independent from alpha", [&] {
            mipChain = ve::buildLinearMipChainRgba8(baseLevel);
        });
        expectEqual("linear scalar chain has base and one mip level", mipChain.size(), static_cast<std::size_t>(2));
        if (mipChain.size() == 2U) {
            expectEqual("linear scalar mip width", mipChain[1U].width, 1U);
            expectEqual("linear scalar mip height", mipChain[1U].height, 1U);
            expectEqual("linear scalar mip red straight average", mipChain[1U].pixels.at(0U), 145U);
            expectEqual("linear scalar mip green straight average", mipChain[1U].pixels.at(1U), 150U);
            expectEqual("linear scalar mip blue straight average", mipChain[1U].pixels.at(2U), 155U);
            expectEqual("linear scalar mip alpha straight average", mipChain[1U].pixels.at(3U), 112U);
        }
    }

    {
        const ve::LoadedImageRgba8 baseLevel{
            2U,
            2U,
            std::vector<std::uint8_t>{
                128U, 128U, 255U, 255U,
                128U, 128U, 255U, 255U,
                128U, 128U, 255U, 255U,
                128U, 128U, 255U, 255U}};
        std::vector<ve::LoadedImageRgba8> mipChain;
        expectNoThrow("buildNormalMapMipChainRgba8 keeps flat +Z stable for 2x2 input", [&] {
            mipChain = ve::buildNormalMapMipChainRgba8(baseLevel);
        });
        expectEqual("flat +Z flat chain has base and one mip level", mipChain.size(), static_cast<std::size_t>(2));

        if (mipChain.size() == 2U) {
            expectEqual("flat +Z base level width", mipChain[0U].width, 2U);
            expectEqual("flat +Z base level height", mipChain[0U].height, 2U);
            expectEqual("flat +Z mip level width", mipChain[1U].width, 1U);
            expectEqual("flat +Z mip level height", mipChain[1U].height, 1U);

            const auto normal = decodeNormalFromImagePixel(mipChain[1U], 0U);
            expectNearly("flat +Z mip 1x1 decoded normal length", normalLength(normal), 1.0f, 1.0e-4f);
            expectGreater("flat +Z mip 1x1 decoded normal has positive z", normal.z, 0.99f);
            expectAbsLess("flat +Z mip 1x1 decoded normal x is near 0", normal.x, 0.02f);
            expectAbsLess("flat +Z mip 1x1 decoded normal y is near 0", normal.y, 0.02f);
        }
    }

    {
        const ve::LoadedImageRgba8 baseLevel{
            2U,
            2U,
            std::vector<std::uint8_t>{
                255U, 0U, 255U, 255U, // +X, +Z
                0U, 255U, 0U, 255U, // -X, -Z
                0U, 255U, 255U, 255U, // +Y, +Z
                255U, 0U, 0U, 255U // -Y, -Z
            }};
        std::vector<ve::LoadedImageRgba8> mipChain;
        expectNoThrow("buildNormalMapMipChainRgba8 renormalizes opposing XY mip as vector sum", [&] {
            mipChain = ve::buildNormalMapMipChainRgba8(baseLevel);
        });
        expectEqual("opposing XY chain has base and one mip level", mipChain.size(), static_cast<std::size_t>(2));

        if (mipChain.size() == 2U) {
            const auto normal = decodeNormalFromImagePixel(mipChain[1U], 0U);
            expectNearly("opposing XY mip decoded normal length", normalLength(normal), 1.0f, 1.0e-4f);
            expectGreater("opposing XY mip fallback decoded normal has positive z", normal.z, 0.99f);
            expectAbsLess("opposing XY fallback decoded normal x is near 0", normal.x, 0.02f);
            expectAbsLess("opposing XY fallback decoded normal y is near 0", normal.y, 0.02f);
        }
    }

    {
        const std::uint32_t npotWidth = 3U;
        const std::uint32_t npotHeight = 5U;
        const std::size_t pixelCount = static_cast<std::size_t>(npotWidth) * npotHeight;
        std::vector<std::uint8_t> pixels(pixelCount * 4U, 128U);
        for (std::size_t pixelIndex = 0U; pixelIndex < pixelCount; ++pixelIndex) {
            const std::size_t x = pixelIndex % npotWidth;
            const std::size_t y = pixelIndex / npotWidth;
            const std::size_t offset = pixelIndex * 4U;
            pixels[offset + 0U] = 128U;
            pixels[offset + 1U] = 128U;
            pixels[offset + 2U] = 255U;
            pixels[offset + 3U] = 255U;
            if (x == npotWidth - 1U) {
                pixels[offset + 0U] = 255U;
                pixels[offset + 1U] = 128U;
                pixels[offset + 2U] = 128U;
            }
            if (y == npotHeight - 1U) {
                pixels[offset + 0U] = 128U;
                pixels[offset + 1U] = 255U;
                pixels[offset + 2U] = 128U;
            }
        }

        const ve::LoadedImageRgba8 baseLevel{npotWidth, npotHeight, std::move(pixels)};
        std::vector<ve::LoadedImageRgba8> mipChain;
        expectNoThrow("buildNormalMapMipChainRgba8 samples NPOT boundaries proportionally", [&] {
            mipChain = ve::buildNormalMapMipChainRgba8(baseLevel);
        });
        expectEqual("NPOT chain has three levels", mipChain.size(), static_cast<std::size_t>(3));

        if (mipChain.size() == 3U) {
            expectEqual("NPOT base level width", mipChain[0U].width, npotWidth);
            expectEqual("NPOT base level height", mipChain[0U].height, npotHeight);
            expectEqual("NPOT mip level one width", mipChain[1U].width, 1U);
            expectEqual("NPOT mip level one height", mipChain[1U].height, 2U);
            expectEqual("NPOT final mip width", mipChain[2U].width, 1U);
            expectEqual("NPOT final mip height", mipChain[2U].height, 1U);
            const auto mipOneTop = decodeNormalFromImagePixel(mipChain[1U], 0U);
            const auto mipOneBottom = decodeNormalFromImagePixel(mipChain[1U], 1U);
            expectNearly("NPOT mip top normal length", normalLength(mipOneTop), 1.0f, 5.0e-3f);
            expectNearly("NPOT mip bottom normal length", normalLength(mipOneBottom), 1.0f, 5.0e-3f);
            expectGreater("NPOT top mip includes last-column normals", mipOneTop.x, 0.2f);
            expectAbsLess("NPOT top mip excludes last-row normals", mipOneTop.y, 0.02f);
            expectGreater("NPOT bottom mip includes last-row normals", mipOneBottom.y, 0.2f);
            const auto mipFinal = decodeNormalFromImagePixel(mipChain[2U], 0U);
            expectNearly("NPOT final mip normal length", normalLength(mipFinal), 1.0f, 5.0e-3f);
            expectGreater("NPOT final mip preserves edge X tilt", mipFinal.x, 0.2f);
            expectGreater("NPOT final mip preserves edge Y tilt", mipFinal.y, 0.15f);
        }
    }


    expectThrowsRuntimeError("buildNormalMapMipChainRgba8 rejects zero-dimension width", [] {
        const ve::LoadedImageRgba8 invalidWidth{0U, 2U, std::vector<std::uint8_t>(8U, 255U)};
        (void)ve::buildNormalMapMipChainRgba8(invalidWidth);
    });
    expectThrowsRuntimeError("buildNormalMapMipChainRgba8 rejects zero-dimension height", [] {
        const ve::LoadedImageRgba8 invalidHeight{2U, 0U, std::vector<std::uint8_t>(8U, 255U)};
        (void)ve::buildNormalMapMipChainRgba8(invalidHeight);
    });
    expectThrowsRuntimeError("buildNormalMapMipChainRgba8 rejects wrong-size payload", [] {
        const ve::LoadedImageRgba8 badPayload{2U, 2U, std::vector<std::uint8_t>{255U, 0U, 255U, 255U, 0U, 255U, 255U, 255U, 0U, 128U, 255U, 255U}};
        (void)ve::buildNormalMapMipChainRgba8(badPayload);
    });

    for (const auto& fixture : fixtures) {
        removeIfExists(fixture);
    }

    if (gFailureCount == 0) {
        return 0;
    }

    std::cerr << "ImageLoader CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
