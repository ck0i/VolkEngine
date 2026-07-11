#pragma once

#include "core/Config.hpp"
#include "core/JobSystem.hpp"
#include "core/MetricDistribution.hpp"
#include "renderer/Renderer.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ve {
struct StreamingFrameSample {
  std::uint64_t frame = 0U;
  float observerX = 0.0F;
  float observerZ = 0.0F;
  float originX = 0.0F;
  float originZ = 0.0F;
  std::uint64_t residentBytes = 0U;
  std::uint32_t residentResources = 0U;
  std::uint32_t queuedResources = 0U;
  std::uint32_t loadingResources = 0U;
  std::uint32_t activeCells = 0U;
  std::uint32_t desiredCells = 0U;
  std::uint32_t pendingCells = 0U;
  bool coverageGap = false;
};

struct StreamingRunStats {
  static constexpr std::size_t kMaximumFrameSamples = 4'096U;
  bool enabled = false;
  std::string manifestHash;
  std::string contentHash;
  std::uint64_t budgetBytes = 0U;
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
  std::uint64_t traversalFrames = 0U;
  std::uint64_t coverageGapFrames = 0U;
  std::uint64_t publications = 0U;
  std::uint64_t originShifts = 0U;
  std::uint64_t partialLoadFailures = 0U;
  std::uint64_t retainedFrontierFrames = 0U;
  std::uint32_t queuedResources = 0U;
  std::uint32_t loadingResources = 0U;
  std::uint32_t residentResources = 0U;
  std::uint32_t activeCells = 0U;
  std::uint32_t desiredCells = 0U;
  std::uint32_t maxVisibleInstances = 0U;
  std::uint64_t maxSceneTriangles = 0U;
  std::uint32_t pendingCells = 0U;
  std::vector<StreamingFrameSample> frames;
};


struct LandscapeRunStats {
  bool enabled = false;
  std::uint64_t seed = 0U;
  std::string contentHash;
  std::array<std::uint32_t, 3> terrainPatchesByLod{};
  std::uint64_t terrainVertices = 0U;
  std::uint64_t terrainTriangles = 0U;
  std::array<std::uint32_t, 4> biomeSampleCounts{};
  std::array<std::uint32_t, 3> foliageInstancesBySpecies{};
  std::uint32_t waterPatchCount = 0U;
  std::uint32_t editBrushCount = 0U;
  std::uint64_t editRevision = 0U;
  float traversalDistanceMeters = 0.0F;
  bool atmosphere = false;
  bool gpuFoliageWind = false;
  std::uint32_t maxVisibleLandscapeInstances = 0U;
  std::uint32_t maxVisibleFoliageInstances = 0U;
  std::uint32_t maxVisibleWaterInstances = 0U;
  double cpuFrameBudgetMs = 0.0;
  double gpuFrameBudgetMs = 0.0;
};

struct RunSummary {
  static constexpr std::uint32_t kSchemaVersion = 7;

  EngineConfig config;
  RunOptions options;
  RenderDeviceInfo device;
  RenderStats stats;
  RunMetricDistributions distributions;
  JobSystemStats jobs;
  StreamingRunStats streaming;
  LandscapeRunStats landscape;
  std::uint64_t frameCount = 0;
  int exitStatus = 0;
};

[[nodiscard]] std::string serializeRunSummary(const RunSummary &summary);
void writeRunSummaryAtomic(const std::filesystem::path &path,
                           const RunSummary &summary);

} // namespace ve
