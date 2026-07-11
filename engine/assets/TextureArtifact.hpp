#pragma once

#include "assets/GltfImporter.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ve {

enum class TextureStorage : std::uint8_t {
    Rgba8,
    Rgba32Float,
    Bc1Rgba,
    Bc3Rgba,
    Bc7Rgba,
};

struct TextureMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

struct TextureArtifact {
    static constexpr std::uint32_t kSchemaVersion = 2;

    AssetId id;
    TextureRole role = TextureRole::BaseColor;
    TextureColorSpace colorSpace = TextureColorSpace::Linear;
    TextureStorage storage = TextureStorage::Rgba8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<TextureMip> mips;
    std::vector<std::byte> data;
};

struct TextureImportOptions {
    std::size_t maximumSourceBytes = 256U * 1024U * 1024U;
    std::size_t maximumPayloadBytes = 512U * 1024U * 1024U;
};

[[nodiscard]] TextureArtifact importTextureArtifact(
    const std::filesystem::path& path, AssetId id, TextureRole role,
    TextureColorSpace colorSpace, const TextureImportOptions& options = {});
[[nodiscard]] std::vector<std::byte> serializeTextureArtifact(const TextureArtifact& texture);
[[nodiscard]] TextureArtifact deserializeTextureArtifact(std::span<const std::byte> bytes);

} // namespace ve
