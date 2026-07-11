#include "core/JobSystem.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <thread>

namespace {

constexpr std::size_t kJobCount = 128U;
constexpr std::uint32_t kIterations = 1'000'000U;

struct WorkPayload {
  std::uint64_t seed = 0;
  std::uint64_t output = 0;
};

void runWork(void *context, ve::JobContext &) {
  auto &payload = *static_cast<WorkPayload *>(context);
  std::uint64_t value = payload.seed;
  for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
    value ^= value >> 12U;
    value ^= value << 25U;
    value ^= value >> 27U;
    value *= 0x2545F4914F6CDD1DULL;
    value = std::rotl(value, 17);
  }
  payload.output = value;
}

struct Result {
  double milliseconds = 0.0;
  std::uint64_t checksum = 0;
  ve::JobSystemStats stats;
};

Result measure(const std::uint32_t workerCount) {
  ve::JobSystem jobs({.workerCount = workerCount,
                      .maximumJobs = kJobCount,
                      .maximumDependencies = 1U,
                      .timelineCapacity = 1U});
  std::array<WorkPayload, kJobCount> payloads{};
  std::array<ve::JobHandle, kJobCount> handles{};
  for (std::size_t index = 0; index < payloads.size(); ++index) {
    payloads[index].seed =
        0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(index + 1U);
  }

  const auto start = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < handles.size(); ++index) {
    handles[index] = jobs.submit({.name = "benchmark-work",
                                  .callback = runWork,
                                  .context = &payloads[index]});
  }
  jobs.waitAll(handles);
  const double milliseconds = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
  jobs.releaseAll(handles);

  std::uint64_t checksum = 0;
  for (const WorkPayload &payload : payloads)
    checksum ^= payload.output;
  return {milliseconds, checksum, jobs.stats()};
}

} // namespace

int main() {
  const std::uint32_t hardwareWorkers =
      std::max(1U, std::thread::hardware_concurrency());
  if (hardwareWorkers < 8U) {
    std::cerr << "Job-system benchmark requires eight logical workers\n";
    return 2;
  }
  constexpr std::uint32_t parallelWorkers = 8U;
  Result serial{};
  Result parallel{};
  serial.milliseconds = std::numeric_limits<double>::max();
  parallel.milliseconds = std::numeric_limits<double>::max();
  for (std::uint32_t round = 0; round < 3U; ++round) {
    const Result parallelRound = measure(parallelWorkers);
    const Result serialRound = measure(1U);
    if (serialRound.checksum != parallelRound.checksum)
      return 1;
    if (serialRound.milliseconds < serial.milliseconds)
      serial = serialRound;
    if (parallelRound.milliseconds < parallel.milliseconds)
      parallel = parallelRound;
  }
  const double speedup = serial.milliseconds / parallel.milliseconds;
  std::cout << std::fixed << std::setprecision(3) << "{\"jobs\":" << kJobCount
            << ",\"iterations_per_job\":" << kIterations
            << ",\"parallel_workers\":" << parallelWorkers
            << ",\"serial_ms\":" << serial.milliseconds
            << ",\"parallel_ms\":" << parallel.milliseconds
            << ",\"speedup\":" << speedup
            << ",\"parallel_steals\":" << parallel.stats.steals
            << ",\"checksum\":" << serial.checksum << "}\n";
  return 0;
}
