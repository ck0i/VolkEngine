#include "streaming/ResidencyManager.hpp"

#include "core/FileSystem.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ve {
namespace {

[[nodiscard]] bool terminal(const JobStatus status) noexcept {
  return status == JobStatus::Succeeded || status == JobStatus::Failed ||
         status == JobStatus::Cancelled;
}

[[nodiscard]] std::size_t classIndex(const ResidencyClass value) noexcept {
  return static_cast<std::size_t>(value);
}

} // namespace

struct ResidencyManager::LoadTask {
  std::filesystem::path path;
  std::size_t maximumBytes = 0U;
  std::uint64_t generation = 0U;
  std::vector<std::byte> bytes;
  std::string error;
  bool cancelled = false;

  static void run(void *context, JobContext &job) noexcept {
    auto &task = *static_cast<LoadTask *>(context);
    if (job.cancellationRequested()) {
      task.cancelled = true;
      return;
    }
    try {
      task.bytes = readBinaryFile(task.path, task.maximumBytes);
      if (job.cancellationRequested()) {
        task.bytes.clear();
        task.cancelled = true;
      }
    } catch (const std::exception &error) {
      task.error = error.what();
    } catch (...) {
      task.error = "Unknown residency IO failure";
    }
  }
};

struct ResidencyManager::Entry {
  ResidencyResourceDesc resource;
  ResidencyState state = ResidencyState::Unloaded;
  ResidencyFailure failure = ResidencyFailure::None;
  std::string diagnostic;
  std::vector<std::byte> payload;
  std::unique_ptr<LoadTask> task;
  JobHandle job;
  std::uint64_t generation = 1U;
  std::uint64_t lastUseFrame = 0U;
  std::int32_t priority = std::numeric_limits<std::int32_t>::min();
  bool desired = false;
  bool pinned = false;
};

ResidencyManager::ResidencyManager(JobSystem &jobs, ResidencyConfig config)
    : jobs_(&jobs), config_(config) {
  if (config_.maximumResources == 0U || config_.maximumQueuedRequests == 0U ||
      config_.maximumConcurrentLoads == 0U ||
      config_.residencyBudgetBytes == 0U ||
      config_.maximumArtifactBytes == 0U) {
    throw std::invalid_argument("Residency limits must be positive");
  }
  if (config_.maximumConcurrentLoads > config_.maximumQueuedRequests) {
    throw std::invalid_argument(
        "Concurrent residency loads exceed the request limit");
  }
  entries_.reserve(config_.maximumResources);
}

ResidencyManager::~ResidencyManager() {
  for (Entry &entry : entries_) {
    if (entry.job.valid() && !terminal(jobs_->status(entry.job))) {
      static_cast<void>(jobs_->cancel(entry.job));
    }
  }
  for (Entry &entry : entries_) {
    if (!entry.job.valid())
      continue;
    try {
      static_cast<void>(jobs_->wait(entry.job));
    } catch (...) {
    }
    jobs_->release(entry.job);
    entry.job = {};
    entry.task.reset();
  }
}

void ResidencyManager::registerResource(ResidencyResourceDesc resource) {
  if (!resource.key.valid() ||
      resource.key.resourceClass >= ResidencyClass::Count) {
    throw std::invalid_argument("Residency resource identity is invalid");
  }
  if (resource.artifactPath.empty()) {
    throw std::invalid_argument("Residency artifact path is empty");
  }
  if (resource.name.empty()) {
    throw std::invalid_argument("Residency resource name is empty");
  }
  if (find(resource.key) != nullptr) {
    throw std::invalid_argument("Residency resource identity is duplicated");
  }
  if (entries_.size() == config_.maximumResources) {
    throw std::overflow_error("Residency resource capacity is exhausted");
  }
  std::ranges::sort(resource.dependencies);
  if (std::ranges::adjacent_find(resource.dependencies) !=
      resource.dependencies.end()) {
    throw std::invalid_argument("Residency dependency is duplicated");
  }
  if (std::ranges::find(resource.dependencies, resource.key) !=
      resource.dependencies.end()) {
    throw std::invalid_argument("Residency resource depends on itself");
  }
  entries_.push_back({std::move(resource)});
}

void ResidencyManager::beginFrame(const std::uint64_t frameIndex) {
  if (frameIndex < frameIndex_) {
    throw std::invalid_argument("Residency frame index moved backwards");
  }
  frameIndex_ = frameIndex;
  for (Entry &entry : entries_) {
    entry.desired = false;
    entry.pinned = false;
    entry.priority = std::numeric_limits<std::int32_t>::min();
  }
}

ResidencyRequestResult ResidencyManager::request(const ResidencyKey key,
                                                 const std::int32_t priority,
                                                 const bool pin) {
  Entry *entry = find(key);
  if (entry == nullptr) {
    ++metrics_.missingDependencyFailures;
    return ResidencyRequestResult::Missing;
  }
  std::vector<ResidencyKey> path;
  path.reserve(16U);
  const ResidencyRequestResult result =
      requestRecursive(*entry, priority, pin, path);
  refreshMetrics();
  return result;
}

ResidencyRequestResult
ResidencyManager::requestRecursive(Entry &entry, const std::int32_t priority,
                                   const bool pin,
                                   std::vector<ResidencyKey> &path) {
  if (std::ranges::find(path, entry.resource.key) != path.end()) {
    fail(entry, ResidencyFailure::DependencyCycle,
         "Residency dependency graph contains a cycle");
    ++metrics_.missingDependencyFailures;
    return ResidencyRequestResult::Failed;
  }
  if (entry.state == ResidencyState::Failed) {
    return ResidencyRequestResult::Failed;
  }
  entry.desired = true;
  entry.pinned = entry.pinned || pin;
  entry.priority = std::max(entry.priority, priority);
  entry.lastUseFrame = frameIndex_;
  if (entry.state == ResidencyState::Resident) {
    return ResidencyRequestResult::Resident;
  }

  path.push_back(entry.resource.key);
  for (const ResidencyKey dependencyKey : entry.resource.dependencies) {
    Entry *dependency = find(dependencyKey);
    if (dependency == nullptr) {
      path.pop_back();
      fail(entry, ResidencyFailure::MissingDependency,
           "Residency resource has an unregistered dependency");
      ++metrics_.missingDependencyFailures;
      return ResidencyRequestResult::Failed;
    }
    const ResidencyRequestResult dependencyResult =
        requestRecursive(*dependency, priority, pin, path);
    if (dependencyResult == ResidencyRequestResult::Failed ||
        dependencyResult == ResidencyRequestResult::Missing) {
      path.pop_back();
      fail(entry, ResidencyFailure::MissingDependency,
           "Residency dependency failed");
      ++metrics_.missingDependencyFailures;
      return ResidencyRequestResult::Failed;
    }
  }
  path.pop_back();

  if (entry.state == ResidencyState::Unloaded) {
    std::size_t queuedOrLoading = 0U;
    for (const Entry &candidate : entries_) {
      queuedOrLoading += candidate.state == ResidencyState::Queued ||
                         candidate.state == ResidencyState::Loading;
    }
    if (queuedOrLoading >= config_.maximumQueuedRequests) {
      ++metrics_.backpressureEvents;
      return ResidencyRequestResult::Backpressured;
    }
    if (entry.resource.estimatedBytes > config_.residencyBudgetBytes) {
      fail(entry, ResidencyFailure::OutOfMemory,
           "Residency estimate exceeds the complete budget");
      ++metrics_.outOfMemoryFailures;
      return ResidencyRequestResult::Failed;
    }
    if (entry.resource.estimatedBytes > config_.maximumArtifactBytes) {
      fail(entry, ResidencyFailure::ArtifactTooLarge,
           "Residency estimate exceeds the artifact limit");
      ++metrics_.ioFailures;
      return ResidencyRequestResult::Failed;
    }
    entry.state = ResidencyState::Queued;
  }
  return ResidencyRequestResult::Accepted;
}

void ResidencyManager::endFrame() {
  for (Entry &entry : entries_) {
    if (entry.desired)
      continue;
    if (entry.state == ResidencyState::Queued) {
      ++entry.generation;
      entry.state = ResidencyState::Unloaded;
      entry.failure = ResidencyFailure::Cancelled;
      entry.diagnostic = "Residency request was cancelled before dispatch";
      ++metrics_.cancellations;
    } else if (entry.state == ResidencyState::Loading && entry.job.valid()) {
      ++entry.generation;
      static_cast<void>(jobs_->cancel(entry.job));
      ++metrics_.cancellations;
    }
  }
  update();
}

void ResidencyManager::update() {
  publishCompleted();
  scheduleQueued();
  refreshMetrics();
}

void ResidencyManager::publishCompleted() {
  for (Entry &entry : entries_) {
    if (entry.state != ResidencyState::Loading || !entry.job.valid())
      continue;
    const JobStatus status = jobs_->status(entry.job);
    if (!terminal(status))
      continue;

    jobs_->release(entry.job);
    entry.job = {};
    std::unique_ptr<LoadTask> task = std::move(entry.task);
    if (task->generation != entry.generation) {
      ++metrics_.staleCompletions;
      entry.state =
          entry.desired ? ResidencyState::Queued : ResidencyState::Unloaded;
      entry.failure =
          entry.desired ? ResidencyFailure::None : ResidencyFailure::Cancelled;
      entry.diagnostic = entry.desired ? std::string{}
                                       : "Stale residency completion discarded";
      continue;
    }
    if (status == JobStatus::Cancelled || task->cancelled) {
      entry.state =
          entry.desired ? ResidencyState::Queued : ResidencyState::Unloaded;
      entry.failure = ResidencyFailure::Cancelled;
      entry.diagnostic = "Residency IO was cancelled";
      continue;
    }
    if (!task->error.empty()) {
      fail(entry, ResidencyFailure::Io, std::move(task->error));
      ++metrics_.ioFailures;
      continue;
    }
    if (!makeRoom(task->bytes.size(), &entry)) {
      fail(entry, ResidencyFailure::OutOfMemory,
           "Residency budget cannot admit the completed artifact");
      ++metrics_.outOfMemoryFailures;
      continue;
    }
    metrics_.ioBytes += task->bytes.size();
    metrics_.residentBytes += task->bytes.size();
    metrics_.peakResidentBytes =
        std::max(metrics_.peakResidentBytes, metrics_.residentBytes);
    entry.payload = std::move(task->bytes);
    entry.state = ResidencyState::Resident;
    entry.failure = ResidencyFailure::None;
    entry.diagnostic.clear();
    entry.lastUseFrame = frameIndex_;
    ++metrics_.publishedLoads;
  }
}

void ResidencyManager::scheduleQueued() {
  std::size_t active = 0U;
  for (const Entry &entry : entries_) {
    active += entry.state == ResidencyState::Loading;
  }

  while (active < config_.maximumConcurrentLoads) {
    Entry *selected = nullptr;
    for (Entry &candidate : entries_) {
      if (candidate.state != ResidencyState::Queued || !candidate.desired)
        continue;
      bool dependenciesReady = true;
      for (const ResidencyKey dependencyKey : candidate.resource.dependencies) {
        const Entry *dependency = find(dependencyKey);
        if (dependency == nullptr ||
            dependency->state == ResidencyState::Failed) {
          fail(candidate, ResidencyFailure::MissingDependency,
               "Residency dependency became unavailable");
          ++metrics_.missingDependencyFailures;
          dependenciesReady = false;
          break;
        }
        if (dependency->state != ResidencyState::Resident) {
          dependenciesReady = false;
          break;
        }
      }
      if (!dependenciesReady || candidate.state == ResidencyState::Failed)
        continue;
      if (selected == nullptr || candidate.priority > selected->priority ||
          (candidate.priority == selected->priority &&
           candidate.resource.key < selected->resource.key)) {
        selected = &candidate;
      }
    }
    if (selected == nullptr)
      break;

    selected->task = std::make_unique<LoadTask>();
    selected->task->path = selected->resource.artifactPath;
    selected->task->maximumBytes = config_.maximumArtifactBytes;
    selected->task->generation = selected->generation;
    try {
      selected->job = jobs_->submit({selected->resource.name,
                                     LoadTask::run,
                                     selected->task.get(),
                                     {},
                                     JobCategory::Io});
    } catch (const std::runtime_error &) {
      selected->task.reset();
      ++metrics_.backpressureEvents;
      break;
    }
    selected->state = ResidencyState::Loading;
    ++active;
  }

  bool dispatchableQueued = false;
  for (const Entry &entry : entries_) {
    if (entry.state == ResidencyState::Queued && entry.desired) {
      dispatchableQueued = true;
      break;
    }
  }
  if (dispatchableQueued && active >= config_.maximumConcurrentLoads) {
    ++metrics_.backpressureEvents;
  }
}

bool ResidencyManager::makeRoom(const std::uint64_t bytes,
                                const Entry *protectedEntry) {
  if (bytes > config_.residencyBudgetBytes)
    return false;
  while (metrics_.residentBytes > config_.residencyBudgetBytes - bytes) {
    Entry *victim = nullptr;
    for (Entry &candidate : entries_) {
      if (&candidate == protectedEntry ||
          candidate.state != ResidencyState::Resident || candidate.pinned)
        continue;
      if (victim == nullptr || candidate.desired < victim->desired ||
          (candidate.desired == victim->desired &&
           candidate.priority < victim->priority) ||
          (candidate.desired == victim->desired &&
           candidate.priority == victim->priority &&
           candidate.lastUseFrame < victim->lastUseFrame) ||
          (candidate.desired == victim->desired &&
           candidate.priority == victim->priority &&
           candidate.lastUseFrame == victim->lastUseFrame &&
           candidate.resource.key < victim->resource.key)) {
        victim = &candidate;
      }
    }
    if (victim == nullptr)
      return false;
    evictEntry(*victim);
  }
  return true;
}

void ResidencyManager::evict(const ResidencyKey key) {
  Entry *entry = find(key);
  if (entry == nullptr)
    throw std::invalid_argument("Cannot evict an unknown residency resource");
  if (entry->state == ResidencyState::Loading) {
    ++entry->generation;
    static_cast<void>(jobs_->cancel(entry->job));
    entry->desired = false;
    return;
  }
  if (entry->state == ResidencyState::Queued) {
    ++entry->generation;
    entry->state = ResidencyState::Unloaded;
    entry->desired = false;
    return;
  }
  if (entry->state == ResidencyState::Resident)
    evictEntry(*entry);
}

void ResidencyManager::evictEntry(Entry &entry) {
  entry.state = ResidencyState::Evicting;
  metrics_.residentBytes -= entry.payload.size();
  std::vector<std::byte>{}.swap(entry.payload);
  ++entry.generation;
  entry.state = ResidencyState::Unloaded;
  entry.failure = ResidencyFailure::None;
  entry.diagnostic.clear();
  ++metrics_.evictions;
}

void ResidencyManager::retry(const ResidencyKey key) {
  Entry *entry = find(key);
  if (entry == nullptr)
    throw std::invalid_argument("Cannot retry an unknown residency resource");
  if (entry->state == ResidencyState::Loading ||
      entry->state == ResidencyState::Queued) {
    throw std::logic_error("Cannot retry an active residency resource");
  }
  if (entry->state == ResidencyState::Resident)
    return;
  ++entry->generation;
  entry->state = ResidencyState::Unloaded;
  entry->failure = ResidencyFailure::None;
  entry->diagnostic.clear();
}

void ResidencyManager::fail(Entry &entry, const ResidencyFailure failureValue,
                            std::string diagnosticValue) {
  entry.state = ResidencyState::Failed;
  entry.failure = failureValue;
  entry.diagnostic = std::move(diagnosticValue);
  entry.payload.clear();
}

ResidencyManager::Entry *
ResidencyManager::find(const ResidencyKey key) noexcept {
  for (Entry &entry : entries_) {
    if (entry.resource.key == key)
      return &entry;
  }
  return nullptr;
}

const ResidencyManager::Entry *
ResidencyManager::find(const ResidencyKey key) const noexcept {
  for (const Entry &entry : entries_) {
    if (entry.resource.key == key)
      return &entry;
  }
  return nullptr;
}

ResidencyState ResidencyManager::state(const ResidencyKey key) const noexcept {
  const Entry *entry = find(key);
  return entry == nullptr ? ResidencyState::Failed : entry->state;
}

ResidencyFailure
ResidencyManager::failure(const ResidencyKey key) const noexcept {
  const Entry *entry = find(key);
  return entry == nullptr ? ResidencyFailure::MissingResource : entry->failure;
}

std::string_view
ResidencyManager::diagnostic(const ResidencyKey key) const noexcept {
  const Entry *entry = find(key);
  return entry == nullptr ? std::string_view{"Unknown residency resource"}
                          : std::string_view{entry->diagnostic};
}

std::span<const std::byte>
ResidencyManager::payload(const ResidencyKey key) const noexcept {
  const Entry *entry = find(key);
  return entry != nullptr && entry->state == ResidencyState::Resident
             ? std::span<const std::byte>{entry->payload}
             : std::span<const std::byte>{};
}

void ResidencyManager::refreshMetrics() noexcept {
  metrics_.queued = 0U;
  metrics_.loading = 0U;
  metrics_.resident = 0U;
  metrics_.residentByClass.fill(0U);
  metrics_.bytesByClass.fill(0U);
  std::uint64_t residentBytes = 0U;
  for (const Entry &entry : entries_) {
    metrics_.queued += entry.state == ResidencyState::Queued;
    metrics_.loading += entry.state == ResidencyState::Loading;
    if (entry.state != ResidencyState::Resident)
      continue;
    ++metrics_.resident;
    const std::size_t index = classIndex(entry.resource.key.resourceClass);
    ++metrics_.residentByClass[index];
    metrics_.bytesByClass[index] += entry.payload.size();
    residentBytes += entry.payload.size();
  }
  metrics_.residentBytes = residentBytes;
  metrics_.peakResidentBytes =
      std::max(metrics_.peakResidentBytes, metrics_.residentBytes);
}

ResidencyMetrics ResidencyManager::metrics() const noexcept { return metrics_; }

std::size_t ResidencyManager::resourceCount() const noexcept {
  return entries_.size();
}

std::string_view residencyClassName(const ResidencyClass value) noexcept {
  switch (value) {
  case ResidencyClass::Texture:
    return "texture";
  case ResidencyClass::Geometry:
    return "geometry";
  case ResidencyClass::WorldCell:
    return "world-cell";
  case ResidencyClass::Animation:
    return "animation";
  case ResidencyClass::Audio:
    return "audio";
  case ResidencyClass::Count:
    break;
  }
  return "unknown";
}

std::string_view residencyFailureName(const ResidencyFailure value) noexcept {
  switch (value) {
  case ResidencyFailure::None:
    return "none";
  case ResidencyFailure::MissingResource:
    return "missing-resource";
  case ResidencyFailure::MissingDependency:
    return "missing-dependency";
  case ResidencyFailure::DependencyCycle:
    return "dependency-cycle";
  case ResidencyFailure::Io:
    return "io";
  case ResidencyFailure::ArtifactTooLarge:
    return "artifact-too-large";
  case ResidencyFailure::OutOfMemory:
    return "out-of-memory";
  case ResidencyFailure::Backpressure:
    return "backpressure";
  case ResidencyFailure::Cancelled:
    return "cancelled";
  }
  return "unknown";
}

} // namespace ve
