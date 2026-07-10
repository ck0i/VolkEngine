#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ve {
std::filesystem::path executableDirectory();

std::vector<std::byte> readBinaryFile(const std::filesystem::path& path);
std::vector<std::byte> readBinaryFile(const std::filesystem::path& path, std::size_t maximumBytes);
void writeBinaryFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data);
std::string readTextFile(const std::filesystem::path& path);

} // namespace ve
