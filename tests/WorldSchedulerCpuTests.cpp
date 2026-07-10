#include "core/WorldScheduler.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {

static_assert(!std::is_copy_constructible_v<ve::WorldSystemScheduler>);
static_assert(!std::is_copy_assignable_v<ve::WorldSystemScheduler>);
static_assert(!std::is_move_constructible_v<ve::WorldSystemScheduler>);
static_assert(!std::is_move_assignable_v<ve::WorldSystemScheduler>);
static_assert(!std::is_copy_constructible_v<ve::WorldSystemScheduler::CommandWriter>);
static_assert(!std::is_copy_assignable_v<ve::WorldSystemScheduler::CommandWriter>);
static_assert(!std::is_move_constructible_v<ve::WorldSystemScheduler::CommandWriter>);
static_assert(!std::is_move_assignable_v<ve::WorldSystemScheduler::CommandWriter>);

template <typename T>
concept HasPlayback = requires(T& commands, ve::World& world) {
    commands.playback(world);
};

template <typename T>
concept HasDiscard = requires(T& commands) {
    commands.discard();
};

static_assert(!HasPlayback<ve::WorldSystemScheduler::CommandWriter>);
static_assert(!HasDiscard<ve::WorldSystemScheduler::CommandWriter>);

bool gTrackAllocations = false;
std::size_t gAllocationCount = 0U;

} // namespace

void* operator new(const std::size_t size) {
    if (gTrackAllocations) {
        ++gAllocationCount;
    }
    if (void* allocation = std::malloc(size); allocation != nullptr) {
        return allocation;
    }
    throw std::bad_alloc{};
}

void operator delete(void* allocation) noexcept {
    std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

namespace {

int gFailureCount = 0;

void expectTrue(const std::string_view context, const bool value) {
    if (!value) {
        std::cerr << "[FAILED] " << context << '\n';
        ++gFailureCount;
    }
}

template <typename Exception, typename Function>
void expectThrows(const std::string_view context, Function&& function) {
    try {
        function();
        std::cerr << "[FAILED] " << context << ": expected exception\n";
        ++gFailureCount;
    } catch (const Exception&) {
        // expected
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": unexpected exception type\n";
        ++gFailureCount;
    }
}

struct Position {
    int value = 0;
};

struct Health {
    int value = 0;
};

struct Trace {
    std::array<int, 32> order{};
    std::size_t count = 0U;
    double elapsed = 0.0;
    double delta = 0.0;
    bool pressed = false;
    bool held = false;
};

struct TraceSystem {
    Trace* trace = nullptr;
    int id = 0;
};

void recordSystem(void* context,
                  ve::World&,
                  ve::WorldSystemScheduler::CommandWriter&,
                  const ve::InputState& input,
                  const double elapsed,
                  const double delta) {
    auto& system = *static_cast<TraceSystem*>(context);
    system.trace->order[system.trace->count++] = system.id;
    system.trace->elapsed = elapsed;
    system.trace->delta = delta;
    system.trace->pressed = input.pressed(ve::InputKey::Space);
    system.trace->held = input.held(ve::InputKey::Space);
}

struct DeferredContext {
    ve::World::Entity entity{};
    bool laterSystemSawPosition = false;
};

void queueRemoval(void* context,
                  ve::World&,
                  ve::WorldSystemScheduler::CommandWriter& commands,
                  const ve::InputState&,
                  double,
                  double) {
    const auto& state = *static_cast<DeferredContext*>(context);
    commands.remove<Position>(state.entity);
}

void observeBeforePlayback(void* context,
                           ve::World& world,
                           ve::WorldSystemScheduler::CommandWriter&,
                           const ve::InputState&,
                           double,
                           double) {
    auto& state = *static_cast<DeferredContext*>(context);
    state.laterSystemSawPosition = world.contains<Position>(state.entity);
}

struct FailureContext {
    ve::World::Entity entity{};
    bool recordCommand = true;
    bool throwNow = true;
};

void recordOnce(void* context,
                ve::World&,
                ve::WorldSystemScheduler::CommandWriter& commands,
                const ve::InputState&,
                double,
                double) {
    auto& state = *static_cast<FailureContext*>(context);
    if (state.recordCommand) {
        commands.emplace<Health>(state.entity, Health{75});
        state.recordCommand = false;
    }
}

void throwOnce(void* context,
               ve::World&,
               ve::WorldSystemScheduler::CommandWriter&,
               const ve::InputState&,
               double,
               double) {
    auto& state = *static_cast<FailureContext*>(context);
    if (state.throwNow) {
        state.throwNow = false;
        throw std::runtime_error("system failure");
    }
}

struct MutationContext {
    ve::WorldSystemScheduler* scheduler = nullptr;
};

void noOpSystem(void*, ve::World&, ve::WorldSystemScheduler::CommandWriter&, const ve::InputState&, double, double) {}

void mutateScheduler(void* context,
                     ve::World&,
                     ve::WorldSystemScheduler::CommandWriter&,
                     const ve::InputState&,
                     double,
                     double) {
    auto& state = *static_cast<MutationContext*>(context);
    state.scheduler->addSystem("illegal", &noOpSystem);
}

} // namespace

int main() {
    ve::World world;
    const ve::InputState input = [] {
        ve::InputTracker tracker;
        tracker.keyEvent(ve::InputKey::Space, true);
        return tracker.consume();
    }();

    {
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveSystems(3U);
        Trace trace;
        TraceSystem c{&trace, 3};
        TraceSystem a{&trace, 1};
        TraceSystem b{&trace, 2};
        const std::array<std::string_view, 1> cDependencies{"a"};
        scheduler.addSystem({"c", &recordSystem, &c, cDependencies});
        scheduler.addSystem({"a", &recordSystem, &a});
        scheduler.addSystem({"b", &recordSystem, &b});
        expectTrue("registration invalidates scheduler plan", !scheduler.compiled());
        scheduler.compile();
        expectTrue("scheduler compiles registered systems", scheduler.compiled());
        expectTrue("execution order exposes every compiled system", scheduler.executionOrder().size() == 3U);

        const ve::WorldSystemScheduler::ExecutionResult result = scheduler.execute(world, input, 0.02, 0.01);
        expectTrue("stable dependency order uses registration index tie break",
                   trace.count == 3U && trace.order[0] == 1 && trace.order[1] == 3 && trace.order[2] == 2);
        expectTrue("scheduler forwards fixed-step timing and input",
                   std::fabs(trace.elapsed - 0.02) < 1.0e-12 && std::fabs(trace.delta - 0.01) < 1.0e-12 &&
                       trace.pressed && trace.held);
        expectTrue("empty deferred playback reports no mutations", result.applied == 0U && result.rejected == 0U);

        const std::size_t systemCount = scheduler.systemCount();
        expectThrows<std::invalid_argument>("duplicate system names are rejected atomically", [&] {
            scheduler.addSystem("a", &noOpSystem);
        });
        expectTrue("rejected duplicate preserves compiled plan and registry",
                   scheduler.systemCount() == systemCount && scheduler.compiled());
    }

    {
        ve::WorldSystemScheduler invalid;
        expectThrows<std::invalid_argument>("empty system name is rejected", [&] {
            invalid.addSystem("", &noOpSystem);
        });
        expectThrows<std::invalid_argument>("null system callback is rejected", [&] {
            invalid.addSystem("null", nullptr);
        });
        const std::array<std::string_view, 2> duplicateDependencies{"missing", "missing"};
        expectThrows<std::invalid_argument>("duplicate dependency names are rejected", [&] {
            invalid.addSystem({"duplicate-dependency", &noOpSystem, nullptr, duplicateDependencies});
        });
        const std::array<std::string_view, 1> emptyDependency{""};
        expectThrows<std::invalid_argument>("empty dependency name is rejected", [&] {
            invalid.addSystem({"empty-dependency", &noOpSystem, nullptr, emptyDependency});
        });
        expectTrue("invalid registrations leave scheduler empty", invalid.empty());
    }

    {
        ve::WorldSystemScheduler missing;
        const std::array<std::string_view, 1> dependencies{"not-registered"};
        missing.addSystem({"dependent", &noOpSystem, nullptr, dependencies});
        expectThrows<std::invalid_argument>("compile rejects missing dependencies", [&] { missing.compile(); });
        expectTrue("missing dependency compile publishes no plan",
                   !missing.compiled() && missing.executionOrder().empty());

        ve::WorldSystemScheduler cycle;
        const std::array<std::string_view, 1> afterB{"b"};
        const std::array<std::string_view, 1> afterA{"a"};
        cycle.addSystem({"a", &noOpSystem, nullptr, afterB});
        cycle.addSystem({"b", &noOpSystem, nullptr, afterA});
        expectThrows<std::runtime_error>("compile rejects dependency cycle", [&] { cycle.compile(); });
        expectTrue("cyclic compile publishes no plan", !cycle.compiled() && cycle.executionOrder().empty());
    }

    {
        ve::WorldSystemScheduler scheduler;
        expectThrows<std::logic_error>("execute requires compiled plan", [&] {
            (void)scheduler.execute(world, {}, 0.01, 0.01);
        });
        scheduler.addSystem("noop", &noOpSystem);
        scheduler.compile();
        expectThrows<std::invalid_argument>("execute rejects negative elapsed time", [&] {
            (void)scheduler.execute(world, {}, -0.01, 0.01);
        });
        expectThrows<std::invalid_argument>("execute rejects non-positive delta", [&] {
            (void)scheduler.execute(world, {}, 0.01, 0.0);
        });
        expectThrows<std::invalid_argument>("execute rejects non-finite timing", [&] {
            (void)scheduler.execute(world, {}, 0.01, std::numeric_limits<double>::infinity());
        });
    }

    {
        ve::World commandWorld;
        const ve::World::Entity entity = commandWorld.createEntity();
        commandWorld.emplace<Position>(entity, 5);
        DeferredContext context{entity};
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveSystems(2U);
        scheduler.reserveDeferredCommandSlots(1U);
        scheduler.addSystem("remove", &queueRemoval, &context);
        const std::array<std::string_view, 1> dependencies{"remove"};
        scheduler.addSystem({"observe", &observeBeforePlayback, &context, dependencies});
        scheduler.compile();
        const auto result = scheduler.execute(commandWorld, {}, 0.01, 0.01);
        expectTrue("deferred structure remains visible to all systems in the step", context.laterSystemSawPosition);
        expectTrue("scheduler plays deferred commands after all systems",
                   result.applied == 1U && result.rejected == 0U && !commandWorld.contains<Position>(entity));
    }

    {
        ve::World failureWorld;
        const ve::World::Entity entity = failureWorld.createEntity();
        FailureContext context{entity};
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveSystems(2U);
        scheduler.reserveDeferredCommandSlots(1U);
        scheduler.addSystem("record", &recordOnce, &context);
        const std::array<std::string_view, 1> dependencies{"record"};
        scheduler.addSystem({"throw", &throwOnce, &context, dependencies});
        scheduler.compile();
        expectThrows<std::runtime_error>("system exception propagates", [&] {
            (void)scheduler.execute(failureWorld, {}, 0.01, 0.01);
        });
        expectTrue("failed step discards deferred mutations", !failureWorld.contains<Health>(entity));
        const auto resumed = scheduler.execute(failureWorld, {}, 0.02, 0.01);
        expectTrue("discarded command cannot leak into a later step",
                   resumed.applied == 0U && resumed.rejected == 0U && !failureWorld.contains<Health>(entity));
    }

    {
        ve::WorldSystemScheduler scheduler;
        MutationContext context{&scheduler};
        scheduler.addSystem("mutate", &mutateScheduler, &context);
        scheduler.compile();
        expectThrows<std::logic_error>("scheduler mutation during execution is rejected", [&] {
            (void)scheduler.execute(world, {}, 0.01, 0.01);
        });
        expectTrue("execution failure preserves compiled system plan", scheduler.compiled() && scheduler.systemCount() == 1U);
    }

    {
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveSystems(1U);
        Trace trace;
        TraceSystem system{&trace, 1};
        scheduler.addSystem("allocation-check", &recordSystem, &system);
        scheduler.compile();
        gAllocationCount = 0U;
        gTrackAllocations = true;
        (void)scheduler.execute(world, {}, 0.01, 0.01);
        gTrackAllocations = false;
        expectTrue("compiled scheduler execution performs no allocations", gAllocationCount == 0U);
    }

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "World scheduler CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
