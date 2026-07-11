#include "core/JobSystem.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

template <typename Exception, typename Function>
bool throws(Function &&function) {
  try {
    function();
    return false;
  } catch (const Exception &) {
    return true;
  }
}

void increment(void *context, ve::JobContext &) {
  static_cast<std::atomic_uint32_t *>(context)->fetch_add(
      1U, std::memory_order_relaxed);
}

struct OrderedPayload {
  std::atomic_uint32_t *bits = nullptr;
  std::uint32_t required = 0;
  std::uint32_t produced = 0;
};

void ordered(void *context, ve::JobContext &) {
  const auto &payload = *static_cast<OrderedPayload *>(context);
  assert((payload.bits->load(std::memory_order_acquire) & payload.required) ==
         payload.required);
  payload.bits->fetch_or(payload.produced, std::memory_order_release);
}

void fail(void *, ve::JobContext &) {
  throw std::runtime_error("expected job failure");
}

struct ParallelPayload {
  std::atomic_uint32_t *active = nullptr;
  std::atomic_uint32_t *maximum = nullptr;
};

void overlap(void *context, ve::JobContext &) {
  const auto &payload = *static_cast<ParallelPayload *>(context);
  const std::uint32_t current =
      payload.active->fetch_add(1U, std::memory_order_acq_rel) + 1U;
  std::uint32_t observed = payload.maximum->load(std::memory_order_relaxed);
  while (observed < current &&
         !payload.maximum->compare_exchange_weak(observed, current,
                                                 std::memory_order_relaxed)) {
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  payload.active->fetch_sub(1U, std::memory_order_release);
}

struct NestedPayload {
  ve::JobSystem *jobs = nullptr;
  std::atomic_uint32_t *value = nullptr;
};

void nested(void *context, ve::JobContext &current) {
  auto &payload = *static_cast<NestedPayload *>(context);
  const ve::JobHandle child = payload.jobs->submit({.name = "nested-child",
                                                    .callback = increment,
                                                    .context = payload.value});
  assert(current.wait(child) == ve::JobStatus::Succeeded);
  payload.jobs->release(child);
  payload.value->fetch_add(1U, std::memory_order_relaxed);
}

struct BlockingPayload {
  std::atomic_bool *entered = nullptr;
  std::atomic_bool *release = nullptr;
};

void blocking(void *context, ve::JobContext &) {
  const auto &payload = *static_cast<BlockingPayload *>(context);
  payload.entered->store(true, std::memory_order_release);
  while (!payload.release->load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

void cooperativeCancel(void *context, ve::JobContext &job) {
  static_cast<std::atomic_bool *>(context)->store(true,
                                                  std::memory_order_release);
  while (!job.cancellationRequested()) {
    std::this_thread::yield();
  }
}

void waitUntil(const std::atomic_bool &value) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!value.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error("Timed out waiting for job test state");
    }
    std::this_thread::yield();
  }
}

} // namespace

int main() {
  assert(throws<std::invalid_argument>([] {
    static_cast<void>(ve::JobSystem({.workerCount = 1, .maximumJobs = 0}));
  }));

  {
    ve::JobSystem jobs({.workerCount = 4,
                        .maximumJobs = 32,
                        .maximumDependencies = 64,
                        .timelineCapacity = 16});
    std::atomic_uint32_t bits{0};
    OrderedPayload first{&bits, 0U, 1U};
    OrderedPayload second{&bits, 0U, 2U};
    OrderedPayload joined{&bits, 3U, 4U};
    const ve::JobHandle a =
        jobs.submit({.name = "first", .callback = ordered, .context = &first});
    const ve::JobHandle b = jobs.submit(
        {.name = "second", .callback = ordered, .context = &second});
    const std::array dependencies{a, b};
    const ve::JobHandle c = jobs.submit({.name = "joined",
                                         .callback = ordered,
                                         .context = &joined,
                                         .dependencies = dependencies});
    jobs.waitAll(dependencies);
    assert(jobs.wait(c) == ve::JobStatus::Succeeded);
    assert(bits.load(std::memory_order_acquire) == 7U);
    jobs.release(c);
    jobs.releaseAll(dependencies);
    const ve::JobSystemStats stats = jobs.stats();
    assert(stats.submitted == 3U);
    assert(stats.succeeded == 3U);
    assert(stats.failed == 0U);
    assert(stats.activeJobs == 0U);
    jobs.shutdown();
  }

  {
    ve::JobSystem jobs({.workerCount = 4,
                        .maximumJobs = 16,
                        .maximumDependencies = 16,
                        .timelineCapacity = 16});
    std::atomic_uint32_t active{0};
    std::atomic_uint32_t maximum{0};
    ParallelPayload payload{&active, &maximum};
    std::array<ve::JobHandle, 4> handles{};
    for (std::size_t index = 0; index < handles.size(); ++index) {
      handles[index] = jobs.submit(
          {.name = "overlap", .callback = overlap, .context = &payload});
    }
    jobs.waitAll(handles);
    assert(maximum.load(std::memory_order_relaxed) >= 2U);
    jobs.releaseAll(handles);
  }

  {
    ve::JobSystem jobs({.workerCount = 1,
                        .maximumJobs = 8,
                        .maximumDependencies = 8,
                        .timelineCapacity = 8});
    std::atomic_uint32_t value{0};
    NestedPayload payload{&jobs, &value};
    const ve::JobHandle parent = jobs.submit(
        {.name = "nested-parent", .callback = nested, .context = &payload});
    assert(jobs.wait(parent) == ve::JobStatus::Succeeded);
    assert(value.load(std::memory_order_relaxed) == 2U);
    jobs.release(parent);
  }

  {
    ve::JobSystem jobs({.workerCount = 2,
                        .maximumJobs = 8,
                        .maximumDependencies = 8,
                        .timelineCapacity = 8});
    const ve::JobHandle parent =
        jobs.submit({.name = "failure", .callback = fail});
    const std::array dependencies{parent};
    std::atomic_uint32_t ran{0};
    const ve::JobHandle child = jobs.submit({.name = "cancelled-dependent",
                                             .callback = increment,
                                             .context = &ran,
                                             .dependencies = dependencies});
    assert(throws<std::runtime_error>(
        [&] { static_cast<void>(jobs.wait(parent)); }));
    assert(jobs.wait(child) == ve::JobStatus::Cancelled);
    assert(ran.load(std::memory_order_relaxed) == 0U);
    jobs.release(child);
    jobs.release(parent);
    const ve::JobSystemStats stats = jobs.stats();
    assert(stats.failed == 1U);
    assert(stats.cancelled == 1U);
  }

  {
    ve::JobSystem jobs({.workerCount = 1,
                        .maximumJobs = 4,
                        .maximumDependencies = 4,
                        .timelineCapacity = 4});
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    BlockingPayload blocker{&entered, &release};
    const ve::JobHandle running = jobs.submit(
        {.name = "blocker", .callback = blocking, .context = &blocker});
    waitUntil(entered);
    std::atomic_uint32_t ran{0};
    const ve::JobHandle queued =
        jobs.submit({.name = "queued", .callback = increment, .context = &ran});
    assert(jobs.cancel(queued));
    assert(jobs.wait(queued) == ve::JobStatus::Cancelled);
    release.store(true, std::memory_order_release);
    assert(jobs.wait(running) == ve::JobStatus::Succeeded);
    assert(ran.load(std::memory_order_relaxed) == 0U);
    jobs.release(queued);
    jobs.release(running);
  }

  {
    ve::JobSystem jobs({.workerCount = 1,
                        .maximumJobs = 4,
                        .maximumDependencies = 4,
                        .timelineCapacity = 4});
    std::atomic_bool entered{false};
    const ve::JobHandle running = jobs.submit({.name = "cooperative-cancel",
                                               .callback = cooperativeCancel,
                                               .context = &entered});
    waitUntil(entered);
    assert(jobs.cancel(running));
    assert(jobs.wait(running) == ve::JobStatus::Cancelled);
    jobs.release(running);
  }

  {
    ve::JobSystem jobs({.workerCount = 1,
                        .maximumJobs = 2,
                        .maximumDependencies = 2,
                        .timelineCapacity = 2});
    std::atomic_uint32_t count{0};
    const ve::JobHandle first = jobs.submit(
        {.name = "temporary-name", .callback = increment, .context = &count});
    const ve::JobHandle second = jobs.submit(
        {.name = "second", .callback = increment, .context = &count});
    assert(throws<std::runtime_error>([&] {
      static_cast<void>(jobs.submit(
          {.name = "exhausted", .callback = increment, .context = &count}));
    }));
    jobs.waitAll(std::array{first, second});
    jobs.release(first);
    assert(throws<std::invalid_argument>(
        [&] { static_cast<void>(jobs.status(first)); }));
    const ve::JobHandle third = jobs.submit(
        {.name = "third", .callback = increment, .context = &count});
    jobs.waitAll(std::array{second, third});
    jobs.release(second);
    jobs.release(third);
    assert(count.load(std::memory_order_relaxed) == 3U);
    const std::vector<ve::JobTimelineEvent> timeline = jobs.timeline();
    assert(timeline.size() == 2U);
    assert(std::string{timeline[0].name} == "temporary-name" ||
           std::string{timeline[0].name} == "second");
    assert(std::string{timeline[1].name} == "third");
  }

  {
    ve::JobSystem jobs({.workerCount = 2,
                        .maximumJobs = 8,
                        .maximumDependencies = 8,
                        .timelineCapacity = 8});
    std::atomic_uint32_t count{0};
    std::array<ve::JobHandle, 4> handles{};
    for (ve::JobHandle &handle : handles) {
      handle = jobs.submit(
          {.name = "drain", .callback = increment, .context = &count});
    }
    jobs.shutdown(ve::JobShutdownMode::Drain);
    assert(count.load(std::memory_order_relaxed) == handles.size());
    assert(throws<std::logic_error>([&] {
      static_cast<void>(jobs.submit({.name = "after-shutdown",
                                     .callback = increment,
                                     .context = &count}));
    }));
  }

  return 0;
}
