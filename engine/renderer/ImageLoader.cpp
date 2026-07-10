#include "renderer/ImageLoader.hpp"

#include <cmath>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <ios>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <utility>

#include <stb_image.h>

namespace ve {
namespace {

std::string readTokenSkippingComments(std::ifstream& file) {
    std::string token;
    char ch = '\0';
    while (file.get(ch)) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            continue;
        }
        if (ch == '#') {
            std::string ignored;
            std::getline(file, ignored);
            continue;
        }
        token.push_back(ch);
        break;
    }

    while (file.get(ch)) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            break;
        }
        if (ch == '#') {
            std::string ignored;
            std::getline(file, ignored);
            break;
        }
        token.push_back(ch);
    }
    return token;
}

std::uint32_t parsePositiveU32(const std::string& token, const char* fieldName) {
    if (token.empty()) {
        throw std::runtime_error(std::string("PPM missing ") + fieldName);
    }
    std::uint32_t value = 0;
    const char* begin = token.data();
    const char* end = begin + token.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value == 0U) {
        throw std::runtime_error(std::string("PPM invalid ") + fieldName);
    }
    return value;
}

std::string lowercaseExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension;
}

std::vector<std::uint8_t> readWholeBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open image: " + path.string());
    }
    const std::streamoff size = file.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Image file is too large for stb_image: " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (file.gcount() != static_cast<std::streamsize>(bytes.size())) {
            throw std::runtime_error("Image file ended early: " + path.string());
        }
    }
    return bytes;
}

struct StbiPixels {
    stbi_uc* data = nullptr;
    explicit StbiPixels(stbi_uc* pixels) noexcept
        : data(pixels) {}


    ~StbiPixels() {
        stbi_image_free(data);
    }

    StbiPixels(const StbiPixels&) = delete;
    StbiPixels& operator=(const StbiPixels&) = delete;
};

LoadedImageRgba8 loadStbImageRgba8(const std::filesystem::path& path) {
    const std::vector<std::uint8_t> encoded = readWholeBinaryFile(path);
    if (encoded.empty()) {
        throw std::runtime_error("Image file is empty: " + path.string());
    }
    int width = 0;
    int height = 0;
    int channelsInFile = 0;
    const StbiPixels decoded{stbi_load_from_memory(encoded.data(),
                                                   static_cast<int>(encoded.size()),
                                                   &width,
                                                   &height,
                                                   &channelsInFile,
                                                   STBI_rgb_alpha)};
    if (decoded.data == nullptr) {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error("Unsupported image format in " + path.string() + (reason != nullptr ? std::string(": ") + reason : std::string{}));
    }
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("Decoded image has invalid dimensions: " + path.string());
    }
    const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixelCount64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) / 4ULL) {
        throw std::runtime_error("Decoded image is too large: " + path.string());
    }
    LoadedImageRgba8 image{};
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    const std::size_t byteCount = static_cast<std::size_t>(pixelCount64) * 4U;
    image.pixels.assign(decoded.data, decoded.data + byteCount);
    return image;
}

struct NormalSample {
    float x = 0.0f;
    float y = 0.0f;
    float z = 1.0f;
};

void validateRgba8Image(const LoadedImageRgba8& image, const char* context) {
    if (image.width == 0U || image.height == 0U) {
        throw std::runtime_error(std::string(context) + " image dimensions must be positive");
    }
    const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(image.width) * image.height;
    if (pixelCount64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) / 4ULL ||
        image.pixels.size() != static_cast<std::size_t>(pixelCount64) * 4U) {
        throw std::runtime_error(std::string(context) + " image must contain width*height RGBA8 pixels");
    }
}

[[nodiscard]] NormalSample normalizeNormalSample(const NormalSample sample) {
    const float length = std::sqrt(sample.x * sample.x + sample.y * sample.y + sample.z * sample.z);
    if (length <= 0.000001f) {
        return {};
    }
    const float invLength = 1.0f / length;
    return {sample.x * invLength, sample.y * invLength, sample.z * invLength};
}

[[nodiscard]] NormalSample decodeNormalSample(const LoadedImageRgba8& image, const std::uint32_t x, const std::uint32_t y) {
    const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
    return normalizeNormalSample({static_cast<float>(image.pixels[offset + 0U]) / 127.5f - 1.0f,
                                  static_cast<float>(image.pixels[offset + 1U]) / 127.5f - 1.0f,
                                  static_cast<float>(image.pixels[offset + 2U]) / 127.5f - 1.0f});
}

[[nodiscard]] std::uint8_t encodeNormalComponent(const float value) {
    const float encoded = std::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f;
    return static_cast<std::uint8_t>(std::lround(encoded));
}

[[nodiscard]] std::uint8_t encodeUnorm8(const float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

[[nodiscard]] float decodeColorSample(const std::uint8_t value, const bool isSrgb) {
    const float encoded = static_cast<float>(value) / 255.0f;
    if (!isSrgb) {
        return encoded;
    }
    return encoded <= 0.04045f ? encoded / 12.92f
                               : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] std::uint8_t encodeColorSample(float value, const bool isSrgb) {
    value = std::clamp(value, 0.0f, 1.0f);
    if (!isSrgb) {
        return encodeUnorm8(value);
    }
    const float encoded = value <= 0.0031308f ? value * 12.92f
                                              : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return encodeUnorm8(encoded);
}

[[nodiscard]] std::uint32_t mipLevelCountForExtent(std::uint32_t width, std::uint32_t height) {
    std::uint32_t levels = 1;
    while (width > 1U || height > 1U) {
        width = std::max(1U, width / 2U);
        height = std::max(1U, height / 2U);
        ++levels;
    }
    return levels;
}

[[nodiscard]] LoadedImageRgba8 buildNextNormalMipLevel(const LoadedImageRgba8& source) {
    LoadedImageRgba8 mip{};
    mip.width = std::max(1U, source.width / 2U);
    mip.height = std::max(1U, source.height / 2U);
    mip.pixels.resize(static_cast<std::size_t>(mip.width) * mip.height * 4U);
    for (std::uint32_t y = 0; y < mip.height; ++y) {
        for (std::uint32_t x = 0; x < mip.width; ++x) {
            NormalSample sum{};
            sum.z = 0.0f;
            std::uint32_t alphaSum = 0;
            std::uint32_t sampleCount = 0;
            const std::uint32_t xBegin = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * source.width) / mip.width);
            const std::uint32_t yBegin = static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * source.height) / mip.height);
            const std::uint32_t xEnd = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x + 1U) * source.width) / mip.width);
            const std::uint32_t yEnd = static_cast<std::uint32_t>((static_cast<std::uint64_t>(y + 1U) * source.height) / mip.height);
            for (std::uint32_t sy = yBegin; sy < yEnd; ++sy) {
                for (std::uint32_t sx = xBegin; sx < xEnd; ++sx) {
                    const NormalSample normal = decodeNormalSample(source, sx, sy);
                    sum.x += normal.x;
                    sum.y += normal.y;
                    sum.z += normal.z;
                    alphaSum += source.pixels[(static_cast<std::size_t>(sy) * source.width + sx) * 4U + 3U];
                    ++sampleCount;
                }
            }
            const NormalSample normal = normalizeNormalSample(sum);
            const std::size_t offset = (static_cast<std::size_t>(y) * mip.width + x) * 4U;
            mip.pixels[offset + 0U] = encodeNormalComponent(normal.x);
            mip.pixels[offset + 1U] = encodeNormalComponent(normal.y);
            mip.pixels[offset + 2U] = encodeNormalComponent(normal.z);
            mip.pixels[offset + 3U] = static_cast<std::uint8_t>((alphaSum + sampleCount / 2U) / sampleCount);
        }
    }
    return mip;
}

[[nodiscard]] LoadedImageRgba8 buildNextAlbedoMipLevel(const LoadedImageRgba8& source, const bool isSrgb) {
    LoadedImageRgba8 mip{};
    mip.width = std::max(1U, source.width / 2U);
    mip.height = std::max(1U, source.height / 2U);
    mip.pixels.resize(static_cast<std::size_t>(mip.width) * mip.height * 4U);
    for (std::uint32_t y = 0; y < mip.height; ++y) {
        for (std::uint32_t x = 0; x < mip.width; ++x) {
            float colorSum[3]{};
            std::uint32_t alphaSum = 0;
            std::uint32_t sampleCount = 0;
            const std::uint32_t xBegin = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * source.width) / mip.width);
            const std::uint32_t yBegin = static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * source.height) / mip.height);
            const std::uint32_t xEnd = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x + 1U) * source.width) / mip.width);
            const std::uint32_t yEnd = static_cast<std::uint32_t>((static_cast<std::uint64_t>(y + 1U) * source.height) / mip.height);
            for (std::uint32_t sy = yBegin; sy < yEnd; ++sy) {
                for (std::uint32_t sx = xBegin; sx < xEnd; ++sx) {
                    const std::size_t offset = (static_cast<std::size_t>(sy) * source.width + sx) * 4U;
                    const std::uint8_t alpha = source.pixels[offset + 3U];
                    const float alphaWeight = static_cast<float>(alpha);
                    colorSum[0] += decodeColorSample(source.pixels[offset + 0U], isSrgb) * alphaWeight;
                    colorSum[1] += decodeColorSample(source.pixels[offset + 1U], isSrgb) * alphaWeight;
                    colorSum[2] += decodeColorSample(source.pixels[offset + 2U], isSrgb) * alphaWeight;
                    alphaSum += alpha;
                    ++sampleCount;
                }
            }
            const float invColorWeight = alphaSum > 0U ? 1.0f / static_cast<float>(alphaSum) : 0.0f;
            const std::size_t offset = (static_cast<std::size_t>(y) * mip.width + x) * 4U;
            mip.pixels[offset + 0U] = encodeColorSample(colorSum[0] * invColorWeight, isSrgb);
            mip.pixels[offset + 1U] = encodeColorSample(colorSum[1] * invColorWeight, isSrgb);
            mip.pixels[offset + 2U] = encodeColorSample(colorSum[2] * invColorWeight, isSrgb);
            mip.pixels[offset + 3U] = static_cast<std::uint8_t>((alphaSum + sampleCount / 2U) / sampleCount);
        }
    }
    return mip;
}

[[nodiscard]] LoadedImageRgba8 buildNextLinearMipLevel(const LoadedImageRgba8& source) {
    LoadedImageRgba8 mip{};
    mip.width = std::max(1U, source.width / 2U);
    mip.height = std::max(1U, source.height / 2U);
    mip.pixels.resize(static_cast<std::size_t>(mip.width) * mip.height * 4U);
    for (std::uint32_t y = 0; y < mip.height; ++y) {
        for (std::uint32_t x = 0; x < mip.width; ++x) {
            std::uint32_t sum[4]{};
            std::uint32_t sampleCount = 0;
            const std::uint32_t xBegin = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * source.width) / mip.width);
            const std::uint32_t yBegin = static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * source.height) / mip.height);
            const std::uint32_t xEnd = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x + 1U) * source.width) / mip.width);
            const std::uint32_t yEnd = static_cast<std::uint32_t>((static_cast<std::uint64_t>(y + 1U) * source.height) / mip.height);
            for (std::uint32_t sy = yBegin; sy < yEnd; ++sy) {
                for (std::uint32_t sx = xBegin; sx < xEnd; ++sx) {
                    const std::size_t offset = (static_cast<std::size_t>(sy) * source.width + sx) * 4U;
                    sum[0] += source.pixels[offset + 0U];
                    sum[1] += source.pixels[offset + 1U];
                    sum[2] += source.pixels[offset + 2U];
                    sum[3] += source.pixels[offset + 3U];
                    ++sampleCount;
                }
            }
            const std::size_t offset = (static_cast<std::size_t>(y) * mip.width + x) * 4U;
            mip.pixels[offset + 0U] = static_cast<std::uint8_t>((sum[0] + sampleCount / 2U) / sampleCount);
            mip.pixels[offset + 1U] = static_cast<std::uint8_t>((sum[1] + sampleCount / 2U) / sampleCount);
            mip.pixels[offset + 2U] = static_cast<std::uint8_t>((sum[2] + sampleCount / 2U) / sampleCount);
            mip.pixels[offset + 3U] = static_cast<std::uint8_t>((sum[3] + sampleCount / 2U) / sampleCount);
        }
    }
    return mip;
}

} // namespace

LoadedImageRgba8 loadPpmRgba8(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open PPM image: " + path.string());
    }

    const std::string magic = readTokenSkippingComments(file);
    if (magic != "P6") {
        throw std::runtime_error("Unsupported PPM image format in " + path.string() + ": expected binary P6");
    }

    const std::uint32_t width = parsePositiveU32(readTokenSkippingComments(file), "width");
    const std::uint32_t height = parsePositiveU32(readTokenSkippingComments(file), "height");
    const std::uint32_t maxValue = parsePositiveU32(readTokenSkippingComments(file), "max value");
    const std::uint32_t bytesPerSample = maxValue < 256U ? 1U : 2U;
    if (maxValue > 65535U) {
        throw std::runtime_error("Unsupported PPM max value in " + path.string() + ": expected 1..65535");
    }

    const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(width) * height;
    const std::uint64_t encodedBytesPerPixel = 3ULL * bytesPerSample;
    if (pixelCount64 > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) / encodedBytesPerPixel ||
        pixelCount64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) / encodedBytesPerPixel ||
        pixelCount64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) / 4ULL) {
        throw std::runtime_error("PPM image too large: " + path.string());
    }
    const std::uint64_t rgbBytes64 = pixelCount64 * encodedBytesPerPixel;

    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(rgbBytes64));
    file.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    if (file.gcount() != static_cast<std::streamsize>(rgb.size())) {
        throw std::runtime_error("PPM image ended early: " + path.string());
    }

    LoadedImageRgba8 image{};
    image.width = width;
    image.height = height;
    const std::size_t pixelCount = static_cast<std::size_t>(pixelCount64);
    image.pixels.resize(pixelCount * 4U);
    const auto readSample = [&](const std::size_t sampleIndex) -> std::uint8_t {
        const std::size_t offset = sampleIndex * bytesPerSample;
        std::uint32_t value = rgb[offset];
        if (bytesPerSample == 2U) {
            value = (value << 8U) | rgb[offset + 1U];
        }
        if (value > maxValue) {
            throw std::runtime_error("PPM sample exceeds max value in " + path.string());
        }
        return static_cast<std::uint8_t>((value * 255U + (maxValue / 2U)) / maxValue);
    };
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const std::size_t sample = pixel * 3U;
        const std::size_t dst = pixel * 4U;
        image.pixels[dst + 0] = readSample(sample + 0U);
        image.pixels[dst + 1] = readSample(sample + 1U);
        image.pixels[dst + 2] = readSample(sample + 2U);
        image.pixels[dst + 3] = 255U;
    }
    return image;
}

LoadedImageRgba8 loadImageRgba8(const std::filesystem::path& path) {
    if (lowercaseExtension(path) == ".ppm") {
        return loadPpmRgba8(path);
    }
    return loadStbImageRgba8(path);
}

std::vector<LoadedImageRgba8> buildAlbedoMipChainRgba8(LoadedImageRgba8 baseLevel, const bool isSrgb) {
    validateRgba8Image(baseLevel, "Albedo");
    std::vector<LoadedImageRgba8> levels;
    levels.reserve(mipLevelCountForExtent(baseLevel.width, baseLevel.height));
    levels.push_back(std::move(baseLevel));
    while (levels.back().width > 1U || levels.back().height > 1U) {
        levels.push_back(buildNextAlbedoMipLevel(levels.back(), isSrgb));
    }
    return levels;
}

std::vector<LoadedImageRgba8> buildLinearMipChainRgba8(LoadedImageRgba8 baseLevel) {
    validateRgba8Image(baseLevel, "Linear image");
    std::vector<LoadedImageRgba8> levels;
    levels.reserve(mipLevelCountForExtent(baseLevel.width, baseLevel.height));
    levels.push_back(std::move(baseLevel));
    while (levels.back().width > 1U || levels.back().height > 1U) {
        levels.push_back(buildNextLinearMipLevel(levels.back()));
    }
    return levels;
}

std::vector<LoadedImageRgba8> buildNormalMapMipChainRgba8(LoadedImageRgba8 baseLevel) {
    validateRgba8Image(baseLevel, "Normal map");
    std::vector<LoadedImageRgba8> levels;
    levels.reserve(mipLevelCountForExtent(baseLevel.width, baseLevel.height));
    levels.push_back(std::move(baseLevel));
    while (levels.back().width > 1U || levels.back().height > 1U) {
        levels.push_back(buildNextNormalMipLevel(levels.back()));
    }
    return levels;
}

} // namespace ve
