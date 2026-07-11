#include "core/FileSystem.hpp"
#include "streaming/WorldPartition.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {

template <typename Exception, typename Callback>
bool throws(Callback &&callback) {
  try {
    callback();
    return false;
  } catch (const Exception &) {
    return true;
  }
}

ve::AssetId id(const std::uint64_t value) {
  return {0x4d31504152544954ULL, value};
}

ve::CookedWorld oneEntityWorld(const std::uint64_t value,
                               const ve::Vec3 translation) {
  ve::CookedWorld world;
  world.identities.push_back({0x4d31574f524c4445ULL, value});
  world.names.push_back("Partition entity " + std::to_string(value));
  world.parentIndices.push_back(std::numeric_limits<std::uint32_t>::max());
  world.transforms.push_back({translation, {}, {1.0F, 1.0F, 1.0F}});
  world.renderableMask.push_back(0U);
  world.renderables.push_back({});
  ve::validateCookedWorld(world);
  return world;
}

std::uint64_t writeCell(const std::filesystem::path &path,
                        const ve::CookedWorld &world) {
  const std::vector<std::byte> bytes = ve::encodeCookedWorld(world);
  ve::writeBinaryFileAtomic(path, bytes);
  return bytes.size();
}

const ve::PartitionPublication &
waitForPublication(ve::WorldPartitionRuntime &runtime, const ve::Vec3 observer,
                   std::uint64_t &frame) {
  for (std::size_t attempt = 0U; attempt < 100'000U; ++attempt) {
    runtime.update(observer, frame++);
    if (const ve::PartitionPublication *publication =
            runtime.pendingPublication()) {
      return *publication;
    }
    std::this_thread::yield();
  }
  throw std::runtime_error("Partition publication timed out");
}

} // namespace

int main() {
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path() /
      ("volkengine-partition-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(temporary);

  const std::filesystem::path rootPath = temporary / "root.vecooked";
  const std::uint64_t rootBytes =
      writeCell(rootPath, oneEntityWorld(1U, {0.0F, 0.0F, 0.0F}));
  std::array<std::filesystem::path, 4> childPaths;
  std::array<std::uint64_t, 4> childBytes{};
  const std::array centers{
      ve::Vec3{-2'048.0F, 0.0F, -2'048.0F}, ve::Vec3{2'048.0F, 0.0F, -2'048.0F},
      ve::Vec3{-2'048.0F, 0.0F, 2'048.0F}, ve::Vec3{2'048.0F, 0.0F, 2'048.0F}};
  for (std::size_t index = 0U; index < childPaths.size(); ++index) {
    childPaths[index] =
        temporary / ("child-" + std::to_string(index) + ".vecooked");
    childBytes[index] = writeCell(childPaths[index],
                                  oneEntityWorld(index + 2U, centers[index]));
  }
  const std::filesystem::path sentinelPath = temporary / "sentinel.vecooked";
  const std::uint64_t sentinelBytes =
      writeCell(sentinelPath, oneEntityWorld(6U, {10'000.0F, 0.0F, 0.0F}));

  ve::WorldPartitionManifest manifest;
  manifest.cells.push_back(
      {id(1U), {}, {}, 4'096.0F, 500.0F, rootPath, rootBytes, {}});
  for (std::size_t index = 0U; index < childPaths.size(); ++index) {
    manifest.cells.push_back({id(index + 2U),
                              id(1U),
                              centers[index],
                              2'048.0F,
                              0.0F,
                              childPaths[index],
                              childBytes[index],
                              {}});
  }
  manifest.cells.push_back({id(6U),
                            {},
                            {10'000.0F, 0.0F, 0.0F},
                            512.0F,
                            0.0F,
                            sentinelPath,
                            sentinelBytes,
                            {}});
  ve::validateWorldPartition(manifest);
  const std::vector<std::byte> encoded = ve::encodeWorldPartition(manifest);
  ve::WorldPartitionManifest reversed = manifest;
  std::ranges::reverse(reversed.cells);
  assert(ve::encodeWorldPartition(reversed) == encoded);
  const ve::WorldPartitionManifest decoded =
      ve::decodeWorldPartition(encoded, temporary);
  assert(ve::encodeWorldPartition(decoded) == encoded);
  const std::filesystem::path manifestPath = temporary / "world.vepartition";
  ve::saveWorldPartition(manifest, manifestPath);
  assert(ve::encodeWorldPartition(ve::loadWorldPartition(manifestPath)) ==
         encoded);

  ve::WorldPartitionManifest gap = manifest;
  gap.cells.erase(gap.cells.begin() + 4);
  assert(throws<std::runtime_error>([&] { ve::validateWorldPartition(gap); }));
  ve::WorldPartitionManifest overlap = manifest;
  overlap.cells[2].center = overlap.cells[1].center;
  assert(
      throws<std::runtime_error>([&] { ve::validateWorldPartition(overlap); }));
  ve::WorldPartitionManifest cycle = manifest;
  cycle.cells[0].parent = cycle.cells[1].id;
  assert(
      throws<std::runtime_error>([&] { ve::validateWorldPartition(cycle); }));
  std::vector<std::byte> trailing = encoded;
  trailing.push_back(std::byte{0});
  assert(throws<std::runtime_error>([&] {
    static_cast<void>(ve::decodeWorldPartition(trailing, temporary));
  }));

  const std::vector<std::byte> rootPayload = ve::readBinaryFile(rootPath);
  const std::array duplicatePayloads{std::span<const std::byte>{rootPayload},
                                     std::span<const std::byte>{rootPayload}};
  assert(throws<std::runtime_error>([&] {
    static_cast<void>(ve::combineCookedWorldCells(duplicatePayloads, {}));
  }));

  // Corrupt one fine cell before runtime registration. The coarse root must
  // remain active until the repaired four-cell frontier is complete.
  const std::array corrupt{std::byte{0x13}, std::byte{0x37}};
  ve::writeBinaryFileAtomic(childPaths[0], corrupt);

  ve::JobSystem jobs{{2U, 128U, 256U, 256U}};
  ve::ResidencyManager residency{jobs,
                                 {.maximumResources = 32U,
                                  .maximumQueuedRequests = 16U,
                                  .maximumConcurrentLoads = 2U,
                                  .residencyBudgetBytes = 4'096U,
                                  .maximumArtifactBytes = 1'024U}};
  ve::WorldPartitionRuntime runtime{residency,
                                    manifest,
                                    {.prefetchMargin = 100.0F,
                                     .originCellSize = 1'024.0F,
                                     .originShiftDistance = 1'024.0F}};

  std::uint64_t frame = 1U;
  const ve::PartitionPublication &initial =
      waitForPublication(runtime, {1'000.0F, 0.0F, 0.0F}, frame);
  assert(initial.cells == std::vector<ve::AssetId>({id(1U), id(6U)}));
  runtime.commitPublication(initial.revision);
  assert(runtime.activeCells().size() == 2U &&
         runtime.activeCells()[0] == id(1U) &&
         runtime.activeCells()[1] == id(6U));
  runtime.resetTraversalMetrics();

  for (std::size_t attempt = 0U;
       attempt < 100'000U && runtime.metrics().partialLoadFailures == 0U;
       ++attempt) {
    runtime.update({}, frame++);
    assert(runtime.activeCells().size() == 2U &&
           runtime.activeCells()[0] == id(1U) &&
           runtime.activeCells()[1] == id(6U));
    std::this_thread::yield();
  }
  assert(runtime.metrics().partialLoadFailures == 1U);
  assert(runtime.metrics().coverageGapFrames == 0U);
  assert(runtime.metrics().retainedFrontierFrames > 0U);

  childBytes[0] = writeCell(childPaths[0], oneEntityWorld(2U, centers[0]));
  runtime.retryRejectedFrontier();
  assert(residency.state({id(6U), ve::ResidencyClass::WorldCell}) ==
         ve::ResidencyState::Resident);
  const ve::PartitionPublication &detailed =
      waitForPublication(runtime, {}, frame);
  assert(detailed.cells.size() == 5U && detailed.world.identities.size() == 5U);
  assert(runtime.activeCells().size() == 2U);
  runtime.commitPublication(detailed.revision);
  assert(runtime.activeCells().size() == 5U);
  assert(runtime.metrics().coverageGapFrames == 0U);

  const ve::PartitionPublication &coarse =
      waitForPublication(runtime, {3'000.0F, 0.0F, 0.0F}, frame);
  assert(coarse.cells == std::vector<ve::AssetId>({id(1U), id(6U)}));
  assert(coarse.worldOrigin.x == 2'048.0F);
  assert(coarse.world.transforms[0].translation.x == -2'048.0F);
  const std::uint64_t rejectedRevision = coarse.revision;
  runtime.rejectPublication(rejectedRevision);
  assert(runtime.activeCells().size() == 5U);
  runtime.retryRejectedFrontier();
  assert(residency.state({id(6U), ve::ResidencyClass::WorldCell}) ==
         ve::ResidencyState::Resident);
  const ve::PartitionPublication &retried =
      waitForPublication(runtime, {3'000.0F, 0.0F, 0.0F}, frame);
  runtime.commitPublication(retried.revision);
  assert(runtime.activeCells().size() == 2U);
  assert(runtime.worldOrigin().x == 2'048.0F);
  assert(runtime.metrics().originShifts == 1U);
  assert(runtime.metrics().coverageGapFrames == 0U);
  assert(residency.metrics().mainThreadIoOperations == 0U);
  assert(residency.metrics().residentBytes <= 4'096U);

  std::error_code error;
  std::filesystem::remove_all(temporary, error);
  return 0;
}
