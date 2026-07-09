#include "core/Time.hpp"

namespace ve {

Clock::Clock()
    : start_(std::chrono::steady_clock::now()), previous_(start_) {}

FrameTiming Clock::tick() {
    const TimePoint now = std::chrono::steady_clock::now();
    const double delta = std::chrono::duration<double>(now - previous_).count();
    const double elapsed = std::chrono::duration<double>(now - start_).count();
    previous_ = now;
    return FrameTiming{delta, elapsed, frameIndex_++};
}

} // namespace ve
