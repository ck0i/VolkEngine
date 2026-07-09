#include "core/FileSystem.hpp"

#include <fstream>
#include <stdexcept>

namespace ve {

std::vector<std::byte> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open binary file: " + path.string());
    }

    const std::streamsize size = file.tellg();
    if (size < 0) {
        throw std::runtime_error("Failed to determine file size: " + path.string());
    }

    std::vector<std::byte> buffer(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (size > 0 && !file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        throw std::runtime_error("Failed to read binary file: " + path.string());
    }
    return buffer;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open text file: " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace ve
