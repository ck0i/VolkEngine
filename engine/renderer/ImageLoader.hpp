#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ve {

struct LoadedImageRgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

[[nodiscard]] LoadedImageRgba8 loadPpmRgba8(const std::filesystem::path& path);

} // namespace ve
