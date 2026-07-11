#pragma once

#include "assets/AssetDatabase.hpp"
#include "assets/RuntimeAssets.hpp"
#include "assets/GltfImporter.hpp"

#include <cstdint>
#include <filesystem>

namespace ve {

struct AssetPipelineMetrics {
    double cookMilliseconds = 0.0;
    std::uint32_t cacheHits = 0;
    std::uint32_t cacheMisses = 0;
    std::uint32_t rebuiltAssets = 0;
};

struct ReferenceAssetBundle {
    AssetDatabase database;
    ImportedGltfScene scene;
    AssetPipelineMetrics metrics;
};

enum class AssetReloadStatus : std::uint8_t { Unchanged, Published, Failed };

struct AssetReloadResult {
    AssetReloadStatus status = AssetReloadStatus::Unchanged;
    AssetPipelineMetrics metrics;
    std::string diagnostic;
};

class ReferenceAssetReloader {
public:
    ReferenceAssetReloader(std::filesystem::path assetRoot,
                           std::filesystem::path cacheRoot,
                           std::string targetPlatform);

    [[nodiscard]] const ReferenceAssetBundle& active() const noexcept { return active_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] AssetReloadResult reload() noexcept;

private:
    std::filesystem::path assetRoot_;
    std::filesystem::path cacheRoot_;
    std::string targetPlatform_;
    ReferenceAssetBundle active_;
    std::uint64_t generation_ = 1;
};

[[nodiscard]] MeshAssetHandle referenceMeshHandle(const ImportedGltfScene& scene,
                                                  AssetId meshId);
[[nodiscard]] ReferenceAssetBundle cookReferenceAssets(const std::filesystem::path& assetRoot,
                                                        const std::filesystem::path& cacheRoot,
                                                        std::string targetPlatform);

} // namespace ve
