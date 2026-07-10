#include "core/Time.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ve {

FixedStepClock::FixedStepClock()
    : FixedStepClock(kDefaultStepSeconds,
                     kDefaultMaximumAccumulatedSeconds,
                     kDefaultMaximumSubsteps) {}

FixedStepClock::FixedStepClock(const double stepSeconds,
                               const double maximumAccumulatedSeconds,
                               const std::uint32_t maximumSubsteps)
    : stepSeconds_(stepSeconds),
      maximumAccumulatedSeconds_(maximumAccumulatedSeconds),
      maximumSubsteps_(maximumSubsteps) {
    if (!std::isfinite(stepSeconds_) || stepSeconds_ <= 0.0 ||
        !std::isfinite(maximumAccumulatedSeconds_) ||
        maximumAccumulatedSeconds_ < stepSeconds_ ||
        maximumAccumulatedSeconds_ - stepSeconds_ == maximumAccumulatedSeconds_ ||
        maximumSubsteps_ == 0U) {
        throw std::invalid_argument("Fixed-step timing requires finite positive values with enough precision to consume one step, an accumulator at least one step long, and at least one substep");
    }
}

FixedStepBatch FixedStepClock::advance(const double frameDeltaSeconds) noexcept {
    FixedStepBatch batch{};
    batch.firstStepElapsedSeconds = elapsedSeconds_;
    batch.stepSeconds = stepSeconds_;

    if (std::isfinite(frameDeltaSeconds) && frameDeltaSeconds > 0.0) {
        const double capacity = maximumAccumulatedSeconds_ - accumulatedSeconds_;
        const double acceptedSeconds = std::min(frameDeltaSeconds, capacity);
        accumulatedSeconds_ += acceptedSeconds;
        batch.droppedSeconds = frameDeltaSeconds - acceptedSeconds;
    }

    const double previousElapsedSeconds = elapsedSeconds_;
    const double availableSteps = std::floor(accumulatedSeconds_ / stepSeconds_);
    const double executableSteps = std::min(availableSteps, static_cast<double>(maximumSubsteps_));
    batch.stepCount = static_cast<std::uint32_t>(executableSteps);
    if (batch.stepCount > 0U) {
        const double executedSeconds = static_cast<double>(batch.stepCount) * stepSeconds_;
        accumulatedSeconds_ -= executedSeconds;
        elapsedSeconds_ = advanceSimulationSeconds(elapsedSeconds_, executedSeconds, executedSeconds);
    }
    if (accumulatedSeconds_ < 0.0) {
        accumulatedSeconds_ = 0.0;
    }

    batch.firstStepElapsedSeconds = batch.stepCount > 0U
        ? advanceSimulationSeconds(previousElapsedSeconds, stepSeconds_, stepSeconds_)
        : previousElapsedSeconds;
    batch.retainedSeconds = accumulatedSeconds_;
    batch.interpolationAlpha = std::min(accumulatedSeconds_ / stepSeconds_, 1.0);
    return batch;
}

void FixedStepClock::reset() noexcept {
    accumulatedSeconds_ = 0.0;
    elapsedSeconds_ = 0.0;
}

Clock::Clock()
    : Clock(std::chrono::steady_clock::now()) {}

Clock::Clock(const std::chrono::steady_clock::time_point start)
    : start_(start), previous_(start) {}

FrameTiming Clock::tick() {
    return tickAt(std::chrono::steady_clock::now());
}

FrameTiming Clock::tickAt(const std::chrono::steady_clock::time_point now) {
    if (now < previous_) {
        throw std::runtime_error("Clock sample moved backwards");
    }
    const double delta = std::chrono::duration<double>(now - previous_).count();
    const double elapsed = std::chrono::duration<double>(now - start_).count();
    previous_ = now;
    return FrameTiming{delta, elapsed, frameIndex_++};
}

} // namespace ve
