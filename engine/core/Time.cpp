#include "core/Time.hpp"
#include <stdexcept>

namespace ve {

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
