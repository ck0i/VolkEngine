#pragma once

#include "core/SimulationEvents.hpp"
#include "core/SimulationTimers.hpp"

#include "core/World.hpp"
#include "platform/Input.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ve {

class WorldSystemScheduler final {
public:
    class CommandWriter final {
    public:
        CommandWriter(const CommandWriter&) = delete;
        CommandWriter& operator=(const CommandWriter&) = delete;
        CommandWriter(CommandWriter&&) = delete;
        CommandWriter& operator=(CommandWriter&&) = delete;
        ~CommandWriter() = default;

        void destroy(const World::Entity entity) {
            commands_->destroy(entity);
        }

        template <typename T>
        void remove(const World::Entity entity) {
            commands_->remove<T>(entity);
        }

        template <typename T>
        void emplace(const World::Entity entity, T ownedValue) {
            commands_->emplace<T>(entity, std::move(ownedValue));
        }

    private:
        friend class WorldSystemScheduler;
        explicit CommandWriter(WorldCommandBuffer& commands) noexcept : commands_(&commands) {}
        WorldCommandBuffer* commands_ = nullptr;
    };

    using Callback = void (*)(void* context,
                              World& world,
                              CommandWriter& commands,
                              const InputState& input,
                              double elapsed,
                              double delta);
    using ExecutionResult = WorldCommandBuffer::PlaybackResult;

    struct SystemDesc {
        std::string_view name;
        Callback callback = nullptr;
        void* context = nullptr;
        std::span<const std::string_view> dependencies{};
    };

    WorldSystemScheduler() = default;
    WorldSystemScheduler(const WorldSystemScheduler&) = delete;
    WorldSystemScheduler& operator=(const WorldSystemScheduler&) = delete;
    WorldSystemScheduler(WorldSystemScheduler&&) = delete;
    WorldSystemScheduler& operator=(WorldSystemScheduler&&) = delete;
    ~WorldSystemScheduler() = default;

    void reserveSystems(const std::size_t systemCount) {
        ensureNotExecuting();
        systems_.reserve(systemCount);
        executionOrder_.reserve(systemCount);
        indegrees_.reserve(systemCount);
        emitted_.reserve(systemCount);
    }

    void reserveSimulationResources(const std::size_t resourceCount) {
        ensureNotExecuting();
        simulationResources_.reserve(resourceCount);
    }

    template <typename T, std::size_t Capacity>
    [[nodiscard]] SimulationEventChannel<T, Capacity>& createEventChannel() {
        ensureNotExecuting();

        auto channel = std::unique_ptr<SimulationEventChannel<T, Capacity>>(
            new SimulationEventChannel<T, Capacity>());
        SimulationEventChannel<T, Capacity>& reference = *channel;
        auto resource = std::unique_ptr<detail::SimulationStepResource>(
            static_cast<detail::SimulationStepResource*>(channel.release()));
        simulationResources_.push_back(std::move(resource));
        invalidateCompilation();
        return reference;
    }

    template <typename T, std::size_t Capacity>
    [[nodiscard]] SimulationTimerQueue<T, Capacity>& createTimerQueue() {
        ensureNotExecuting();

        auto queue = std::unique_ptr<SimulationTimerQueue<T, Capacity>>(
            new SimulationTimerQueue<T, Capacity>());
        SimulationTimerQueue<T, Capacity>& reference = *queue;
        auto resource = std::unique_ptr<detail::SimulationStepResource>(
            static_cast<detail::SimulationStepResource*>(queue.release()));
        simulationResources_.push_back(std::move(resource));
        invalidateCompilation();
        return reference;
    }
    void reserveDeferredCommandSlots(const std::size_t commandCount) {
        ensureNotExecuting();
        commands_.reserve(commandCount);
    }

    void addSystem(const SystemDesc& descriptor) {
        ensureNotExecuting();
        validateDescriptor(descriptor);

        System system;
        system.name.assign(descriptor.name.data(), descriptor.name.size());
        system.callback = descriptor.callback;
        system.context = descriptor.context;
        system.dependencies.reserve(descriptor.dependencies.size());
        for (const std::string_view dependency : descriptor.dependencies) {
            system.dependencies.emplace_back(dependency.data(), dependency.size());
        }

        systems_.push_back(std::move(system));
        invalidateCompilation();
    }

    void addSystem(const std::string_view name,
                   const Callback callback,
                   void* const context = nullptr,
                   const std::span<const std::string_view> dependencies = {}) {
        addSystem(SystemDesc{name, callback, context, dependencies});
    }


    void clear() {
        ensureNotExecuting();
        systems_.clear();
        commands_.discard();
        invalidateCompilation();
    }

    void compile() {
        ensureNotExecuting();
        invalidateCompilation();

        try {
            const std::size_t count = systems_.size();
            indegrees_.assign(count, 0U);
            emitted_.assign(count, 0U);
            executionOrder_.reserve(count);

            for (std::size_t systemIndex = 0U; systemIndex < count; ++systemIndex) {
                const System& system = systems_[systemIndex];
                for (const std::string& dependency : system.dependencies) {
                    if (findSystem(dependency) == nullptr) {
                        throw std::invalid_argument("World system dependency is not registered: " + dependency);
                    }
                    ++indegrees_[systemIndex];
                }
            }

            for (std::size_t emittedCount = 0U; emittedCount < count; ++emittedCount) {
                std::size_t next = count;
                for (std::size_t candidate = 0U; candidate < count; ++candidate) {
                    if (emitted_[candidate] == 0U && indegrees_[candidate] == 0U) {
                        next = candidate;
                        break;
                    }
                }
                if (next == count) {
                    throw std::runtime_error("World system dependency cycle detected");
                }

                emitted_[next] = 1U;
                executionOrder_.push_back(next);
                const std::string& completedName = systems_[next].name;
                for (std::size_t dependent = 0U; dependent < count; ++dependent) {
                    if (emitted_[dependent] != 0U) {
                        continue;
                    }
                    for (const std::string& dependency : systems_[dependent].dependencies) {
                        if (dependency == completedName) {
                            --indegrees_[dependent];
                        }
                    }
                }
            }
            compiled_ = true;
        } catch (...) {
            executionOrder_.clear();
            compiled_ = false;
            throw;
        }
    }

    [[nodiscard]] ExecutionResult execute(World& world,
                                          const InputState& input,
                                          const double elapsed,
                                          const double delta) {
        if (!compiled_) {
            throw std::logic_error("World system scheduler must be compiled before execution");
        }
        if (executing_) {
            throw std::logic_error("Recursive World system scheduler execution is forbidden");
        }
        if (!std::isfinite(elapsed) || elapsed < 0.0 ||
            !std::isfinite(delta) || delta <= 0.0) {
            throw std::invalid_argument("World system timing must be finite, with non-negative elapsed time and positive delta time");
        }

        executing_ = true;
        for (const std::unique_ptr<detail::SimulationStepResource>& resource : simulationResources_) {
            resource->checkpoint();
        }

        CommandWriter commandWriter{commands_};
        try {
            ensureSimulationResourcesWithinCapacity();
            for (const std::size_t systemIndex : executionOrder_) {
                const System& system = systems_[systemIndex];
                system.callback(system.context, world, commandWriter, input, elapsed, delta);
            }
            ensureSimulationResourcesWithinCapacity();
        } catch (...) {
            commands_.discard();
            for (const std::unique_ptr<detail::SimulationStepResource>& resource : simulationResources_) {
                resource->rollback();
            }
            executing_ = false;
            throw;
        }

        try {
            ExecutionResult result = commands_.playback(world);
            for (const std::unique_ptr<detail::SimulationStepResource>& resource : simulationResources_) {
                resource->promote();
            }
            executing_ = false;
            return result;
        } catch (...) {
            commands_.discard();
            for (const std::unique_ptr<detail::SimulationStepResource>& resource : simulationResources_) {
                resource->promote();
            }
            executing_ = false;
            throw;
        }
    }

    [[nodiscard]] bool compiled() const noexcept { return compiled_; }
    [[nodiscard]] std::size_t systemCount() const noexcept { return systems_.size(); }
    [[nodiscard]] bool empty() const noexcept { return systems_.empty(); }
    [[nodiscard]] std::span<const std::size_t> executionOrder() const noexcept { return executionOrder_; }

private:
    struct System {
        std::string name;
        Callback callback = nullptr;
        void* context = nullptr;
        std::vector<std::string> dependencies;
    };

    void validateDescriptor(const SystemDesc& descriptor) const {
        if (descriptor.name.empty()) {
            throw std::invalid_argument("World system name must not be empty");
        }
        if (descriptor.callback == nullptr) {
            throw std::invalid_argument("World system callback must not be null");
        }
        if (findSystem(descriptor.name) != nullptr) {
            throw std::invalid_argument("World system name is already registered");
        }
        for (std::size_t dependencyIndex = 0U; dependencyIndex < descriptor.dependencies.size(); ++dependencyIndex) {
            const std::string_view dependency = descriptor.dependencies[dependencyIndex];
            if (dependency.empty()) {
                throw std::invalid_argument("World system dependency name must not be empty");
            }
            for (std::size_t previous = 0U; previous < dependencyIndex; ++previous) {
                if (dependency == descriptor.dependencies[previous]) {
                    throw std::invalid_argument("World system dependencies must not contain duplicates");
                }
            }
        }
    }

    [[nodiscard]] const System* findSystem(const std::string_view name) const noexcept {
        for (const System& system : systems_) {
            if (system.name == name) {
                return &system;
            }
        }
        return nullptr;
    }

    void ensureSimulationResourcesWithinCapacity() const {
        for (const std::unique_ptr<detail::SimulationStepResource>& resource : simulationResources_) {
            if (resource->overflowed()) {
                throw std::overflow_error("Simulation step resource capacity exceeded");
            }
        }
    }

    void ensureNotExecuting() const {
        if (executing_) {
            throw std::logic_error("World system scheduler mutation during execution is forbidden");
        }
    }

    void invalidateCompilation() noexcept {
        executionOrder_.clear();
        compiled_ = false;
    }

    std::vector<System> systems_;
    std::vector<std::size_t> executionOrder_;
    std::vector<std::size_t> indegrees_;
    std::vector<std::uint8_t> emitted_;
    std::vector<std::unique_ptr<detail::SimulationStepResource>> simulationResources_;
    WorldCommandBuffer commands_;
    bool compiled_ = false;
    bool executing_ = false;
};

} // namespace ve
