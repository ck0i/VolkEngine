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

struct EventPayload {
    int producer = 0;
    int value = 0;
};

static_assert(std::is_trivially_copyable_v<EventPayload>);

using EventChannel = ve::SimulationEventChannel<EventPayload, 2U>;

struct OtherEventPayload {
    int code = 0;
};

static_assert(std::is_trivially_copyable_v<OtherEventPayload>);

static_assert(!std::is_default_constructible_v<EventChannel>);
static_assert(!std::is_copy_constructible_v<EventChannel>);
static_assert(!std::is_move_constructible_v<EventChannel>);

struct EventReadContext {
    EventChannel* channel = nullptr;
    std::array<EventPayload, 16> observed{};
    std::size_t observedCount = 0U;
    bool publish = false;
    EventPayload produced{};
    bool publishSucceeded = false;
    bool throwNow = false;
};

void readAndMaybePublish(void* context,
                         ve::World&,
                         ve::WorldSystemScheduler::CommandWriter&,
                         const ve::InputState&,
                         double,
                         double) {
    auto& state = *static_cast<EventReadContext*>(context);
    for (const EventPayload event : state.channel->events()) {
        state.observed[state.observedCount++] = event;
    }
    if (state.publish) {
        state.publishSucceeded = state.channel->publish(state.produced);
    }
    if (state.throwNow) {
        state.throwNow = false;
        throw std::runtime_error("event system failure");
    }
}

struct EventProducerContext {
    EventChannel* channel = nullptr;
    EventPayload event{};
    bool publishSucceeded = false;
};

void publishEvent(void* context,
                  ve::World&,
                  ve::WorldSystemScheduler::CommandWriter&,
                  const ve::InputState&,
                  double,
                  double) {
    auto& state = *static_cast<EventProducerContext*>(context);
    state.publishSucceeded = state.channel->publish(state.event);
}

struct OverflowCommandContext {
    EventChannel* channel = nullptr;
    ve::World::Entity entity{};
};

void queueCommandAndOverflow(void* context,
                             ve::World&,
                             ve::WorldSystemScheduler::CommandWriter& commands,
                             const ve::InputState&,
                             double,
                             double) {
    const auto& state = *static_cast<OverflowCommandContext*>(context);
    commands.emplace<Health>(state.entity, Health{25});
    (void)state.channel->publish(EventPayload{1, 1});
    (void)state.channel->publish(EventPayload{1, 2});
    (void)state.channel->publish(EventPayload{1, 3});
}

struct PlaybackEventContext {
    EventChannel* channel = nullptr;
    ve::World::Entity entity{};
    std::array<EventPayload, 8> observed{};
    std::size_t observedCount = 0U;
    bool active = false;
};

void queueCommandAndPublishEvent(void* context,
                                 ve::World&,
                                 ve::WorldSystemScheduler::CommandWriter& commands,
                                 const ve::InputState&,
                                 double,
                                 double) {
    auto& state = *static_cast<PlaybackEventContext*>(context);
    for (const EventPayload event : state.channel->events()) {
        state.observed[state.observedCount++] = event;
    }
    if (!state.active) {
        return;
    }
    commands.emplace<Health>(state.entity, Health{50});
    (void)state.channel->publish(EventPayload{9, 2});
}

struct EventSchedulerGuardContext {
    ve::WorldSystemScheduler* scheduler = nullptr;
    EventChannel* channel = nullptr;
    ve::World* world = nullptr;
    bool recursiveRejected = false;
    bool creationRejected = false;
    bool reservationRejected = false;
    bool resetRejected = false;
};

void rejectEventSchedulerMutation(void* context,
                                  ve::World&,
                                  ve::WorldSystemScheduler::CommandWriter&,
                                  const ve::InputState&,
                                  double,
                                  double) {
    auto& state = *static_cast<EventSchedulerGuardContext*>(context);
    try {
        (void)state.scheduler->execute(*state.world, {}, 0.01, 0.01);
    } catch (const std::logic_error&) {
        state.recursiveRejected = true;
    }
    try {
        (void)state.scheduler->createEventChannel<OtherEventPayload, 2U>();
    } catch (const std::logic_error&) {
        state.creationRejected = true;
    }
    try {
        state.scheduler->reserveEventChannels(2U);
    } catch (const std::logic_error&) {
        state.reservationRejected = true;
    }
    try {
        state.channel->reset();
    } catch (const std::logic_error&) {
        state.resetRejected = true;
    }
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
        scheduler.reserveEventChannels(2U);
        auto& events = scheduler.createEventChannel<EventPayload, 2U>();
        auto& otherEvents = scheduler.createEventChannel<OtherEventPayload, 2U>();
        expectTrue("typed event channels initially expose empty current batches",
                   events.events().empty() && otherEvents.events().empty());
        expectTrue("event channel accepts FIFO payloads",
                   events.publish({1, 10}) && events.publish({2, 20}) && otherEvents.publish({30}));
        expectTrue("pending event payloads are not directly visible", events.events().empty() && otherEvents.events().empty());
        scheduler.compile();
        (void)scheduler.execute(world, {}, 0.01, 0.01);
        const std::span<const EventPayload> promoted = events.events();
        const std::span<const OtherEventPayload> promotedOther = otherEvents.events();
        expectTrue("successful promotion preserves typed FIFO payloads",
                   promoted.size() == 2U && promoted[0].producer == 1 && promoted[0].value == 10 &&
                       promoted[1].producer == 2 && promoted[1].value == 20 &&
                       promotedOther.size() == 1U && promotedOther[0].code == 30);
        (void)scheduler.execute(world, {}, 0.02, 0.01);
        expectTrue("current event batches live for exactly one successful step",
                   events.events().empty() && otherEvents.events().empty());
    }

    {
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveSystems(3U);
        scheduler.reserveEventChannels(1U);
        auto& events = scheduler.createEventChannel<EventPayload, 2U>();
        EventProducerContext first{&events, {1, 11}};
        EventProducerContext second{&events, {2, 22}};
        EventReadContext consumer{&events};
        scheduler.addSystem("first-producer", &publishEvent, &first);
        scheduler.addSystem("second-producer", &publishEvent, &second);
        scheduler.addSystem("consumer", &readAndMaybePublish, &consumer);
        scheduler.compile();
        (void)scheduler.execute(world, {}, 0.01, 0.01);
        expectTrue("systems cannot observe events published in the same step", consumer.observedCount == 0U);
        const auto firstBatch = events.events();
        expectTrue("multiple producers preserve deterministic registration FIFO order",
                   first.publishSucceeded && second.publishSucceeded && firstBatch.size() == 2U &&
                       firstBatch[0].producer == 1 && firstBatch[0].value == 11 &&
                       firstBatch[1].producer == 2 && firstBatch[1].value == 22);
        first.publishSucceeded = false;
        second.publishSucceeded = false;
        (void)scheduler.execute(world, {}, 0.02, 0.01);
        expectTrue("events are consumed on the next successful step only",
                   consumer.observedCount == 2U && consumer.observed[0].producer == 1 &&
                       consumer.observed[1].producer == 2);
    }

    {
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveSystems(1U);
        scheduler.reserveEventChannels(1U);
        auto& events = scheduler.createEventChannel<EventPayload, 2U>();
        EventReadContext consumer{&events};
        scheduler.addSystem("external-consumer", &readAndMaybePublish, &consumer);
        scheduler.compile();
        expectTrue("external pre-step publish succeeds", events.publish({7, 70}));
        (void)scheduler.execute(world, {}, 0.01, 0.01);
        expectTrue("external pre-step pending events remain invisible during that step",
                   consumer.observedCount == 0U && events.events().size() == 1U && events.events()[0].producer == 7);
        (void)scheduler.execute(world, {}, 0.02, 0.01);
        expectTrue("external pre-step events participate in the next successful step exactly once",
                   consumer.observedCount == 1U && consumer.observed[0].value == 70 && events.events().empty());
    }

    {
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveEventChannels(1U);
        auto& events = scheduler.createEventChannel<EventPayload, 2U>();
        scheduler.compile();
        expectTrue("event channel capacity reports overflow through publish",
                   events.publish({1, 1}) && events.publish({1, 2}) && !events.publish({1, 3}));
        expectThrows<std::overflow_error>("event overflow latch aborts the scheduler step", [&] {
            (void)scheduler.execute(world, {}, 0.01, 0.01);
        });
        expectTrue("overflow failure does not promote pending events", events.events().empty());
        expectThrows<std::overflow_error>("overflow latch remains observable until reset", [&] {
            (void)scheduler.execute(world, {}, 0.015, 0.01);
        });
        events.reset();
        expectTrue("event channel reset clears pending data and overflow latch", events.publish({3, 3}));
        (void)scheduler.execute(world, {}, 0.02, 0.01);
        expectTrue("reset channel promotes subsequent events", events.events().size() == 1U && events.events()[0].producer == 3);
    }

    {
        ve::World overflowWorld;
        const ve::World::Entity entity = overflowWorld.createEntity();
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveSystems(1U);
        scheduler.reserveEventChannels(1U);
        scheduler.reserveDeferredCommandSlots(1U);
        auto& events = scheduler.createEventChannel<EventPayload, 2U>();
        OverflowCommandContext context{&events, entity};
        scheduler.addSystem("overflow-before-playback", &queueCommandAndOverflow, &context);
        scheduler.compile();
        expectThrows<std::overflow_error>("channel overflow is detected before command playback", [&] {
            (void)scheduler.execute(overflowWorld, {}, 0.01, 0.01);
        });
        expectTrue("overflow discards deferred commands before world mutation",
                   !overflowWorld.contains<Health>(entity) && events.events().empty());
    }

    {
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveSystems(1U);
        scheduler.reserveEventChannels(1U);
        auto& events = scheduler.createEventChannel<EventPayload, 2U>();
        EventReadContext context{&events};
        scheduler.addSystem("rollback-reader", &readAndMaybePublish, &context);
        scheduler.compile();
        expectTrue("callback rollback seed event publishes", events.publish({4, 1}));
        (void)scheduler.execute(world, {}, 0.01, 0.01);
        context.publish = true;
        context.produced = {4, 2};
        context.throwNow = true;
        expectThrows<std::runtime_error>("callback failure rolls event channels back", [&] {
            (void)scheduler.execute(world, {}, 0.02, 0.01);
        });
        expectTrue("callback failure retains current events for retry",
                   events.events().size() == 1U && events.events()[0].value == 1);
        (void)scheduler.execute(world, {}, 0.03, 0.01);
        context.publish = false;
        expectTrue("retry consumes retained current event and promotes its replacement",
                   context.observedCount == 2U && context.observed[0].value == 1 && context.observed[1].value == 1 &&
                       events.events().size() == 1U && events.events()[0].value == 2);
        (void)scheduler.execute(world, {}, 0.04, 0.01);
        expectTrue("rolled-back pending events are neither lost nor duplicated",
                   context.observedCount == 3U && context.observed[2].value == 2 && events.events().empty());
    }

    {
        ve::World playbackWorld;
        const ve::World::Entity entity = playbackWorld.createEntity();
        playbackWorld.emplace<Position>(entity, 1);
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveSystems(1U);
        scheduler.reserveEventChannels(1U);
        scheduler.reserveDeferredCommandSlots(1U);
        auto& events = scheduler.createEventChannel<EventPayload, 2U>();
        PlaybackEventContext context{&events, entity};
        scheduler.addSystem("playback-rollback", &queueCommandAndPublishEvent, &context);
        scheduler.compile();
        expectTrue("playback rollback seed event publishes", events.publish({9, 1}));
        (void)scheduler.execute(playbackWorld, {}, 0.01, 0.01);
        context.active = true;
        playbackWorld.each<Position>([&](auto&&...) {
            expectThrows<std::logic_error>("playback failure promotes event transaction before rethrow", [&] {
                (void)scheduler.execute(playbackWorld, {}, 0.02, 0.01);
            });
        });
        expectTrue("playback failure consumes current events and promotes emitted events",
                   events.events().size() == 1U && events.events()[0].value == 2);
        context.active = false;
        (void)scheduler.execute(playbackWorld, {}, 0.03, 0.01);
        expectTrue("playback failure prevents retrying events against a partially committed world",
                   context.observedCount == 2U && context.observed[0].value == 1 && context.observed[1].value == 2 &&
                       events.events().empty());
    }

    {
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveEventChannels(2U);
        auto& events = scheduler.createEventChannel<EventPayload, 2U>();
        expectTrue("scheduler-owned event channel reference remains usable while scheduler lives", events.events().empty());
        scheduler.compile();
        auto& otherEvents = scheduler.createEventChannel<OtherEventPayload, 2U>();
        expectTrue("event channel creation invalidates compilation and retains factory ownership",
                   !scheduler.compiled() && otherEvents.events().empty());

        EventSchedulerGuardContext context{&scheduler, &events, &world};
        scheduler.addSystem("event-lifecycle-guard", &rejectEventSchedulerMutation, &context);
        scheduler.compile();
        expectTrue("reset guard seed event publishes", events.publish({5, 1}));
        (void)scheduler.execute(world, {}, 0.01, 0.01);
        expectTrue("reset guard pending batch and overflow latch are seeded",
                   events.publish({5, 2}) && events.publish({5, 3}) && !events.publish({5, 4}));
        expectThrows<std::overflow_error>("reset rejection preserves pending overflow state during callback", [&] {
            (void)scheduler.execute(world, {}, 0.02, 0.01);
        });
        expectTrue("recursive execution creation reservation and reset are rejected during execution",
                   context.recursiveRejected && context.creationRejected && context.reservationRejected && context.resetRejected);
        expectTrue("rejected reset preserves the pre-existing current batch",
                   events.events().size() == 1U && events.events()[0].value == 1);
        expectThrows<std::overflow_error>("rejected reset leaves the overflow latch intact", [&] {
            (void)scheduler.execute(world, {}, 0.03, 0.01);
        });
        events.reset();
        expectTrue("reset guard replacement pending event publishes", events.publish({5, 4}));
        (void)scheduler.execute(world, {}, 0.04, 0.01);
        expectTrue("rejected reset preserves pending events for successful promotion",
                   events.events().size() == 1U && events.events()[0].value == 4);
    }

    {
        ve::WorldSystemScheduler scheduler;
        scheduler.reserveSystems(1U);
        scheduler.reserveEventChannels(1U);
        auto& events = scheduler.createEventChannel<EventPayload, 2U>();
        EventReadContext context{&events};
        context.publish = true;
        context.produced = {8, 8};
        scheduler.addSystem("event-allocation-check", &readAndMaybePublish, &context);
        scheduler.compile();
        gAllocationCount = 0U;
        gTrackAllocations = true;
        const bool publishWithoutAllocation = events.publish({8, 7});
        (void)events.events();
        (void)scheduler.execute(world, {}, 0.01, 0.01);
        (void)events.events();
        gTrackAllocations = false;
        expectTrue("event publish read and compiled execution perform no allocations",
                   publishWithoutAllocation && context.publishSucceeded && gAllocationCount == 0U);
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
