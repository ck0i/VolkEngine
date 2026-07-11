#pragma once

#include <cstddef>

namespace ve {

class WorldSystemScheduler;
template <typename T, std::size_t Capacity>
class SimulationEventChannel;
template <typename T, std::size_t Capacity>
class SimulationTimerQueue;

namespace detail {

class SimulationStepResource {
private:
    constexpr SimulationStepResource() noexcept = default;

    virtual void checkpoint() noexcept = 0;
    virtual void rollback() noexcept = 0;
    virtual void promote() noexcept = 0;
    [[nodiscard]] virtual bool overflowed() const noexcept = 0;

    friend class ::ve::WorldSystemScheduler;
    template <typename T, std::size_t Capacity>
    friend class ::ve::SimulationEventChannel;
    template <typename T, std::size_t Capacity>
    friend class ::ve::SimulationTimerQueue;

public:
    virtual ~SimulationStepResource() = default;
};

} // namespace detail
} // namespace ve
