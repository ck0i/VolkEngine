#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace ve {

struct FrameTiming {
    double deltaSeconds = 0.0;
    double elapsedSeconds = 0.0;
    std::uint64_t frameIndex = 0;
};

[[nodiscard]] constexpr double clampDeltaSeconds(const double deltaSeconds, const double maximumSeconds) noexcept {
    constexpr double kMaximumFiniteSeconds = std::numeric_limits<double>::max();
    if (!(maximumSeconds > 0.0) || maximumSeconds > kMaximumFiniteSeconds ||
        !(deltaSeconds > 0.0) || deltaSeconds > kMaximumFiniteSeconds) {
        return 0.0;
    }
    return deltaSeconds < maximumSeconds ? deltaSeconds : maximumSeconds;
}
[[nodiscard]] constexpr double advanceSimulationSeconds(const double currentSeconds,
                                                         const double deltaSeconds,
                                                         const double maximumDeltaSeconds) noexcept {
    constexpr double kMaximumFiniteSeconds = std::numeric_limits<double>::max();
    if (currentSeconds != currentSeconds ||
        currentSeconds < -kMaximumFiniteSeconds ||
        currentSeconds > kMaximumFiniteSeconds) {
        return 0.0;
    }

    const double stepSeconds = clampDeltaSeconds(deltaSeconds, maximumDeltaSeconds);
    if (currentSeconds > kMaximumFiniteSeconds - stepSeconds) {
        return kMaximumFiniteSeconds;
    }
    return currentSeconds + stepSeconds;
}

struct FixedStepBatch {
    std::uint32_t stepCount = 0;
    double firstStepElapsedSeconds = 0.0;
    double stepSeconds = 0.0;
    double interpolationAlpha = 0.0;
    double retainedSeconds = 0.0;
    double droppedSeconds = 0.0;

    [[nodiscard]] constexpr double elapsedSecondsForStep(const std::uint32_t stepIndex) const noexcept {
        const double offsetSeconds = static_cast<double>(stepIndex) * stepSeconds;
        return advanceSimulationSeconds(firstStepElapsedSeconds, offsetSeconds, offsetSeconds);
    }
};

class FixedStepClock {
public:
    static constexpr double kDefaultStepSeconds = 1.0 / 60.0;
    static constexpr double kDefaultMaximumAccumulatedSeconds = 0.25;
    static constexpr std::uint32_t kDefaultMaximumSubsteps = 8;

    FixedStepClock();
    FixedStepClock(double stepSeconds,
                   double maximumAccumulatedSeconds,
                   std::uint32_t maximumSubsteps);

    [[nodiscard]] FixedStepBatch advance(double frameDeltaSeconds) noexcept;
    void reset() noexcept;

    [[nodiscard]] double elapsedSeconds() const noexcept { return elapsedSeconds_; }
    [[nodiscard]] double retainedSeconds() const noexcept { return accumulatedSeconds_; }
    [[nodiscard]] double stepSeconds() const noexcept { return stepSeconds_; }
    [[nodiscard]] std::uint32_t maximumSubsteps() const noexcept { return maximumSubsteps_; }

private:
    double stepSeconds_ = kDefaultStepSeconds;
    double maximumAccumulatedSeconds_ = kDefaultMaximumAccumulatedSeconds;
    std::uint32_t maximumSubsteps_ = kDefaultMaximumSubsteps;
    double accumulatedSeconds_ = 0.0;
    double elapsedSeconds_ = 0.0;
};

class Clock {
public:
    Clock();
    explicit Clock(std::chrono::steady_clock::time_point start);
    [[nodiscard]] FrameTiming tick();
    [[nodiscard]] FrameTiming tickAt(std::chrono::steady_clock::time_point now);

private:
    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint start_;
    TimePoint previous_;
    std::uint64_t frameIndex_ = 0;
};

} // namespace ve
