#include "assets/ContentHash.hpp"
#include "assets/GltfImporter.hpp"
#include "core/Application.hpp"
#include "core/FileSystem.hpp"
#include "streaming/WorldPartition.hpp"

#if VOLKENGINE_ENABLE_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <limits>
namespace {

constexpr std::uint64_t kTraversalWarmupFrames = 60U;
constexpr std::uint64_t kTraversalFrames = 1'200U;
constexpr float kTraversalMinimumX = -7'000.0F;
constexpr float kTraversalMaximumX = 7'000.0F;

ve::AssetId cellId(const std::uint64_t value) {
  return {0x4d3153545245414dULL, value};
}

ve::ResidencyKey dependencyKey(const std::uint64_t value,
                               const ve::ResidencyClass resourceClass) {
  return {{0x4d31444550454e44ULL, value}, resourceClass};
}

std::uint64_t parseUnsigned(const std::string_view value,
                            const std::string_view option) {
  std::uint64_t result = 0U;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::invalid_argument(std::string(option) +
                                " requires an unsigned integer");
  return result;
}

float traversalX(const std::uint64_t frame) noexcept {
  if (frame < kTraversalWarmupFrames)
    return kTraversalMinimumX;
  const std::uint64_t step =
      std::min(frame - kTraversalWarmupFrames, kTraversalFrames - 1U);
  constexpr std::uint64_t half = kTraversalFrames / 2U;
  const std::uint64_t phase = step < half ? step : kTraversalFrames - 1U - step;
  const float alpha = static_cast<float>(phase) / static_cast<float>(half - 1U);
  return kTraversalMinimumX + (kTraversalMaximumX - kTraversalMinimumX) * alpha;
}

ve::CookedWorld makeCellWorld(const ve::WorldPartitionCell &cell,
                              const ve::AssetId mesh,
                              const ve::AssetId material,
                              const std::uint64_t ordinal) {
  ve::CookedWorld world;
  const std::size_t entityCount = std::max<std::size_t>(
      1U, static_cast<std::size_t>(std::ceil(cell.halfExtent * 2.0F / 256.0F)));
  world.identities.reserve(entityCount);
  world.names.reserve(entityCount);
  world.parentIndices.reserve(entityCount);
  world.transforms.reserve(entityCount);
  world.renderableMask.reserve(entityCount);
  world.renderables.reserve(entityCount);
  for (std::size_t index = 0U; index < entityCount; ++index) {
    world.identities.push_back(
        {0x4d31535452454e54ULL, ordinal * 1'000U + index + 1U});
    world.names.push_back("Streamed triangle " + std::to_string(ordinal) + "/" +
                          std::to_string(index));
    world.parentIndices.push_back(std::numeric_limits<std::uint32_t>::max());
    const float x = cell.center.x - cell.halfExtent + 128.0F +
                    static_cast<float>(index) * 256.0F;
    const float y =
        static_cast<float>(static_cast<std::int32_t>(ordinal % 5U) - 2) * 48.0F;
    const float z = -384.0F - static_cast<float>(ordinal % 3U) * 96.0F;
    world.transforms.push_back({{x, y, z}, {}, {32.0F, 32.0F, 32.0F}});
    world.renderableMask.push_back(1U);
    world.renderables.push_back({mesh, material, true});
  }
  ve::validateCookedWorld(world);
  return world;
}

struct BenchmarkArtifacts {
  ve::WorldPartitionManifest manifest;
  std::string manifestHash;
  std::string contentHash;
  std::uint64_t residencyBudget = 0U;
};

BenchmarkArtifacts cookBenchmark(
    const std::filesystem::path &root, const ve::ImportedGltfScene &reference,
    std::vector<std::pair<ve::ResidencyResourceDesc, std::vector<std::byte>>>
        &dependencies) {
  if (reference.meshes.empty() || reference.materials.empty())
    throw std::runtime_error("Reference scene has no benchmark material");
  std::filesystem::create_directories(root);

  const std::array dependencyClasses{
      ve::ResidencyClass::Texture, ve::ResidencyClass::Geometry,
      ve::ResidencyClass::Animation, ve::ResidencyClass::Audio};
  std::vector<ve::ResidencyKey> dependencyKeys;
  for (std::size_t index = 0U; index < dependencyClasses.size(); ++index) {
    std::vector<std::byte> bytes(64U + index * 8U,
                                 static_cast<std::byte>(0x31U + index));
    const std::filesystem::path path =
        root / (std::string(ve::residencyClassName(dependencyClasses[index])) +
                ".stream");
    ve::writeBinaryFileAtomic(path, bytes);
    const ve::ResidencyKey key =
        dependencyKey(index + 1U, dependencyClasses[index]);
    dependencyKeys.push_back(key);
    dependencies.push_back({{key,
                             path,
                             bytes.size(),
                             {},
                             "benchmark-" + std::string(ve::residencyClassName(
                                                dependencyClasses[index]))},
                            bytes});
  }

  BenchmarkArtifacts result;
  result.manifest.cells.push_back({cellId(1U),
                                   {},
                                   {},
                                   8'192.0F,
                                   10'000.0F,
                                   "cell-1.vecooked",
                                   1U,
                                   dependencyKeys});
  std::uint64_t nextId = 2U;
  const std::array levelOneCenters{
      ve::Vec3{-4'096.0F, 0.0F, -4'096.0F}, ve::Vec3{4'096.0F, 0.0F, -4'096.0F},
      ve::Vec3{-4'096.0F, 0.0F, 4'096.0F}, ve::Vec3{4'096.0F, 0.0F, 4'096.0F}};
  for (const ve::Vec3 center : levelOneCenters) {
    const ve::AssetId parentId = cellId(nextId++);
    result.manifest.cells.push_back(
        {parentId, cellId(1U), center, 4'096.0F, 6'000.0F,
         "cell-" + std::to_string(parentId.low) + ".vecooked", 1U,
         dependencyKeys});
    for (const float zOffset : {-2'048.0F, 2'048.0F}) {
      for (const float xOffset : {-2'048.0F, 2'048.0F}) {
        const ve::AssetId childId = cellId(nextId++);
        result.manifest.cells.push_back(
            {childId,
             parentId,
             {center.x + xOffset, 0.0F, center.z + zOffset},
             2'048.0F,
             0.0F,
             "cell-" + std::to_string(childId.low) + ".vecooked",
             1U,
             dependencyKeys});
      }
    }
  }

  std::vector<std::byte> aggregateContent;
  for (const auto &[description, bytes] : dependencies)
    aggregateContent.insert(aggregateContent.end(), bytes.begin(), bytes.end());
  std::uint64_t maximumCellBytes = 0U;
  std::ranges::sort(result.manifest.cells, {}, &ve::WorldPartitionCell::id);
  for (std::size_t index = 0U; index < result.manifest.cells.size(); ++index) {
    ve::WorldPartitionCell &cell = result.manifest.cells[index];
    const ve::CookedWorld world =
        makeCellWorld(cell, reference.meshes.front().id,
                      reference.meshes.front().material, index + 1U);
    const std::vector<std::byte> bytes = ve::encodeCookedWorld(world);
    ve::writeBinaryFileAtomic(root / cell.artifactPath, bytes);
    cell.estimatedBytes = bytes.size();
    maximumCellBytes =
        std::max(maximumCellBytes, static_cast<std::uint64_t>(bytes.size()));
    aggregateContent.insert(aggregateContent.end(), bytes.begin(), bytes.end());
  }
  const std::vector<std::byte> manifestBytes =
      ve::encodeWorldPartition(result.manifest);
  ve::writeBinaryFileAtomic(root / "benchmark.vepartition", manifestBytes);
  result.manifestHash = ve::hashBytes(manifestBytes).hex();
  result.contentHash = ve::hashBytes(aggregateContent).hex();
  result.residencyBudget =
      std::max(maximumCellBytes * 6U,
               static_cast<std::uint64_t>(aggregateContent.size() * 3U / 4U));
  return result;
}

struct TraversalContext {
  ve::ResidencyManager *residency = nullptr;
  ve::WorldPartitionRuntime *partition = nullptr;
  bool metricsReset = false;
  bool gateEnabled = false;
  std::uint64_t priorGapFrames = 0U;
  ve::StreamingRunStats current;
};

ve::StreamingRunStats streamingStats(const ve::ResidencyMetrics &residency,
                                     const ve::PartitionMetrics &partition) {
  ve::StreamingRunStats stats;
  stats.enabled = true;
  stats.residentBytes = residency.residentBytes;
  stats.peakResidentBytes = residency.peakResidentBytes;
  stats.ioBytes = residency.ioBytes;
  stats.publishedLoads = residency.publishedLoads;
  stats.evictions = residency.evictions;
  stats.cancellations = residency.cancellations;
  stats.backpressureEvents = residency.backpressureEvents;
  stats.missingDependencyFailures = residency.missingDependencyFailures;
  stats.ioFailures = residency.ioFailures;
  stats.outOfMemoryFailures = residency.outOfMemoryFailures;
  stats.staleCompletions = residency.staleCompletions;
  stats.mainThreadIoOperations = residency.mainThreadIoOperations;
  stats.traversalFrames = partition.traversalFrames;
  stats.coverageGapFrames = partition.coverageGapFrames;
  stats.publications = partition.publications;
  stats.originShifts = partition.originShifts;
  stats.partialLoadFailures = partition.partialLoadFailures;
  stats.retainedFrontierFrames = partition.retainedFrontierFrames;
  stats.queuedResources = residency.queued;
  stats.loadingResources = residency.loading;
  stats.residentResources = residency.resident;
  stats.activeCells = partition.activeCells;
  stats.desiredCells = partition.desiredCells;
  stats.pendingCells = partition.pendingCells;
  return stats;
}

bool updateTraversal(void *context, ve::Application &application,
                     ve::World &world, ve::Camera &camera,
                     const std::uint64_t frame) {
  auto &traversal = *static_cast<TraversalContext *>(context);
  const float observerX = traversalX(frame);
  const ve::Vec3 observer{observerX, 0.0F, 0.0F};
  if (frame == kTraversalWarmupFrames) {
    traversal.partition->resetTraversalMetrics();
    traversal.priorGapFrames = 0U;
    traversal.metricsReset = true;
  }
  traversal.partition->update(observer, frame + 1U);
  bool published = false;
  if (const ve::PartitionPublication *publication =
          traversal.partition->pendingPublication()) {
    try {
      application.instantiateCookedWorld(world, publication->world);
      traversal.partition->commitPublication(publication->revision);
      published = true;
    } catch (...) {
      traversal.partition->rejectPublication(publication->revision);
      throw;
    }
  }
  const ve::Vec3 origin = traversal.partition->worldOrigin();
  camera.setPosition({observer.x - origin.x, 1.6F, 5.0F - origin.z});

  const ve::ResidencyMetrics residencyMetrics = traversal.residency->metrics();
  const ve::PartitionMetrics partitionMetrics = traversal.partition->metrics();
  const ve::RenderStats renderStats = application.renderStats();
  const std::uint32_t maxVisible = std::max(
      traversal.current.maxVisibleInstances, renderStats.visibleItemCount);
  const std::uint64_t maxTriangles = std::max(
      traversal.current.maxSceneTriangles, renderStats.sceneTriangleCount);
  traversal.current = streamingStats(residencyMetrics, partitionMetrics);
  traversal.current.maxVisibleInstances = maxVisible;
  traversal.current.maxSceneTriangles = maxTriangles;
  if (traversal.metricsReset) {
    const bool gap =
        partitionMetrics.coverageGapFrames > traversal.priorGapFrames;
    traversal.priorGapFrames = partitionMetrics.coverageGapFrames;
    application.updateStreamingRunStats(
        traversal.current,
        {frame, observer.x, observer.z, origin.x, origin.z,
         residencyMetrics.residentBytes, residencyMetrics.resident,
         residencyMetrics.queued, residencyMetrics.loading,
         partitionMetrics.activeCells, partitionMetrics.desiredCells,
         partitionMetrics.pendingCells, gap});
  }
  return published;
}

void drawStreamingOverlay(void *context,
                          const ve::RendererOverlayFrame &frame) {
#if VOLKENGINE_ENABLE_IMGUI
  const auto &traversal = *static_cast<const TraversalContext *>(context);
  const ve::StreamingRunStats &stats = traversal.current;
  ImGui::SetNextWindowPos({12.0F, 28.0F}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.78F);
  if (ImGui::Begin("M1 streaming profiler")) {
    ImGui::Text("CPU %.3f ms / GPU %.3f ms", frame.stats.cpuFrameMs,
                frame.stats.gpuFrameMs);
    ImGui::Text("Resident %.2f MiB / peak %.2f MiB",
                static_cast<double>(stats.residentBytes) / (1024.0 * 1024.0),
                static_cast<double>(stats.peakResidentBytes) /
                    (1024.0 * 1024.0));
    ImGui::Text("Resources %u resident / %u loading / %u queued",
                stats.residentResources, stats.loadingResources,
                stats.queuedResources);
    ImGui::Text("Cells %u active / %u desired / %u pending", stats.activeCells,
                stats.desiredCells, stats.pendingCells);
    ImGui::Text("Loads %llu / evictions %llu / backpressure %llu",
                static_cast<unsigned long long>(stats.publishedLoads),
                static_cast<unsigned long long>(stats.evictions),
                static_cast<unsigned long long>(stats.backpressureEvents));
    ImGui::Text("Publications %llu / origin shifts %llu / retained %llu",
                static_cast<unsigned long long>(stats.publications),
                static_cast<unsigned long long>(stats.originShifts),
                static_cast<unsigned long long>(stats.retainedFrontierFrames));
    ImGui::Text("Coverage gaps %llu / main-thread IO %llu",
                static_cast<unsigned long long>(stats.coverageGapFrames),
                static_cast<unsigned long long>(stats.mainThreadIoOperations));
  }
  ImGui::End();
#else
  (void)context;
  (void)frame;
#endif
}

} // namespace

int main(int argc, char **argv) {
  try {
    ve::EngineConfig config;
    config.applicationName = "VolkEngine M1 Partition Benchmark";
    config.assetHotReload = false;
    ve::RunOptions run;
    run.maxFrames = kTraversalWarmupFrames + kTraversalFrames + 60U;
    run.warmupFrames = kTraversalWarmupFrames;
    run.scenarioName = "partition-traversal-v1";
    bool gate = false;
    for (int index = 1; index < argc; ++index) {
      const std::string_view option{argv[index]};
      const auto value = [&]() -> std::string_view {
        if (++index >= argc)
          throw std::invalid_argument(std::string(option) +
                                      " requires a value");
        return argv[index];
      };
      if (option == "--frames") {
        run.maxFrames = parseUnsigned(value(), option);
      } else if (option == "--screenshot") {
        run.screenshotPath = value();
      } else if (option == "--screenshot-frame") {
        run.screenshotFrame = parseUnsigned(value(), option);
      } else if (option == "--run-summary") {
        run.summaryPath = value();
      } else if (option == "--benchmark-gate") {
        gate = true;
      } else if (option == "--no-vsync") {
        config.vsync = false;
      } else if (option == "--validation") {
        config.validation = true;
      } else if (option == "--require-validation") {
        config.validation = true;
        config.requireValidation = true;
      } else if (option == "--help") {
        std::cout
            << "Usage: VolkEnginePartitionBenchmark [--frames N] "
               "[--benchmark-gate] [--no-vsync] "
               "[--validation|--require-validation] [--screenshot FILE.ppm] "
               "[--screenshot-frame N] [--run-summary FILE.json]\n";
        return 0;
      } else {
        throw std::invalid_argument("Unknown option " + std::string(option));
      }
    }

    ve::Application application{config};
    const ve::ImportedGltfScene reference =
        ve::importGltfScene(config.assetDirectory / "reference_scene.gltf",
                            ve::builtin_assets::kReferenceSceneId);
    const std::filesystem::path benchmarkRoot =
        config.cacheDirectory / "partition-benchmark";
    std::vector<std::pair<ve::ResidencyResourceDesc, std::vector<std::byte>>>
        dependencies;
    const BenchmarkArtifacts artifacts =
        cookBenchmark(benchmarkRoot, reference, dependencies);

    ve::ResidencyManager residency{
        application.jobSystem(),
        {.maximumResources = 128U,
         .maximumQueuedRequests = 64U,
         .maximumConcurrentLoads = 4U,
         .residencyBudgetBytes = artifacts.residencyBudget,
         .maximumArtifactBytes = 4U * 1024U * 1024U}};
    for (auto &[description, bytes] : dependencies) {
      (void)bytes;
      residency.registerResource(std::move(description));
    }
    ve::WorldPartitionRuntime partition{
        residency,
        ve::loadWorldPartition(benchmarkRoot / "benchmark.vepartition"),
        {.prefetchMargin = 1'000.0F,
         .originCellSize = 1'024.0F,
         .originShiftDistance = 2'048.0F}};
    application.configureStreamingRun(artifacts.manifestHash,
                                      artifacts.contentHash,
                                      artifacts.residencyBudget);

    TraversalContext traversal{&residency, &partition, false, gate, 0U, {}};
    application.setFrameUpdateCallback(updateTraversal, &traversal);
    application.setRendererOverlay(drawStreamingOverlay, &traversal);
    ve::World world;
    const int result = application.run(world, run);
    if (result != 0)
      return result;

    if (gate) {
      const ve::ResidencyMetrics residencyMetrics = residency.metrics();
      const ve::PartitionMetrics partitionMetrics = partition.metrics();
      if (partitionMetrics.traversalFrames < kTraversalFrames ||
          traversal.current.maxVisibleInstances == 0U ||
          traversal.current.maxSceneTriangles == 0U ||
          partitionMetrics.coverageGapFrames != 0U ||
          partitionMetrics.partialLoadFailures != 0U ||
          residencyMetrics.mainThreadIoOperations != 0U ||
          residencyMetrics.residentBytes > artifacts.residencyBudget ||
          residencyMetrics.outOfMemoryFailures != 0U ||
          residencyMetrics.ioFailures != 0U ||
          residencyMetrics.missingDependencyFailures != 0U) {
        std::cerr << "Partition traversal gate failed: frames "
                  << partitionMetrics.traversalFrames << ", gaps "
                  << partitionMetrics.coverageGapFrames << ", partial "
                  << partitionMetrics.partialLoadFailures << ", main IO "
                  << residencyMetrics.mainThreadIoOperations << ", resident "
                  << residencyMetrics.residentBytes << "/"
                  << artifacts.residencyBudget << '\n';
        return 2;
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Fatal partition benchmark error: " << error.what() << '\n';
    return 1;
  }
}
