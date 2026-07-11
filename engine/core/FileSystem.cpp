#include "core/FileSystem.hpp"
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
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

std::atomic<std::uint64_t> nextTemporaryFileSuffix = 0;

std::uint64_t processIdentifier() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#elif defined(__linux__) || defined(__APPLE__)
    return static_cast<std::uint64_t>(getpid());
#else
    return 0;
#endif
}

class TemporaryFile {
public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}

    ~TemporaryFile() {
        if (active_) {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }
    }

    const std::filesystem::path& path() const { return path_; }
    void arm() { active_ = true; }
    void release() { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_ = false;
};

} // namespace

std::filesystem::path executableDirectory() {
    static const std::filesystem::path directory = resolveExecutablePath().parent_path();
    return directory;
}

std::vector<std::byte> readBinaryFile(const std::filesystem::path& path) {
    return readBinaryFile(path, std::numeric_limits<std::size_t>::max());
}

std::vector<std::byte> readBinaryFile(const std::filesystem::path& path, const std::size_t maximumBytes) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open binary file: " + path.string());
    }

    const std::streamsize size = file.tellg();
    if (size < 0) {
        throw std::runtime_error("Failed to determine file size: " + path.string());
    }
    if (static_cast<std::uintmax_t>(size) > static_cast<std::uintmax_t>(maximumBytes) ||
        static_cast<std::uintmax_t>(size) > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("Binary file exceeds size limit: " + path.string());
    }

    std::vector<std::byte> buffer(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (size > 0 && !file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        throw std::runtime_error("Failed to read binary file: " + path.string());
    }
    return buffer;
}

void writeBinaryFileAtomic(const std::filesystem::path& path, const std::span<const std::byte> data) {
    if (path.empty() || path.filename().empty()) {
        throw std::runtime_error("Invalid binary file path: " + path.string());
    }
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("Binary file payload is too large: " + path.string());
    }

    std::filesystem::path temporaryPath;
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    for (;;) {
        temporaryPath = path;
        temporaryPath += std::filesystem::path{
            ".tmp." + std::to_string(processIdentifier()) + "."
            + std::to_string(nextTemporaryFileSuffix.fetch_add(1, std::memory_order_relaxed))};
        file = CreateFileW(temporaryPath.c_str(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            throw std::runtime_error(
                "Failed to create temporary binary file: " + temporaryPath.string()
                + ": Windows error " + std::to_string(error));
        }
    }

    TemporaryFile temporaryFile(temporaryPath);
    temporaryFile.arm();
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t remaining = data.size() - offset;
        const DWORD requested = remaining > std::numeric_limits<DWORD>::max()
                                    ? std::numeric_limits<DWORD>::max()
                                    : static_cast<DWORD>(remaining);
        DWORD written = 0;
        const BOOL writeSucceeded = WriteFile(file, data.data() + offset, requested, &written, nullptr);
        if (writeSucceeded == 0 || written != requested) {
            const DWORD error = writeSucceeded == 0 ? GetLastError() : ERROR_WRITE_FAULT;
            CloseHandle(file);
            throw std::runtime_error(
                "Failed to write temporary binary file: " + temporaryPath.string()
                + ": Windows error " + std::to_string(error));
        }
        offset += written;
    }
    if (FlushFileBuffers(file) == 0) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        throw std::runtime_error(
            "Failed to flush temporary binary file: " + temporaryPath.string()
            + ": Windows error " + std::to_string(error));
    }
    if (CloseHandle(file) == 0) {
        throw std::runtime_error(
            "Failed to close temporary binary file: " + temporaryPath.string()
            + ": Windows error " + std::to_string(GetLastError()));
    }
#else
    std::ofstream file;
    for (;;) {
        temporaryPath = path;
        temporaryPath += std::filesystem::path{
            ".tmp." + std::to_string(processIdentifier()) + "."
            + std::to_string(nextTemporaryFileSuffix.fetch_add(1, std::memory_order_relaxed))};
        file.open(temporaryPath, std::ios::binary | std::ios::noreplace);
        if (file) {
            break;
        }
        file.clear();
        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::symlink_status(temporaryPath, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error(
                "Failed to validate temporary binary file path: " + temporaryPath.string() + ": "
                + error.message());
        }
        if (status.type() == std::filesystem::file_type::not_found) {
            throw std::runtime_error("Failed to create temporary binary file: " + temporaryPath.string());
        }
    }

    TemporaryFile temporaryFile(temporaryPath);
    temporaryFile.arm();
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file) {
            file.close();
            throw std::runtime_error("Failed to write temporary binary file: " + temporaryPath.string());
        }
    }
    file.flush();
    if (!file) {
        file.close();
        throw std::runtime_error("Failed to flush temporary binary file: " + temporaryPath.string());
    }
    file.close();
    if (file.fail()) {
        throw std::runtime_error("Failed to close temporary binary file: " + temporaryPath.string());
    }
#endif

#if defined(_WIN32)
    if (MoveFileExW(temporaryFile.path().c_str(),
                    path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        throw std::runtime_error(
            "Failed to publish temporary binary file " + temporaryPath.string() + " to " + path.string()
            + ": Windows error " + std::to_string(GetLastError()));
    }
#else
    std::error_code error;
    std::filesystem::rename(temporaryFile.path(), path, error);
    if (error) {
        throw std::runtime_error(
            "Failed to publish temporary binary file " + temporaryPath.string() + " to " + path.string()
            + ": " + error.message());
    }
#endif
    temporaryFile.release();
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open text file: " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace ve
