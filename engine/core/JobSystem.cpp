#include "core/JobSystem.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace ve {
namespace {

[[nodiscard]] bool terminal(const JobStatus status) noexcept {
  return status == JobStatus::Succeeded || status == JobStatus::Failed ||
         status == JobStatus::Cancelled;
}

[[nodiscard]] std::uint64_t monotonicNanoseconds() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

thread_local JobSystem *workerSystem = nullptr;
thread_local std::uint32_t workerIndex = UINT32_MAX;

} // namespace

struct JobSystem::Impl {
  static constexpr std::uint32_t kInvalid = UINT32_MAX;

  struct Slot {
    JobCallback callback = nullptr;
    void *context = nullptr;
    std::exception_ptr failure;
    std::atomic<bool> cancellationRequested{false};
    JobStatus status = JobStatus::Cancelled;
    JobCategory category = JobCategory::General;
    std::uint32_t generation = 1;
    std::uint32_t remainingDependencies = 0;
    std::uint32_t firstDependent = kInvalid;
    std::uint32_t assignedWorker = 0;
    std::uint64_t queuedNanoseconds = 0;
    std::uint64_t startedNanoseconds = 0;
    char name[JobTimelineEvent::kMaximumNameBytes + 1]{};
  };

  struct Edge {
    std::uint32_t dependent = kInvalid;
    std::uint32_t next = kInvalid;
  };

  struct RingQueue {
    explicit RingQueue(const std::uint32_t capacity)
        : values(capacity, kInvalid) {}

    [[nodiscard]] bool pushBack(const std::uint32_t value) noexcept {
      if (count == values.size())
        return false;
      values[(head + count) % values.size()] = value;
      ++count;
      return true;
    }
    [[nodiscard]] std::uint32_t popBack() noexcept {
      if (count == 0)
        return kInvalid;
      const std::size_t position = (head + count - 1U) % values.size();
      const std::uint32_t result = values[position];
      --count;
      return result;
    }
    [[nodiscard]] std::uint32_t popFront() noexcept {
      if (count == 0)
        return kInvalid;
      const std::uint32_t result = values[head];
      head = (head + 1U) % values.size();
      --count;
      return result;
    }

    std::vector<std::uint32_t> values;
    std::size_t head = 0;
    std::size_t count = 0;
  };

  struct Worker {
    explicit Worker(const std::uint32_t capacity) : queue(capacity) {}
    RingQueue queue;
    std::thread thread;
  };
  Impl(JobSystem &ownerValue, const Config &config)
      : owner(ownerValue), maximumJobs(config.maximumJobs),
        maximumDependencies(config.maximumDependencies),
        timelineCapacity(config.timelineCapacity),
        slots(std::make_unique<Slot[]>(maximumJobs)),
        edges(std::make_unique<Edge[]>(maximumDependencies)),
        freeSlots(maximumJobs), freeEdges(maximumDependencies),
        propagation(maximumJobs), timelineEvents(timelineCapacity) {
    if (maximumJobs == 0 || maximumDependencies == 0 || timelineCapacity == 0) {
      throw std::invalid_argument("JobSystem capacities must be positive");
    }
    const std::uint32_t hardware =
        std::max(1U, std::thread::hardware_concurrency());
    const std::uint32_t requested = config.workerCount == 0
                                        ? std::max(1U, hardware - 1U)
                                        : config.workerCount;
    if (requested > 256U) {
      throw std::invalid_argument("JobSystem worker count exceeds 256");
    }
    workers.reserve(requested);
    for (std::uint32_t index = 0; index < requested; ++index) {
      workers.emplace_back(maximumJobs);
    }
    for (std::uint32_t index = 0; index < maximumJobs; ++index) {
      freeSlots[index] = maximumJobs - index - 1U;
    }
    freeSlotCount = maximumJobs;
    for (std::uint32_t index = 0; index < maximumDependencies; ++index) {
      freeEdges[index] = maximumDependencies - index - 1U;
    }
    freeEdgeCount = maximumDependencies;
    aggregate.workerCount = requested;
    try {
      for (std::uint32_t index = 0; index < requested; ++index) {
        workers[index].thread =
            std::thread([this, index] { workerLoop(index); });
      }
    } catch (...) {
      stopping = true;
      workAvailable.notify_all();
      for (Worker &worker : workers) {
        if (worker.thread.joinable())
          worker.thread.join();
      }
      throw;
    }
  }

  ~Impl() = default;

  [[nodiscard]] bool validUnlocked(const JobHandle handle) const noexcept {
    return handle.index < maximumJobs && handle.generation != 0 &&
           slots[handle.index].generation == handle.generation &&
           slots[handle.index].callback != nullptr;
  }

  [[nodiscard]] JobHandle handleFor(const std::uint32_t index) const noexcept {
    return JobHandle{index, slots[index].generation};
  }

  [[nodiscard]] std::uint32_t allocateEdge() {
    if (freeEdgeCount == 0) {
      throw std::runtime_error("JobSystem dependency capacity exhausted");
    }
    return freeEdges[--freeEdgeCount];
  }

  void releaseEdge(const std::uint32_t index) noexcept {
    edges[index] = {};
    freeEdges[freeEdgeCount++] = index;
  }

  void enqueueUnlocked(const std::uint32_t slotIndex) {
    const std::uint32_t target =
        nextWorker++ % static_cast<std::uint32_t>(workers.size());
    Slot &slot = slots[slotIndex];
    slot.assignedWorker = target;
    slot.status = JobStatus::Ready;
    if (!workers[target].queue.pushBack(slotIndex)) {
      throw std::runtime_error("JobSystem ready queue capacity exhausted");
    }
    std::uint32_t queued = 0;
    for (const Worker &worker : workers) {
      queued += static_cast<std::uint32_t>(worker.queue.count);
    }
    aggregate.queueHighWatermark =
        std::max(aggregate.queueHighWatermark, queued);
    workAvailable.notify_one();
  }

  [[nodiscard]] std::uint32_t
  takeWorkUnlocked(const std::uint32_t requester) noexcept {
    if (const std::uint32_t own = workers[requester].queue.popBack();
        own != kInvalid) {
      return own;
    }
    for (std::uint32_t offset = 1; offset < workers.size(); ++offset) {
      const std::uint32_t victim =
          (requester + offset) % static_cast<std::uint32_t>(workers.size());
      if (const std::uint32_t stolen = workers[victim].queue.popFront();
          stolen != kInvalid) {
        ++aggregate.steals;
        return stolen;
      }
    }
    return kInvalid;
  }

  void appendTimelineUnlocked(const std::uint32_t slotIndex,
                              const std::uint32_t completedWorker,
                              const std::uint64_t finished) noexcept {
    if (timelineEvents.empty())
      return;
    JobTimelineEvent event{};
    const Slot &slot = slots[slotIndex];
    event.handle = handleFor(slotIndex);
    event.status = slot.status;
    event.category = slot.category;
    event.workerIndex = completedWorker;
    event.queuedNanoseconds = slot.queuedNanoseconds;
    event.startedNanoseconds = slot.startedNanoseconds;
    event.finishedNanoseconds = finished;
    std::memcpy(event.name, slot.name, sizeof(event.name));
    const std::size_t position =
        (timelineBegin + timelineCount) % timelineEvents.size();
    timelineEvents[position] = event;
    if (timelineCount < timelineEvents.size()) {
      ++timelineCount;
    } else {
      timelineBegin = (timelineBegin + 1U) % timelineEvents.size();
    }
  }

  void makeTerminalUnlocked(const std::uint32_t initial,
                            const JobStatus initialStatus,
                            const std::uint32_t completedWorker,
                            const std::uint64_t finished) {
    Slot &first = slots[initial];
    first.status = initialStatus;
    appendTimelineUnlocked(initial, completedWorker, finished);
    --aggregate.activeJobs;
    const std::size_t category = static_cast<std::size_t>(first.category);
    switch (initialStatus) {
    case JobStatus::Succeeded:
      ++aggregate.succeeded;
      ++aggregate.categorySucceeded[category];
      break;
    case JobStatus::Failed:
      ++aggregate.failed;
      ++aggregate.categoryFailed[category];
      break;
    case JobStatus::Cancelled:
      ++aggregate.cancelled;
      ++aggregate.categoryCancelled[category];
      break;
    default:
      break;
    }

    std::size_t propagationCount = 0;
    propagation[propagationCount++] = initial;
    while (propagationCount > 0) {
      const std::uint32_t parentIndex = propagation[--propagationCount];
      Slot &parent = slots[parentIndex];
      std::uint32_t edgeIndex = parent.firstDependent;
      parent.firstDependent = kInvalid;
      while (edgeIndex != kInvalid) {
        const Edge edge = edges[edgeIndex];
        releaseEdge(edgeIndex);
        edgeIndex = edge.next;

        Slot &dependent = slots[edge.dependent];
        if (dependent.remainingDependencies > 0) {
          --dependent.remainingDependencies;
        }
        if (terminal(dependent.status) ||
            dependent.status == JobStatus::Running) {
          continue;
        }
        if (parent.status != JobStatus::Succeeded) {
          dependent.cancellationRequested.store(true,
                                                std::memory_order_relaxed);
          dependent.status = JobStatus::Cancelled;
          appendTimelineUnlocked(edge.dependent, kInvalid, finished);
          --aggregate.activeJobs;
          ++aggregate.cancelled;
          ++aggregate.categoryCancelled[static_cast<std::size_t>(
              dependent.category)];
          propagation[propagationCount++] = edge.dependent;
        } else if (dependent.remainingDependencies == 0) {
          enqueueUnlocked(edge.dependent);
        }
      }
    }
    completed.notify_all();
  }

  [[nodiscard]] bool runOne(const std::uint32_t requester) {
    std::uint32_t slotIndex = kInvalid;
    {
      std::lock_guard lock(mutex);
      slotIndex = takeWorkUnlocked(requester);
      if (slotIndex == kInvalid)
        return false;
      Slot &slot = slots[slotIndex];
      if (slot.status != JobStatus::Ready)
        return true;
      slot.status = JobStatus::Running;
      slot.startedNanoseconds = monotonicNanoseconds();
      ++aggregate.runningJobs;
    }

    JobStatus result = JobStatus::Succeeded;
    std::exception_ptr failure;
    try {
      JobContext context{owner, handleFor(slotIndex)};
      slots[slotIndex].callback(slots[slotIndex].context, context);
      if (slots[slotIndex].cancellationRequested.load(
              std::memory_order_relaxed)) {
        result = JobStatus::Cancelled;
      }
    } catch (...) {
      failure = std::current_exception();
      result = JobStatus::Failed;
    }
    const std::uint64_t finished = monotonicNanoseconds();
    {
      std::lock_guard lock(mutex);
      Slot &slot = slots[slotIndex];
      slot.failure = failure;
      --aggregate.runningJobs;
      aggregate.executedNanoseconds += finished - slot.startedNanoseconds;
      aggregate.categoryNanoseconds[static_cast<std::size_t>(slot.category)] +=
          finished - slot.startedNanoseconds;
      makeTerminalUnlocked(slotIndex, result, requester, finished);
    }
    return true;
  }

  void workerLoop(const std::uint32_t index) {
    workerSystem = &owner;
    workerIndex = index;
    for (;;) {
      if (runOne(index))
        continue;
      std::unique_lock lock(mutex);
      workAvailable.wait(lock, [this, index] {
        if (stopping)
          return true;
        if (workers[index].queue.count != 0)
          return true;
        return std::ranges::any_of(workers, [](const Worker &worker) {
          return worker.queue.count != 0;
        });
      });
      if (stopping && aggregate.activeJobs == 0)
        break;
    }
    workerIndex = kInvalid;
    workerSystem = nullptr;
  }

  JobSystem &owner;
  const std::uint32_t maximumJobs;
  const std::uint32_t maximumDependencies;
  const std::uint32_t timelineCapacity;
  std::unique_ptr<Slot[]> slots;
  std::unique_ptr<Edge[]> edges;
  std::vector<std::uint32_t> freeSlots;
  std::vector<std::uint32_t> freeEdges;
  std::vector<std::uint32_t> propagation;
  std::vector<JobTimelineEvent> timelineEvents;
  std::vector<Worker> workers;
  mutable std::mutex mutex;
  std::condition_variable workAvailable;
  std::condition_variable completed;
  JobSystemStats aggregate{};
  std::uint32_t freeSlotCount = 0;
  std::uint32_t freeEdgeCount = 0;
  std::uint32_t nextWorker = 0;
  std::size_t timelineBegin = 0;
  std::size_t timelineCount = 0;
  bool accepting = true;
  bool stopping = false;
};

JobContext::JobContext(JobSystem &system, const JobHandle current) noexcept
    : system_(&system), current_(current) {}

bool JobContext::cancellationRequested() const noexcept {
  return system_->cancellationRequested(current_);
}

JobStatus JobContext::wait(const JobHandle handle) {
  return system_->waitFrom(handle, current_);
}

JobSystem::JobSystem(const Config config)
    : impl_(std::make_unique<Impl>(*this, config)) {}

JobSystem::~JobSystem() {
  if (impl_)
    shutdown(JobShutdownMode::Cancel);
}

JobHandle JobSystem::submit(const JobDesc &descriptor) {
  if (descriptor.name.empty())
    throw std::invalid_argument("Job name must not be empty");
  if (descriptor.callback == nullptr)
    throw std::invalid_argument("Job callback must not be null");
  if (descriptor.name.size() > JobTimelineEvent::kMaximumNameBytes) {
    throw std::invalid_argument("Job name exceeds fixed timeline capacity");
  }
  if (static_cast<std::size_t>(descriptor.category) >= kJobCategoryCount) {
    throw std::invalid_argument("Job category is invalid");
  }

  std::lock_guard lock(impl_->mutex);
  if (!impl_->accepting)
    throw std::logic_error("JobSystem is shutting down");
  if (impl_->freeSlotCount == 0)
    throw std::runtime_error("JobSystem job capacity exhausted");
  if (descriptor.dependencies.size() > impl_->freeEdgeCount) {
    throw std::runtime_error("JobSystem dependency capacity exhausted");
  }
  for (std::size_t index = 0; index < descriptor.dependencies.size(); ++index) {
    if (!impl_->validUnlocked(descriptor.dependencies[index])) {
      throw std::invalid_argument("Job dependency handle is stale or invalid");
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (descriptor.dependencies[prior] == descriptor.dependencies[index]) {
        throw std::invalid_argument("Job dependencies must be unique");
      }
    }
  }

  const std::uint32_t slotIndex = impl_->freeSlots[--impl_->freeSlotCount];
  Impl::Slot &slot = impl_->slots[slotIndex];
  slot.callback = descriptor.callback;
  slot.context = descriptor.context;
  slot.failure = nullptr;
  slot.cancellationRequested.store(false, std::memory_order_relaxed);
  slot.status = JobStatus::Waiting;
  slot.category = descriptor.category;
  slot.remainingDependencies = 0;
  slot.firstDependent = Impl::kInvalid;
  slot.queuedNanoseconds = monotonicNanoseconds();
  slot.startedNanoseconds = 0;
  std::memset(slot.name, 0, sizeof(slot.name));
  std::memcpy(slot.name, descriptor.name.data(), descriptor.name.size());
  ++impl_->aggregate.submitted;
  ++impl_->aggregate
        .categorySubmitted[static_cast<std::size_t>(descriptor.category)];
  ++impl_->aggregate.activeJobs;

  bool cancelledByDependency = false;
  for (const JobHandle dependency : descriptor.dependencies) {
    const Impl::Slot &parent = impl_->slots[dependency.index];
    if (parent.status == JobStatus::Succeeded)
      continue;
    if (parent.status == JobStatus::Failed ||
        parent.status == JobStatus::Cancelled) {
      cancelledByDependency = true;
      continue;
    }
    const std::uint32_t edgeIndex = impl_->allocateEdge();
    impl_->edges[edgeIndex] = {slotIndex,
                               impl_->slots[dependency.index].firstDependent};
    impl_->slots[dependency.index].firstDependent = edgeIndex;
    ++slot.remainingDependencies;
  }

  if (cancelledByDependency) {
    slot.cancellationRequested.store(true, std::memory_order_relaxed);
    impl_->makeTerminalUnlocked(slotIndex, JobStatus::Cancelled, Impl::kInvalid,
                                monotonicNanoseconds());
  } else if (slot.remainingDependencies == 0) {
    impl_->enqueueUnlocked(slotIndex);
  }
  return impl_->handleFor(slotIndex);
}

JobStatus JobSystem::status(const JobHandle handle) const {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->validUnlocked(handle))
    throw std::invalid_argument("Job handle is stale or invalid");
  return impl_->slots[handle.index].status;
}

bool JobSystem::cancellationRequested(const JobHandle handle) const noexcept {
  if (!handle.valid() || handle.index >= impl_->maximumJobs)
    return true;
  const Impl::Slot &slot = impl_->slots[handle.index];
  if (slot.generation != handle.generation || slot.callback == nullptr)
    return true;
  return slot.cancellationRequested.load(std::memory_order_relaxed);
}

bool JobSystem::cancel(const JobHandle handle) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->validUnlocked(handle))
    throw std::invalid_argument("Job handle is stale or invalid");
  Impl::Slot &slot = impl_->slots[handle.index];
  if (terminal(slot.status))
    return false;
  slot.cancellationRequested.store(true, std::memory_order_relaxed);
  if (slot.status == JobStatus::Running)
    return true;
  impl_->makeTerminalUnlocked(handle.index, JobStatus::Cancelled,
                              Impl::kInvalid, monotonicNanoseconds());
  return true;
}

JobStatus JobSystem::waitFrom(const JobHandle handle, const JobHandle current) {
  if (handle == current)
    throw std::logic_error("A job cannot wait on itself");
  for (;;) {
    std::exception_ptr failure;
    JobStatus result;
    {
      std::unique_lock lock(impl_->mutex);
      if (!impl_->validUnlocked(handle))
        throw std::invalid_argument("Job handle is stale or invalid");
      result = impl_->slots[handle.index].status;
      if (terminal(result))
        failure = impl_->slots[handle.index].failure;
      if (!terminal(result) && workerSystem != this) {
        impl_->completed.wait(lock, [&] {
          return !impl_->validUnlocked(handle) ||
                 terminal(impl_->slots[handle.index].status);
        });
        continue;
      }
    }
    if (terminal(result)) {
      if (failure)
        std::rethrow_exception(failure);
      return result;
    }
    if (!impl_->runOne(workerIndex)) {
      std::unique_lock lock(impl_->mutex);
      impl_->completed.wait_for(lock, std::chrono::microseconds(100));
    }
  }
}

JobStatus JobSystem::wait(const JobHandle handle) {
  return waitFrom(handle, {});
}

void JobSystem::waitAll(const std::span<const JobHandle> handles) {
  std::exception_ptr firstFailure;
  for (const JobHandle handle : handles) {
    try {
      static_cast<void>(wait(handle));
    } catch (...) {
      if (!firstFailure)
        firstFailure = std::current_exception();
    }
  }
  if (firstFailure)
    std::rethrow_exception(firstFailure);
}

void JobSystem::release(const JobHandle handle) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->validUnlocked(handle)) {
    throw std::invalid_argument("Job handle is stale or invalid");
  }
  Impl::Slot &slot = impl_->slots[handle.index];
  if (!terminal(slot.status)) {
    throw std::logic_error("Cannot release an unfinished job");
  }
  if (slot.firstDependent != Impl::kInvalid) {
    throw std::logic_error(
        "Cannot release a job before dependents are resolved");
  }
  if (slot.remainingDependencies != 0) {
    throw std::logic_error(
        "Cannot release a job before dependencies are resolved");
  }
  slot.callback = nullptr;
  slot.context = nullptr;
  slot.failure = nullptr;
  slot.cancellationRequested.store(false, std::memory_order_relaxed);
  slot.status = JobStatus::Cancelled;
  slot.remainingDependencies = 0;
  slot.name[0] = '\0';
  if (++slot.generation == 0)
    ++slot.generation;
  impl_->freeSlots[impl_->freeSlotCount++] = handle.index;
}

void JobSystem::releaseAll(const std::span<const JobHandle> handles) {
  for (const JobHandle handle : handles)
    release(handle);
}

JobSystemStats JobSystem::stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->aggregate;
}

std::vector<JobTimelineEvent> JobSystem::timeline() const {
  std::lock_guard lock(impl_->mutex);
  std::vector<JobTimelineEvent> result;
  result.reserve(impl_->timelineCount);
  for (std::size_t index = 0; index < impl_->timelineCount; ++index) {
    result.push_back(impl_->timelineEvents[(impl_->timelineBegin + index) %
                                           impl_->timelineEvents.size()]);
  }
  return result;
}

std::uint32_t JobSystem::workerCount() const noexcept {
  return static_cast<std::uint32_t>(impl_->workers.size());
}

std::uint32_t JobSystem::capacity() const noexcept {
  return impl_->maximumJobs;
}

void JobSystem::shutdown(const JobShutdownMode mode) {
  if (!impl_)
    return;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping)
      return;
    impl_->accepting = false;
    if (mode == JobShutdownMode::Cancel) {
      for (std::uint32_t index = 0; index < impl_->maximumJobs; ++index) {
        Impl::Slot &slot = impl_->slots[index];
        if (slot.callback == nullptr || terminal(slot.status))
          continue;
        slot.cancellationRequested.store(true, std::memory_order_relaxed);
        if (slot.status != JobStatus::Running) {
          impl_->makeTerminalUnlocked(index, JobStatus::Cancelled,
                                      Impl::kInvalid, monotonicNanoseconds());
        }
      }
    }
  }
  if (mode == JobShutdownMode::Drain) {
    std::unique_lock lock(impl_->mutex);
    impl_->completed.wait(lock,
                          [this] { return impl_->aggregate.activeJobs == 0; });
  } else {
    std::unique_lock lock(impl_->mutex);
    impl_->completed.wait(lock,
                          [this] { return impl_->aggregate.runningJobs == 0; });
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
  }
  impl_->workAvailable.notify_all();
  for (Impl::Worker &worker : impl_->workers) {
    if (worker.thread.joinable())
      worker.thread.join();
  }
}

} // namespace ve
