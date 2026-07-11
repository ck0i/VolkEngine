#pragma once

#include "assets/ContentHash.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ve {

enum class ArtifactType : std::uint8_t { Mesh = 1, Texture = 2, Material = 3, Scene = 4 };

struct DerivedDataKeyInput {
    ContentHash sourceHash;
    std::string importerId;
    std::uint32_t importerVersion = 0;
    ContentHash settingsHash;
    std::vector<ContentHash> dependencyArtifactHashes;
    ArtifactType type = ArtifactType::Mesh;
    std::uint32_t artifactSchemaVersion = 1;
    std::string targetPlatform;
    std::string gpuFormat;
};

[[nodiscard]] ContentHash makeDerivedDataKey(const DerivedDataKeyInput& input);

struct ArtifactBlob {
    ArtifactType type = ArtifactType::Mesh;
    std::uint32_t schemaVersion = 0;
    ContentHash payloadHash;
    std::vector<std::byte> payload;
};

class DerivedDataCache {
public:
    static constexpr std::uint64_t kMaximumArtifactBytes = 1ULL << 30U;

    explicit DerivedDataCache(std::filesystem::path root) : root_(std::move(root)) {}
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] std::filesystem::path artifactPath(ContentHash key, ArtifactType type) const;
    [[nodiscard]] bool contains(ContentHash key, ArtifactType type) const;
    [[nodiscard]] ArtifactBlob load(ContentHash key, ArtifactType expectedType,
                                    std::uint32_t expectedSchemaVersion) const;
    [[nodiscard]] bool publish(ContentHash key, ArtifactType type, std::uint32_t schemaVersion,
                               std::span<const std::byte> payload);

private:
    std::filesystem::path root_;
};

} // namespace ve
