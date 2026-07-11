#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <span>
#include <string_view>

namespace ve {

struct LoadedImageRgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

struct LoadedImageRgba32F {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> pixels;
};

[[nodiscard]] LoadedImageRgba8 loadPpmRgba8(const std::filesystem::path& path);
[[nodiscard]] LoadedImageRgba8 loadImageRgba8(const std::filesystem::path& path);
[[nodiscard]] LoadedImageRgba8 loadImageRgba8(std::span<const std::byte> encoded,
                                              std::string_view sourceName);
[[nodiscard]] LoadedImageRgba32F loadImageRgba32F(const std::filesystem::path& path);
[[nodiscard]] LoadedImageRgba32F loadImageRgba32F(std::span<const std::byte> encoded,
                                                 std::string_view sourceName);
[[nodiscard]] std::vector<LoadedImageRgba8> buildNormalMapMipChainRgba8(LoadedImageRgba8 baseLevel);
[[nodiscard]] std::vector<LoadedImageRgba8> buildAlbedoMipChainRgba8(LoadedImageRgba8 baseLevel, bool isSrgb);
[[nodiscard]] std::vector<LoadedImageRgba8> buildLinearMipChainRgba8(LoadedImageRgba8 baseLevel);

} // namespace ve
