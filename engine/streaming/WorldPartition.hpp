#pragma once

#include "core/Math.hpp"
#include "scene/CookedWorld.hpp"
#include "streaming/ResidencyManager.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace ve {

struct WorldPartitionLimits {
  std::size_t maximumCells = 65'536U;
  std::size_t maximumDependenciesPerCell = 256U;
  std::size_t maximumPathBytes = 1'024U;
  std::size_t maximumManifestBytes = 64U * 1024U * 1024U;
};

struct WorldPartitionCell {
  AssetId id;
  AssetId parent;
  Vec3 center{};
  float halfExtent = 1.0F;
  float splitDistance = 0.0F;
  std::filesystem::path artifactPath;
  std::uint64_t estimatedBytes = 0U;
  std::vector<ResidencyKey> dependencies;
};

struct WorldPartitionManifest {
  std::vector<WorldPartitionCell> cells;
};

void validateWorldPartition(const WorldPartitionManifest &manifest,
                            const WorldPartitionLimits &limits = {});
[[nodiscard]] std::vector<std::byte>
encodeWorldPartition(const WorldPartitionManifest &manifest,
                     const WorldPartitionLimits &limits = {});
[[nodiscard]] WorldPartitionManifest
decodeWorldPartition(std::span<const std::byte> bytes,
                     const std::filesystem::path &artifactRoot = {},
                     const WorldPartitionLimits &limits = {});
void saveWorldPartition(const WorldPartitionManifest &manifest,
                        const std::filesystem::path &path,
                        const WorldPartitionLimits &limits = {});
[[nodiscard]] WorldPartitionManifest
loadWorldPartition(const std::filesystem::path &path,
                   const WorldPartitionLimits &limits = {});

struct PartitionRuntimeConfig {
  float prefetchMargin = 64.0F;
  float originCellSize = 1'024.0F;
  float originShiftDistance = 2'048.0F;
  std::int32_t activePriority = 1'000'000;
  std::int32_t desiredPriority = 100'000;
  std::int32_t prefetchPriority = 1'000;
  CookedWorldLimits cookedWorldLimits{};
};

struct PartitionMetrics {
  std::uint64_t traversalFrames = 0U;
  std::uint64_t coverageGapFrames = 0U;
  std::uint64_t publications = 0U;
  std::uint64_t originShifts = 0U;
  std::uint64_t partialLoadFailures = 0U;
  std::uint64_t retainedFrontierFrames = 0U;
  std::uint32_t activeCells = 0U;
  std::uint32_t desiredCells = 0U;
  std::uint32_t pendingCells = 0U;
  Vec3 worldOrigin{};
};

struct PartitionPublication {
  std::uint64_t revision = 0U;
  Vec3 worldOrigin{};
  CookedWorld world;
  std::vector<AssetId> cells;
};

class WorldPartitionRuntime final {
public:
  WorldPartitionRuntime(ResidencyManager &residency,
                        WorldPartitionManifest manifest,
                        PartitionRuntimeConfig config = {});
  void retryRejectedFrontier();

  void update(Vec3 globalObserver, std::uint64_t frameIndex);
  [[nodiscard]] const PartitionPublication *pendingPublication() const noexcept;
  void commitPublication(std::uint64_t revision);
  void rejectPublication(std::uint64_t revision) noexcept;
  void resetTraversalMetrics() noexcept;

  [[nodiscard]] std::span<const AssetId> activeCells() const noexcept;
  [[nodiscard]] Vec3 worldOrigin() const noexcept;
  [[nodiscard]] PartitionMetrics metrics() const noexcept;

private:
  [[nodiscard]] const WorldPartitionCell *find(AssetId id) const noexcept;
  void selectCell(const WorldPartitionCell &cell, Vec3 observer,
                  std::vector<AssetId> &output) const;
  void collectPrefetch(const WorldPartitionCell &cell, Vec3 observer,
                       std::vector<AssetId> &output) const;
  [[nodiscard]] bool coversObserver(std::span<const AssetId> cells,
                                    Vec3 observer) const noexcept;
  [[nodiscard]] Vec3 desiredOrigin(Vec3 observer) const noexcept;
  void buildPublication(const std::vector<AssetId> &desired, Vec3 origin);

  ResidencyManager *residency_ = nullptr;
  WorldPartitionManifest manifest_;
  PartitionRuntimeConfig config_;
  std::vector<AssetId> roots_;
  std::vector<AssetId> active_;
  std::vector<AssetId> desired_;
  std::vector<std::vector<std::size_t>> children_;
  std::vector<AssetId> prefetch_;
  std::optional<PartitionPublication> pending_;
  std::vector<AssetId> rejectedFrontier_;
  Vec3 origin_{};
  std::uint64_t nextRevision_ = 1U;
  PartitionMetrics metrics_;
};

[[nodiscard]] CookedWorld combineCookedWorldCells(
    std::span<const std::span<const std::byte>> encodedCells, Vec3 worldOrigin,
    const CookedWorldLimits &limits = {});

} // namespace ve
