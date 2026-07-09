#include "renderer/ImageLoader.hpp"

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
    const std::filesystem::path filePath = std::filesystem::current_path() / std::string(name);
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

    for (const auto& fixture : fixtures) {
        removeIfExists(fixture);
    }

    if (gFailureCount == 0) {
        return 0;
    }

    std::cerr << "ImageLoader CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
