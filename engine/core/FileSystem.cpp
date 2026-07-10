#include "core/FileSystem.hpp"
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif


#include <fstream>
#include <stdexcept>

namespace ve {
namespace {

std::filesystem::path resolveExecutablePath() {
#if defined(_WIN32)
    std::wstring buffer(260, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            throw std::runtime_error("Failed to resolve executable path");
        }
        if (copied < buffer.size()) {
            buffer.resize(copied);
            return std::filesystem::path{buffer};
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    for (;;) {
        const ssize_t copied = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (copied < 0) {
            throw std::runtime_error("Failed to resolve executable path");
        }
        if (static_cast<std::size_t>(copied) < buffer.size()) {
            return std::filesystem::path{std::string(buffer.data(), static_cast<std::size_t>(copied))};
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::vector<char> buffer(256);
    for (;;) {
        std::uint32_t bufferSize = static_cast<std::uint32_t>(buffer.size());
        const int result = _NSGetExecutablePath(buffer.data(), &bufferSize);
        if (result == 0) {
            return std::filesystem::path{buffer.data()};
        }
        if (result != -1 || bufferSize <= buffer.size()) {
            throw std::runtime_error("Failed to resolve executable path");
        }
        buffer.resize(bufferSize);
    }
#else
    return std::filesystem::current_path();
#endif
}

} // namespace


std::filesystem::path executableDirectory() {
    static const std::filesystem::path directory = resolveExecutablePath().parent_path();
    return directory;
}

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
