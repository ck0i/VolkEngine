#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace ve {

enum class GpuResourceKind : std::uint8_t {
    Buffer,
    Image
};

class GpuResourceRegistry {
public:
    static constexpr std::uint32_t kInvalidId = 0xffffffffU;
    static constexpr std::size_t kMaxResources = 128;
    static constexpr std::size_t kMaxNameLength = 64;

    struct Record {
        std::uint32_t id = kInvalidId;
        std::array<char, kMaxNameLength> name{};
        GpuResourceKind kind = GpuResourceKind::Buffer;
        std::uint64_t bytes = 0;
        bool imported = false;
        bool live = false;
    };

    struct Stats {
        std::uint32_t liveResources = 0;
        std::uint32_t buffers = 0;
        std::uint32_t images = 0;
        std::uint32_t importedImages = 0;
        std::uint64_t bytes = 0;
        std::uint64_t bufferBytes = 0;
        std::uint64_t imageBytes = 0;
        std::uint64_t importedImageBytes = 0;
        std::uint64_t ownedImageBytes = 0;
    };

    [[nodiscard]] std::uint32_t registerResource(GpuResourceKind kind, std::string_view name, std::uint64_t bytes, bool imported = false) {
        for (Record& record : records_) {
            if (record.live) {
                continue;
            }
            const std::uint32_t id = nextId_++;
            record = Record{};
            record.id = id;
            record.kind = kind;
            record.bytes = bytes;
            record.imported = imported;
            record.live = true;
            copyName(record, name.empty() ? std::string_view{"Unnamed GPU Resource"} : name);
            return id;
        }
        throw std::runtime_error("GPU resource registry capacity exceeded");
    }

    void unregisterResource(std::uint32_t id) {
        if (id == kInvalidId) {
            return;
        }
        for (Record& record : records_) {
            if (record.live && record.id == id) {
                record.live = false;
                return;
            }
        }
    }

    [[nodiscard]] Stats stats() const {
        Stats result{};
        for (const Record& record : records_) {
            if (!record.live) {
                continue;
            }
            ++result.liveResources;
            result.bytes += record.bytes;
            if (record.kind == GpuResourceKind::Buffer) {
                ++result.buffers;
                result.bufferBytes += record.bytes;
            } else {
                ++result.images;
                result.imageBytes += record.bytes;
                if (record.imported) {
                    ++result.importedImages;
                    result.importedImageBytes += record.bytes;
                } else {
                    result.ownedImageBytes += record.bytes;
                }
            }
        }
        return result;
    }

private:
    static void copyName(Record& record, std::string_view name) {
        const std::size_t count = name.size() < (record.name.size() - 1U) ? name.size() : (record.name.size() - 1U);
        for (std::size_t i = 0; i < count; ++i) {
            record.name[i] = name[i];
        }
        record.name[count] = '\0';
    }

    std::array<Record, kMaxResources> records_{};
    std::uint32_t nextId_ = 1;
};

} // namespace ve
