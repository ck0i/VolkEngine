#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ve {

struct JobHandle {
  std::uint32_t index = UINT32_MAX;
  std::uint32_t generation = 0;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return index != UINT32_MAX && generation != 0;
  }
  friend constexpr bool operator==(JobHandle, JobHandle) = default;
};

enum class JobStatus : std::uint8_t {
  Waiting,
  Ready,
  Running,
  Succeeded,
  Failed,
  Cancelled,
};

enum class JobShutdownMode : std::uint8_t { Drain, Cancel };
enum class JobCategory : std::uint8_t {
  General,
  Simulation,
  Io,
  Asset,
  Count,
};

inline constexpr std::size_t kJobCategoryCount =
    static_cast<std::size_t>(JobCategory::Count);

struct JobSystemStats {
  std::uint64_t submitted = 0;
  std::uint64_t succeeded = 0;
  std::uint64_t failed = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t executedNanoseconds = 0;
  std::uint64_t steals = 0;
  std::uint32_t workerCount = 0;
  std::uint32_t activeJobs = 0;
  std::uint32_t runningJobs = 0;
  std::uint32_t queueHighWatermark = 0;
  std::array<std::uint64_t, kJobCategoryCount> categorySubmitted{};
  std::array<std::uint64_t, kJobCategoryCount> categorySucceeded{};
  std::array<std::uint64_t, kJobCategoryCount> categoryFailed{};
  std::array<std::uint64_t, kJobCategoryCount> categoryCancelled{};
  std::array<std::uint64_t, kJobCategoryCount> categoryNanoseconds{};
};

struct JobTimelineEvent {
  static constexpr std::size_t kMaximumNameBytes = 47;

  JobHandle handle{};
  JobStatus status = JobStatus::Waiting;
  JobCategory category = JobCategory::General;
  std::uint32_t workerIndex = UINT32_MAX;
  std::uint64_t queuedNanoseconds = 0;
  std::uint64_t startedNanoseconds = 0;
  std::uint64_t finishedNanoseconds = 0;
  char name[kMaximumNameBytes + 1]{};
};

class JobSystem;

class JobContext final {
public:
  [[nodiscard]] bool cancellationRequested() const noexcept;
  JobStatus wait(JobHandle handle);

private:
  friend class JobSystem;
  JobContext(JobSystem &system, JobHandle current) noexcept;

  JobSystem *system_ = nullptr;
  JobHandle current_{};
};

using JobCallback = void (*)(void *context, JobContext &job);

struct JobDesc {
  std::string_view name;
  JobCallback callback = nullptr;
  void *context = nullptr;
  std::span<const JobHandle> dependencies{};
  JobCategory category = JobCategory::General;
};

class JobSystem final {
public:
  struct Config {
    std::uint32_t workerCount = 0;
    std::uint32_t maximumJobs = 1'024;
    std::uint32_t maximumDependencies = 4'096;
    std::uint32_t timelineCapacity = 4'096;
  };

  JobSystem();
  explicit JobSystem(Config config);
  JobSystem(const JobSystem &) = delete;
  JobSystem &operator=(const JobSystem &) = delete;
  JobSystem(JobSystem &&) = delete;
  JobSystem &operator=(JobSystem &&) = delete;
  ~JobSystem();

  [[nodiscard]] JobHandle submit(const JobDesc &descriptor);
  [[nodiscard]] JobStatus status(JobHandle handle) const;
  [[nodiscard]] bool cancellationRequested(JobHandle handle) const noexcept;
  bool cancel(JobHandle handle);

  JobStatus wait(JobHandle handle);
  void waitAll(std::span<const JobHandle> handles);
  void release(JobHandle handle);
  void releaseAll(std::span<const JobHandle> handles);

  [[nodiscard]] JobSystemStats stats() const;
  [[nodiscard]] std::vector<JobTimelineEvent> timeline() const;
  [[nodiscard]] std::uint32_t workerCount() const noexcept;
  [[nodiscard]] std::uint32_t capacity() const noexcept;

  void shutdown(JobShutdownMode mode = JobShutdownMode::Drain);

private:
  friend class JobContext;
  struct Impl;
  std::unique_ptr<Impl> impl_;

  JobStatus waitFrom(JobHandle handle, JobHandle current);
};

} // namespace ve
