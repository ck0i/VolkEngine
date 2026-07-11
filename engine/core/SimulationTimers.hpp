#pragma once

#include "core/SimulationStepResource.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace ve {

class TimerHandle final {
public:
    constexpr TimerHandle() noexcept = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0U; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    friend constexpr bool operator==(const TimerHandle&, const TimerHandle&) noexcept = default;

private:
    constexpr explicit TimerHandle(const std::uint64_t value) noexcept : value_(value) {}
    std::uint64_t value_ = 0U;

    template <typename T, std::size_t Capacity>
    friend class SimulationTimerQueue;
};

template <typename T>
struct SimulationTimerEvent final {
    TimerHandle handle{};
    T payload{};
};

template <typename T, std::size_t Capacity>
class SimulationTimerQueue final : private detail::SimulationStepResource {
    static_assert(Capacity > 0U, "Simulation timer queue capacity must be positive");
    static_assert(std::is_trivially_copyable_v<T>, "Simulation timer payloads must be trivially copyable");
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "Simulation timer payloads must be nothrow default constructible");
    static_assert(std::is_trivially_copy_assignable_v<T>,
                  "Simulation timer payloads must be trivially copy assignable");
    static_assert(std::is_nothrow_copy_constructible_v<T>,
                  "Simulation timer payloads must be nothrow copy constructible");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "Simulation timer payloads must be nothrow move constructible");
    static_assert(std::is_nothrow_move_assignable_v<T>,
                  "Simulation timer payloads must be nothrow move assignable");

    struct Timer final {
        TimerHandle handle{};
        T payload{};
        std::uint64_t dueTick = 0U;
        std::uint64_t repeatSteps = 0U;
    };

public:
    SimulationTimerQueue(const SimulationTimerQueue&) = delete;
    SimulationTimerQueue& operator=(const SimulationTimerQueue&) = delete;
    SimulationTimerQueue(SimulationTimerQueue&&) = delete;
    SimulationTimerQueue& operator=(SimulationTimerQueue&&) = delete;
    ~SimulationTimerQueue() override = default;

    [[nodiscard]] TimerHandle schedule(std::uint64_t delaySteps,
                                       const T& payload,
                                       const std::uint64_t repeatSteps = 0U) noexcept {
        if (pendingScheduleCount_ == Capacity || activeCount_ + pendingScheduleCount_ >= Capacity ||
            nextHandle_ == 0U) {
            overflowed_ = true;
            return {};
        }
        if (delaySteps == 0U) {
            delaySteps = 1U;
        }
        if (delaySteps > std::numeric_limits<std::uint64_t>::max() - currentTick_) {
            overflowed_ = true;
            return {};
        }

        const TimerHandle handle{nextHandle_};
        if (nextHandle_ == std::numeric_limits<std::uint64_t>::max()) {
            nextHandle_ = 0U;
        } else {
            ++nextHandle_;
        }
        pendingSchedules_[pendingScheduleCount_] = Timer{handle, payload, currentTick_ + delaySteps, repeatSteps};
        ++pendingScheduleCount_;
        return handle;
    }

    [[nodiscard]] bool cancel(const TimerHandle handle) noexcept {
        if (!handle.valid() || canceled(handle)) {
            return false;
        }
        if (!contains(handle)) {
            return false;
        }
        if (pendingCancelCount_ == Capacity) {
            overflowed_ = true;
            return false;
        }
        pendingCancels_[pendingCancelCount_] = handle;
        ++pendingCancelCount_;
        return true;
    }

    [[nodiscard]] bool active(const TimerHandle handle) const noexcept {
        return handle.valid() && contains(handle) && !canceled(handle);
    }

    [[nodiscard]] std::size_t activeCount() const noexcept {
        std::size_t count = 0U;
        for (std::size_t index = 0U; index < activeCount_; ++index) {
            if (!canceled(active_[index].handle)) {
                ++count;
            }
        }
        for (std::size_t index = 0U; index < pendingScheduleCount_; ++index) {
            if (!canceled(pendingSchedules_[index].handle)) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::span<const SimulationTimerEvent<T>> events() const noexcept {
        return {dueEvents_.data(), dueEventCount_};
    }

    [[nodiscard]] std::uint64_t currentTick() const noexcept { return currentTick_; }

    void reset() {
        if (transactionActive_) {
            throw std::logic_error("Cannot reset a simulation timer queue during execution");
        }
        activeCount_ = 0U;
        pendingScheduleCount_ = 0U;
        pendingCancelCount_ = 0U;
        dueEventCount_ = 0U;
        checkpointScheduleCount_ = 0U;
        checkpointCancelCount_ = 0U;
        currentTick_ = 0U;
        overflowed_ = false;
        checkpointOverflowed_ = false;
    }

private:
    friend class WorldSystemScheduler;

    SimulationTimerQueue() noexcept = default;

    [[nodiscard]] bool contains(const TimerHandle handle) const noexcept {
        for (std::size_t index = 0U; index < activeCount_; ++index) {
            if (active_[index].handle == handle) {
                return true;
            }
        }
        for (std::size_t index = 0U; index < pendingScheduleCount_; ++index) {
            if (pendingSchedules_[index].handle == handle) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool canceled(const TimerHandle handle) const noexcept {
        for (std::size_t index = 0U; index < pendingCancelCount_; ++index) {
            if (pendingCancels_[index] == handle) {
                return true;
            }
        }
        return false;
    }

    void checkpoint() noexcept override {
        transactionActive_ = true;
        checkpointScheduleCount_ = pendingScheduleCount_;
        checkpointCancelCount_ = pendingCancelCount_;
        checkpointOverflowed_ = overflowed_;
        dueEventCount_ = 0U;

        if (currentTick_ == std::numeric_limits<std::uint64_t>::max()) {
            overflowed_ = true;
            return;
        }
        for (std::size_t index = 0U; index < activeCount_; ++index) {
            const Timer& timer = active_[index];
            if (canceled(timer.handle)) {
                continue;
            }
            if (timer.dueTick > currentTick_) {
                break;
            }
            dueEvents_[dueEventCount_] = SimulationTimerEvent<T>{timer.handle, timer.payload};
            ++dueEventCount_;
            if (timer.repeatSteps != 0U && !canceled(timer.handle) &&
                timer.repeatSteps > std::numeric_limits<std::uint64_t>::max() - timer.dueTick) {
                overflowed_ = true;
            }
        }
    }

    void rollback() noexcept override {
        pendingScheduleCount_ = checkpointScheduleCount_;
        pendingCancelCount_ = checkpointCancelCount_;
        overflowed_ = checkpointOverflowed_;
        dueEventCount_ = 0U;
        transactionActive_ = false;
    }

    void promote() noexcept override {
        std::size_t writeIndex = 0U;
        bool changed = pendingScheduleCount_ != 0U || pendingCancelCount_ != 0U;
        for (std::size_t index = 0U; index < activeCount_; ++index) {
            Timer timer = active_[index];
            if (canceled(timer.handle)) {
                changed = true;
                continue;
            }
            if (timer.dueTick <= currentTick_) {
                if (timer.repeatSteps == 0U) {
                    changed = true;
                    continue;
                }
                timer.dueTick += timer.repeatSteps;
                changed = true;
            }
            active_[writeIndex] = timer;
            ++writeIndex;
        }
        activeCount_ = writeIndex;

        for (std::size_t index = 0U; index < pendingScheduleCount_; ++index) {
            if (!canceled(pendingSchedules_[index].handle)) {
                active_[activeCount_] = pendingSchedules_[index];
                ++activeCount_;
            }
        }
        if (changed) {
            std::sort(active_.begin(), active_.begin() + static_cast<std::ptrdiff_t>(activeCount_),
                      [](const Timer& left, const Timer& right) noexcept {
                          if (left.dueTick != right.dueTick) {
                              return left.dueTick < right.dueTick;
                          }
                          return left.handle.value() < right.handle.value();
                      });
        }
        pendingScheduleCount_ = 0U;
        pendingCancelCount_ = 0U;
        dueEventCount_ = 0U;
        overflowed_ = false;
        transactionActive_ = false;
        ++currentTick_;
    }

    [[nodiscard]] bool overflowed() const noexcept override {
        return overflowed_;
    }

    std::array<Timer, Capacity> active_{};
    std::array<Timer, Capacity> pendingSchedules_{};
    std::array<TimerHandle, Capacity> pendingCancels_{};
    std::array<SimulationTimerEvent<T>, Capacity> dueEvents_{};
    std::size_t activeCount_ = 0U;
    std::size_t pendingScheduleCount_ = 0U;
    std::size_t pendingCancelCount_ = 0U;
    std::size_t dueEventCount_ = 0U;
    std::size_t checkpointScheduleCount_ = 0U;
    std::size_t checkpointCancelCount_ = 0U;
    std::uint64_t currentTick_ = 0U;
    std::uint64_t nextHandle_ = 1U;
    bool overflowed_ = false;
    bool checkpointOverflowed_ = false;
    bool transactionActive_ = false;
};

} // namespace ve
