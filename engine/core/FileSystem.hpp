#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ve {

std::vector<std::byte> readBinaryFile(const std::filesystem::path& path);
std::string readTextFile(const std::filesystem::path& path);

} // namespace ve
