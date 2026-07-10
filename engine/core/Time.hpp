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
