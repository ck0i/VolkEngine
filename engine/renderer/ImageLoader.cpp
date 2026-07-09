#include "renderer/ImageLoader.hpp"

#include <cctype>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

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
    std::size_t consumed = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(token, &consumed, 10);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("PPM invalid ") + fieldName);
    }
    if (consumed != token.size() || value == 0UL || value > 0xffffffffUL) {
        throw std::runtime_error(std::string("PPM invalid ") + fieldName);
    }
    return static_cast<std::uint32_t>(value);
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

} // namespace ve
