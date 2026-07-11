#pragma once

#include "assets/AssetDatabase.hpp"
#include "assets/GltfImporter.hpp"
#include "assets/RuntimeAssets.hpp"
#include "core/JobSystem.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

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

class ReferenceAssetCookTask final {
public:
  ReferenceAssetCookTask(JobSystem &jobs, std::filesystem::path assetRoot,
                         std::filesystem::path cacheRoot,
                         std::string targetPlatform);
  ReferenceAssetCookTask(const ReferenceAssetCookTask &) = delete;
  ReferenceAssetCookTask &operator=(const ReferenceAssetCookTask &) = delete;
  ReferenceAssetCookTask(ReferenceAssetCookTask &&) = delete;
  ReferenceAssetCookTask &operator=(ReferenceAssetCookTask &&) = delete;
  ~ReferenceAssetCookTask();

  [[nodiscard]] ReferenceAssetBundle take();
  [[nodiscard]] bool finished() const;

private:
  static void cook(void *context, JobContext &job);

  JobSystem *jobs_ = nullptr;
  std::filesystem::path assetRoot_;
  std::filesystem::path cacheRoot_;
  std::string targetPlatform_;
  std::optional<ReferenceAssetBundle> result_;
  JobHandle handle_{};
};

class ReferenceAssetReloader {
public:
  ReferenceAssetReloader(std::filesystem::path assetRoot,
                         std::filesystem::path cacheRoot,
                         std::string targetPlatform);
  ~ReferenceAssetReloader();

  bool beginReload(JobSystem &jobs);
  [[nodiscard]] bool reloadPending() const noexcept;
  [[nodiscard]] std::optional<AssetReloadResult> pollReload();

  [[nodiscard]] const ReferenceAssetBundle &active() const noexcept {
    return active_;
  }
  [[nodiscard]] std::uint64_t generation() const noexcept {
    return generation_;
  }
  [[nodiscard]] AssetReloadResult reload() noexcept;

private:
  struct AsyncReload;

  std::filesystem::path assetRoot_;
  std::filesystem::path cacheRoot_;
  std::string targetPlatform_;
  ReferenceAssetBundle active_;
  std::uint64_t generation_ = 1;
  std::unique_ptr<AsyncReload> pendingReload_;
};

[[nodiscard]] MeshAssetHandle
referenceMeshHandle(const ImportedGltfScene &scene, AssetId meshId);
[[nodiscard]] ReferenceAssetBundle
cookReferenceAssets(const std::filesystem::path &assetRoot,
                    const std::filesystem::path &cacheRoot,
                    std::string targetPlatform);

} // namespace ve
