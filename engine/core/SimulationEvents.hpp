#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace ve {

class WorldSystemScheduler;

template <typename T, std::size_t Capacity>
class SimulationEventChannel;

namespace detail {

class SimulationEventChannelControl {
private:
    constexpr SimulationEventChannelControl() noexcept = default;

    virtual void checkpoint() noexcept = 0;
    virtual void rollback() noexcept = 0;
    virtual void promote() noexcept = 0;
    [[nodiscard]] virtual bool overflowed() const noexcept = 0;

    friend class ::ve::WorldSystemScheduler;
    template <typename T, std::size_t Capacity>
    friend class ::ve::SimulationEventChannel;

public:
    virtual ~SimulationEventChannelControl() = default;
};

} // namespace detail

template <typename T, std::size_t Capacity>
class SimulationEventChannel final : private detail::SimulationEventChannelControl {
    static_assert(Capacity > 0U, "Simulation event channel capacity must be positive");
    static_assert(std::is_trivially_copyable_v<T>, "Simulation event payloads must be trivially copyable");
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "Simulation event payloads must be nothrow default constructible");
    static_assert(std::is_trivially_copy_assignable_v<T>,
                  "Simulation event payloads must be trivially copy assignable");

public:
    SimulationEventChannel(const SimulationEventChannel&) = delete;
    SimulationEventChannel& operator=(const SimulationEventChannel&) = delete;
    SimulationEventChannel(SimulationEventChannel&&) = delete;
    SimulationEventChannel& operator=(SimulationEventChannel&&) = delete;
    ~SimulationEventChannel() override = default;

    [[nodiscard]] bool publish(const T& event) noexcept {
        if (pendingCount_ == Capacity) {
            overflowed_ = true;
            return false;
        }

        buffers_[pendingBuffer_][pendingCount_] = event;
        ++pendingCount_;
        return true;
    }

    [[nodiscard]] std::span<const T> events() const noexcept {
        if (currentCount_ == 0U) {
            return {};
        }
        return {buffers_[currentBuffer_].data(), currentCount_};
    }

    void reset() {
        if (transactionActive_) {
            throw std::logic_error("Cannot reset a simulation event channel during execution");
        }
        currentCount_ = 0U;
        pendingCount_ = 0U;
        checkpointPendingCount_ = 0U;
        overflowed_ = false;
        checkpointOverflowed_ = false;
    }

private:
    friend class WorldSystemScheduler;

    SimulationEventChannel() noexcept = default;

    void checkpoint() noexcept override {
        transactionActive_ = true;
        checkpointPendingCount_ = pendingCount_;
        checkpointOverflowed_ = overflowed_;
    }

    void rollback() noexcept override {
        pendingCount_ = checkpointPendingCount_;
        overflowed_ = checkpointOverflowed_;
        transactionActive_ = false;
    }

    void promote() noexcept override {
        const std::size_t retiredCurrentBuffer = currentBuffer_;
        currentBuffer_ = pendingBuffer_;
        currentCount_ = pendingCount_;
        pendingBuffer_ = retiredCurrentBuffer;
        pendingCount_ = 0U;
        overflowed_ = false;
        transactionActive_ = false;
    }

    [[nodiscard]] bool overflowed() const noexcept override {
        return overflowed_;
    }

    std::array<T, Capacity> buffers_[2]{};
    std::size_t currentBuffer_ = 0U;
    std::size_t pendingBuffer_ = 1U;
    std::size_t currentCount_ = 0U;
    std::size_t pendingCount_ = 0U;
    std::size_t checkpointPendingCount_ = 0U;
    bool overflowed_ = false;
    bool checkpointOverflowed_ = false;
    bool transactionActive_ = false;
};

} // namespace ve
