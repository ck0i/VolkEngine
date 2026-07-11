#pragma once

#include "assets/AssetDatabase.hpp"
#include "core/JobSystem.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ve {

enum class ResidencyClass : std::uint8_t {
  Texture,
  Geometry,
  WorldCell,
  Animation,
  Audio,
  Count,
};

inline constexpr std::size_t kResidencyClassCount =
    static_cast<std::size_t>(ResidencyClass::Count);

enum class ResidencyState : std::uint8_t {
  Unloaded,
  Queued,
  Loading,
  Resident,
  Failed,
  Evicting,
};

enum class ResidencyFailure : std::uint8_t {
  None,
  MissingResource,
  MissingDependency,
  DependencyCycle,
  Io,
  ArtifactTooLarge,
  OutOfMemory,
  Backpressure,
  Cancelled,
};

struct ResidencyKey {
  AssetId id;
  ResidencyClass resourceClass = ResidencyClass::WorldCell;

  [[nodiscard]] bool valid() const noexcept { return id.valid(); }
  friend bool operator==(ResidencyKey, ResidencyKey) = default;
  friend auto operator<=>(ResidencyKey, ResidencyKey) = default;
};

struct ResidencyResourceDesc {
  ResidencyKey key;
  std::filesystem::path artifactPath;
  std::uint64_t estimatedBytes = 0U;
  std::vector<ResidencyKey> dependencies;
  std::string name;
};

struct ResidencyConfig {
  std::size_t maximumResources = 4'096U;
  std::size_t maximumQueuedRequests = 1'024U;
  std::size_t maximumConcurrentLoads = 16U;
  std::uint64_t residencyBudgetBytes = 256U * 1024U * 1024U;
  std::size_t maximumArtifactBytes = 64U * 1024U * 1024U;
};

struct ResidencyMetrics {
  std::uint64_t residentBytes = 0U;
  std::uint64_t peakResidentBytes = 0U;
  std::uint64_t ioBytes = 0U;
  std::uint64_t publishedLoads = 0U;
  std::uint64_t evictions = 0U;
  std::uint64_t cancellations = 0U;
  std::uint64_t backpressureEvents = 0U;
  std::uint64_t missingDependencyFailures = 0U;
  std::uint64_t ioFailures = 0U;
  std::uint64_t outOfMemoryFailures = 0U;
  std::uint64_t staleCompletions = 0U;
  std::uint64_t mainThreadIoOperations = 0U;
  std::uint32_t queued = 0U;
  std::uint32_t loading = 0U;
  std::uint32_t resident = 0U;
  std::array<std::uint32_t, kResidencyClassCount> residentByClass{};
  std::array<std::uint64_t, kResidencyClassCount> bytesByClass{};
};

enum class ResidencyRequestResult : std::uint8_t {
  Accepted,
  Resident,
  Missing,
  Failed,
  Backpressured,
};

class ResidencyManager final {
public:
  ResidencyManager(JobSystem &jobs, ResidencyConfig config = {});
  ResidencyManager(const ResidencyManager &) = delete;
  ResidencyManager &operator=(const ResidencyManager &) = delete;
  ResidencyManager(ResidencyManager &&) = delete;
  ResidencyManager &operator=(ResidencyManager &&) = delete;
  ~ResidencyManager();

  void registerResource(ResidencyResourceDesc resource);
  void beginFrame(std::uint64_t frameIndex);
  [[nodiscard]] ResidencyRequestResult
  request(ResidencyKey key, std::int32_t priority, bool pin = false);
  void endFrame();
  void update();
  void retry(ResidencyKey key);
  void evict(ResidencyKey key);

  [[nodiscard]] ResidencyState state(ResidencyKey key) const noexcept;
  [[nodiscard]] ResidencyFailure failure(ResidencyKey key) const noexcept;
  [[nodiscard]] std::string_view diagnostic(ResidencyKey key) const noexcept;
  [[nodiscard]] std::span<const std::byte>
  payload(ResidencyKey key) const noexcept;
  [[nodiscard]] ResidencyMetrics metrics() const noexcept;
  [[nodiscard]] std::size_t resourceCount() const noexcept;

private:
  struct LoadTask;
  struct Entry;

  [[nodiscard]] Entry *find(ResidencyKey key) noexcept;
  [[nodiscard]] const Entry *find(ResidencyKey key) const noexcept;
  ResidencyRequestResult requestRecursive(Entry &entry, std::int32_t priority,
                                          bool pin,
                                          std::vector<ResidencyKey> &path);
  void publishCompleted();
  void scheduleQueued();
  bool makeRoom(std::uint64_t bytes, const Entry *protectedEntry);
  void evictEntry(Entry &entry);
  void fail(Entry &entry, ResidencyFailure failure, std::string diagnostic);
  void refreshMetrics() noexcept;

  JobSystem *jobs_ = nullptr;
  ResidencyConfig config_;
  std::vector<Entry> entries_;
  std::uint64_t frameIndex_ = 0U;
  ResidencyMetrics metrics_;
};

[[nodiscard]] std::string_view
residencyClassName(ResidencyClass value) noexcept;
[[nodiscard]] std::string_view
residencyFailureName(ResidencyFailure value) noexcept;

} // namespace ve
