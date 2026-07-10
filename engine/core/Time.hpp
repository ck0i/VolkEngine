#pragma once

#include <chrono>
#include <cstdint>

namespace ve {

struct FrameTiming {
    double deltaSeconds = 0.0;
    double elapsedSeconds = 0.0;
    std::uint64_t frameIndex = 0;
};

[[nodiscard]] constexpr double clampDeltaSeconds(const double deltaSeconds, const double maximumSeconds) noexcept {
    if (maximumSeconds <= 0.0 || deltaSeconds <= 0.0) {
        return 0.0;
    }
    return deltaSeconds < maximumSeconds ? deltaSeconds : maximumSeconds;
}

class Clock {
public:
    Clock();
    [[nodiscard]] FrameTiming tick();

private:
    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint start_;
    TimePoint previous_;
    std::uint64_t frameIndex_ = 0;
};

} // namespace ve
