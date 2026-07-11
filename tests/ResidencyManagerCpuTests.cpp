#include "core/FileSystem.hpp"
#include "streaming/ResidencyManager.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

ve::ResidencyKey key(const std::uint64_t value,
                     const ve::ResidencyClass resourceClass) {
  return {{0x4d31524553494445ULL, value}, resourceClass};
}

std::filesystem::path artifact(const std::filesystem::path &root,
                               const std::string_view name,
                               const std::size_t bytes) {
  const std::filesystem::path path = root / name;
  std::vector<std::byte> payload(bytes);
  for (std::size_t index = 0U; index < bytes; ++index)
    payload[index] = static_cast<std::byte>((index * 17U + bytes) & 0xffU);
  ve::writeBinaryFileAtomic(path, payload);
  return path;
}

void waitFor(ve::ResidencyManager &manager, const ve::ResidencyKey resource,
             const ve::ResidencyState expected) {
  for (std::size_t attempt = 0U; attempt < 100'000U; ++attempt) {
    manager.update();
    const ve::ResidencyState state = manager.state(resource);
    if (state == expected)
      return;
    if (state == ve::ResidencyState::Failed &&
        expected != ve::ResidencyState::Failed) {
      throw std::runtime_error(std::string(manager.diagnostic(resource)));
    }
    std::this_thread::yield();
  }
  throw std::runtime_error("Residency test timed out");
}

} // namespace

int main() {
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path() /
      ("volkengine-residency-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(temporary);

  ve::JobSystem jobs{{2U, 64U, 128U, 128U}};
  ve::ResidencyManager manager{jobs,
                               {.maximumResources = 32U,
                                .maximumQueuedRequests = 16U,
                                .maximumConcurrentLoads = 2U,
                                .residencyBudgetBytes = 1'024U,
                                .maximumArtifactBytes = 512U}};

  const std::array resources{key(1U, ve::ResidencyClass::Texture),
                             key(2U, ve::ResidencyClass::Geometry),
                             key(3U, ve::ResidencyClass::Animation),
                             key(4U, ve::ResidencyClass::Audio),
                             key(5U, ve::ResidencyClass::WorldCell)};
  for (std::size_t index = 0U; index < resources.size(); ++index) {
    manager.registerResource(
        {resources[index],
         artifact(temporary, "class-" + std::to_string(index), 17U + index),
         17U + index,
         {},
         std::string(ve::residencyClassName(resources[index].resourceClass))});
  }
  // The cell exercises the same dependency expansion used for mixed streamed
  // content without duplicating the registered resource records.
  const ve::ResidencyKey mixedCell = key(6U, ve::ResidencyClass::WorldCell);
  manager.registerResource(
      {mixedCell,
       artifact(temporary, "mixed-cell", 29U),
       29U,
       {resources[0], resources[1], resources[2], resources[3]},
       "mixed-cell"});
  manager.beginFrame(1U);
  assert(manager.request(mixedCell, 100, true) ==
         ve::ResidencyRequestResult::Accepted);
  manager.endFrame();
  waitFor(manager, mixedCell, ve::ResidencyState::Resident);
  const ve::ResidencyMetrics mixedMetrics = manager.metrics();
  assert(mixedMetrics.resident == 5U);
  assert(mixedMetrics.residentByClass[static_cast<std::size_t>(
             ve::ResidencyClass::Texture)] == 1U);
  assert(mixedMetrics.residentByClass[static_cast<std::size_t>(
             ve::ResidencyClass::Geometry)] == 1U);
  assert(mixedMetrics.residentByClass[static_cast<std::size_t>(
             ve::ResidencyClass::Animation)] == 1U);
  assert(mixedMetrics.residentByClass[static_cast<std::size_t>(
             ve::ResidencyClass::Audio)] == 1U);
  assert(mixedMetrics.residentByClass[static_cast<std::size_t>(
             ve::ResidencyClass::WorldCell)] == 1U);
  assert(mixedMetrics.mainThreadIoOperations == 0U &&
         mixedMetrics.ioBytes == 103U);

  const ve::ResidencyKey missingDependency =
      key(7U, ve::ResidencyClass::WorldCell);
  manager.registerResource({missingDependency,
                            artifact(temporary, "missing-dependency", 4U),
                            4U,
                            {key(999U, ve::ResidencyClass::Geometry)},
                            "missing-dependency"});
  manager.beginFrame(2U);
  assert(manager.request(missingDependency, 10) ==
         ve::ResidencyRequestResult::Failed);
  assert(manager.failure(missingDependency) ==
         ve::ResidencyFailure::MissingDependency);

  const ve::ResidencyKey cycleA = key(8U, ve::ResidencyClass::Animation);
  const ve::ResidencyKey cycleB = key(9U, ve::ResidencyClass::Audio);
  manager.registerResource(
      {cycleA, artifact(temporary, "cycle-a", 4U), 4U, {cycleB}, "cycle-a"});
  manager.registerResource(
      {cycleB, artifact(temporary, "cycle-b", 4U), 4U, {cycleA}, "cycle-b"});
  assert(manager.request(cycleA, 10) == ve::ResidencyRequestResult::Failed);
  assert(manager.failure(cycleA) == ve::ResidencyFailure::MissingDependency ||
         manager.failure(cycleA) == ve::ResidencyFailure::DependencyCycle);

  ve::ResidencyManager bounded{jobs,
                               {.maximumResources = 8U,
                                .maximumQueuedRequests = 2U,
                                .maximumConcurrentLoads = 1U,
                                .residencyBudgetBytes = 8U,
                                .maximumArtifactBytes = 16U}};
  const ve::ResidencyKey oldResource = key(20U, ve::ResidencyClass::Texture);
  const ve::ResidencyKey newResource = key(21U, ve::ResidencyClass::Geometry);
  bounded.registerResource(
      {oldResource, artifact(temporary, "old", 6U), 6U, {}, "old"});
  bounded.registerResource(
      {newResource, artifact(temporary, "new", 6U), 6U, {}, "new"});
  bounded.beginFrame(1U);
  assert(bounded.request(oldResource, 1) ==
         ve::ResidencyRequestResult::Accepted);
  bounded.endFrame();
  waitFor(bounded, oldResource, ve::ResidencyState::Resident);
  bounded.beginFrame(2U);
  assert(bounded.request(newResource, 2, true) ==
         ve::ResidencyRequestResult::Accepted);
  bounded.endFrame();
  waitFor(bounded, newResource, ve::ResidencyState::Resident);
  assert(bounded.state(oldResource) == ve::ResidencyState::Unloaded);
  assert(bounded.metrics().evictions == 1U &&
         bounded.metrics().residentBytes == 6U);

  // A pinned frontier makes an otherwise valid completion explicitly OOM.
  bounded.retry(oldResource);
  bounded.beginFrame(3U);
  assert(bounded.request(newResource, 10, true) ==
         ve::ResidencyRequestResult::Resident);
  assert(bounded.request(oldResource, 9, true) ==
         ve::ResidencyRequestResult::Accepted);
  bounded.endFrame();
  waitFor(bounded, oldResource, ve::ResidencyState::Failed);
  assert(bounded.failure(oldResource) == ve::ResidencyFailure::OutOfMemory);

  ve::ResidencyManager pressure{jobs,
                                {.maximumResources = 4U,
                                 .maximumQueuedRequests = 1U,
                                 .maximumConcurrentLoads = 1U,
                                 .residencyBudgetBytes = 64U,
                                 .maximumArtifactBytes = 32U}};
  const ve::ResidencyKey low = key(30U, ve::ResidencyClass::Audio);
  const ve::ResidencyKey high = key(31U, ve::ResidencyClass::Animation);
  pressure.registerResource(
      {low, artifact(temporary, "low", 8U), 8U, {}, "low"});
  pressure.registerResource(
      {high, artifact(temporary, "high", 8U), 8U, {}, "high"});
  pressure.beginFrame(1U);
  assert(pressure.request(low, 1) == ve::ResidencyRequestResult::Accepted);
  assert(pressure.request(high, 100) ==
         ve::ResidencyRequestResult::Backpressured);
  assert(pressure.metrics().backpressureEvents == 1U);
  pressure.endFrame();
  waitFor(pressure, low, ve::ResidencyState::Resident);

  const ve::ResidencyKey missingFile = key(40U, ve::ResidencyClass::WorldCell);
  manager.registerResource(
      {missingFile, temporary / "not-present", 4U, {}, "missing-file"});
  manager.beginFrame(4U);
  assert(manager.request(missingFile, 1) ==
         ve::ResidencyRequestResult::Accepted);
  manager.endFrame();
  waitFor(manager, missingFile, ve::ResidencyState::Failed);
  assert(manager.failure(missingFile) == ve::ResidencyFailure::Io);
  const std::filesystem::path recoveredPath =
      artifact(temporary, "not-present", 4U);
  (void)recoveredPath;
  manager.retry(missingFile);
  manager.beginFrame(5U);
  assert(manager.request(missingFile, 1) ==
         ve::ResidencyRequestResult::Accepted);
  manager.endFrame();
  waitFor(manager, missingFile, ve::ResidencyState::Resident);

  // Destruction with outstanding IO owns cancellation, drain, and handle
  // release; task contexts cannot outlive this scope.
  {
    ve::ResidencyManager teardown{jobs,
                                  {.maximumResources = 2U,
                                   .maximumQueuedRequests = 2U,
                                   .maximumConcurrentLoads = 1U,
                                   .residencyBudgetBytes = 1'024U,
                                   .maximumArtifactBytes = 1'024U}};
    const ve::ResidencyKey pending = key(50U, ve::ResidencyClass::WorldCell);
    teardown.registerResource({pending,
                               artifact(temporary, "teardown", 1'000U),
                               1'000U,
                               {},
                               "teardown"});
    teardown.beginFrame(1U);
    assert(teardown.request(pending, 1) ==
           ve::ResidencyRequestResult::Accepted);
    teardown.endFrame();
  }
  assert(jobs.stats().activeJobs == 0U);

  std::error_code error;
  std::filesystem::remove_all(temporary, error);
  return 0;
}
