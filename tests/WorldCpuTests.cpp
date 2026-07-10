#include "core/World.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

namespace {
bool gFailNextAllocation = false;
}

void* operator new(const std::size_t size) {
    if (gFailNextAllocation) {
        gFailNextAllocation = false;
        throw std::bad_alloc{};
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

template <typename F>
void expectThrows(const std::string_view context, F&& function) {
    try {
        function();
        std::cerr << "[FAILED] " << context << ": expected exception\n";
        ++gFailureCount;
    } catch (const std::logic_error&) {
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
    int value = 100;
};

struct Armor {
    int value = 0;
};

struct MoveOnlyPayload {
    std::unique_ptr<int> value;
};

struct ReentrantPayload {
    ve::WorldCommandBuffer* commands = nullptr;
    ve::World::Entity target{};

    ReentrantPayload() = default;
    ReentrantPayload(ve::WorldCommandBuffer* commandBuffer, const ve::World::Entity targetEntity) noexcept
        : commands(commandBuffer), target(targetEntity) {}
    ReentrantPayload(const ReentrantPayload&) = delete;
    ReentrantPayload& operator=(const ReentrantPayload&) = delete;
    ReentrantPayload(ReentrantPayload&& other) noexcept : commands(other.commands), target(other.target) {
        enqueueOnMove(other);
    }
    ReentrantPayload& operator=(ReentrantPayload&& other) noexcept {
        commands = other.commands;
        target = other.target;
        enqueueOnMove(other);
        return *this;
    }

    static inline bool enqueueEnabled = false;

private:
    static void enqueueOnMove(ReentrantPayload& source) noexcept {
        if (enqueueEnabled) {
            enqueueEnabled = false;
            source.commands->remove<Armor>(source.target);
        }
    }
};

struct RecursivePlaybackPayload {
    ve::WorldCommandBuffer* commands = nullptr;
    ve::World* world = nullptr;

    RecursivePlaybackPayload() = default;
    RecursivePlaybackPayload(ve::WorldCommandBuffer* commandBuffer, ve::World* targetWorld) noexcept
        : commands(commandBuffer), world(targetWorld) {}
    RecursivePlaybackPayload(const RecursivePlaybackPayload&) = delete;
    RecursivePlaybackPayload& operator=(const RecursivePlaybackPayload&) = delete;
    RecursivePlaybackPayload(RecursivePlaybackPayload&& other) noexcept
        : commands(other.commands), world(other.world) {
        attemptPlayback();
    }
    RecursivePlaybackPayload& operator=(RecursivePlaybackPayload&& other) noexcept {
        commands = other.commands;
        world = other.world;
        attemptPlayback();
        return *this;
    }

    static inline bool playbackEnabled = false;
    static inline bool playbackRejected = false;
    static inline bool observedEmpty = false;
    static inline std::size_t observedSize = 0U;

private:
    void attemptPlayback() noexcept {
        if (!playbackEnabled) {
            return;
        }
        playbackEnabled = false;
        observedEmpty = commands->empty();
        observedSize = commands->size();
        try {
            (void)commands->playback(*world);
        } catch (const std::logic_error&) {
            playbackRejected = true;
        } catch (...) {
        }
    }
};

struct LvaluePositionVisitor {
    int* sum = nullptr;

    void operator()(const ve::World::Entity, const Position& position) & {
        *sum += position.value;
    }
};

} // namespace

int main() {
    ve::World world;
    const ve::World::Entity first = world.createEntity();
    const ve::World::Entity second = world.createEntity();
    expectTrue("new entities are alive", world.alive(first) && world.alive(second));
    expectTrue("entity count tracks creation", world.entityCount() == 2U);

    {
        ve::World reservedWorld;
        reservedWorld.reserveEntities(32U);
        reservedWorld.reserveComponents<Position>(16U);
        expectTrue("entity reservation grows slot capacity", reservedWorld.entityCapacity() >= 32U);
        expectTrue("component reservation grows dense capacity", reservedWorld.componentCapacity<Position>() >= 16U);
        const ve::World::Entity reservedEntity = reservedWorld.createEntity();
        reservedWorld.emplace<Position>(reservedEntity, 42);
        expectTrue("reserved component storage remains usable", reservedWorld.tryGet<Position>(reservedEntity)->value == 42);
        reservedWorld.reserveEntities(1U);
        reservedWorld.reserveComponents<Position>(1U);
        expectTrue("smaller reservations never shrink capacity", reservedWorld.entityCapacity() >= 32U &&
                                                                    reservedWorld.componentCapacity<Position>() >= 16U);
        if constexpr (sizeof(std::size_t) > sizeof(ve::World::Index)) {
            expectThrows("entity reservation rejects index overflow", [&] {
                reservedWorld.reserveEntities(static_cast<std::size_t>(ve::World::kInvalidIndex) + 1U);
            });
        }
    }

    world.emplace<Position>(first, 10);
    world.emplace<Health>(first, 80);
    world.emplace<Armor>(first, 7);
    world.emplace<Position>(second, 20);
    world.emplace<Armor>(second, 8);
    expectTrue("component lookup returns first entity data", world.tryGet<Position>(first) != nullptr && world.tryGet<Position>(first)->value == 10);
    expectTrue("component lookup returns second entity data", world.contains<Position>(second) && world.componentCount<Position>() == 2U);
    expectThrows("duplicate component insertion is rejected", [&] { world.emplace<Position>(first, 11); });

    int positionSum = 0;
    world.each<Position>([&](const ve::World::Entity, const Position& position) { positionSum += position.value; });
    expectTrue("dense component iteration visits every component", positionSum == 30);

    const ve::World& readOnlyWorld = world;
    int readOnlyPositionSum = 0;
    readOnlyWorld.each<Position>([&](const ve::World::Entity, const Position& position) { readOnlyPositionSum += position.value; });
    expectTrue("const world iteration exposes dense component data", readOnlyPositionSum == 30);
    int lvalueVisitorSum = 0;
    readOnlyWorld.each<Position>(LvaluePositionVisitor{&lvalueVisitorSum});
    expectTrue("const iteration supports lvalue-qualified visitors", lvalueVisitorSum == 30);
    int matchedPositionSum = 0;
    int matchedHealthSum = 0;
    world.each<Position, Health>([&](const ve::World::Entity, Position& position, Health& health) {
        matchedPositionSum += position.value;
        matchedHealthSum += health.value;
    });
    int threePositionSum = 0;
    int threeHealthSum = 0;
    int threeArmorSum = 0;
    world.each<Position, Health, Armor>([&](const ve::World::Entity, Position& position, Health& health, Armor& armor) {
        threePositionSum += position.value;
        threeHealthSum += health.value;
        threeArmorSum += armor.value;
    });
    expectTrue("three-component query preserves template callback order and joins the middle-sized pool",
               threePositionSum == 10 && threeHealthSum == 80 && threeArmorSum == 7);

    expectTrue("two-component query joins only matching entities", matchedPositionSum == 10 && matchedHealthSum == 80);
    const ve::World& constQueryWorld = world;
    int constQueryCount = 0;
    constQueryWorld.each<Position, Health>([&](const ve::World::Entity, const Position& position, const Health& health) {
        constQueryCount += position.value == 10 && health.value == 80 ? 1 : 0;
    });
    expectTrue("const two-component query exposes read-only matches", constQueryCount == 1);

    int constThreeComponentCount = 0;
    constQueryWorld.each<Position, Health, Armor>(
        [&](const ve::World::Entity, const Position& position, const Health& health, const Armor& armor) {
            constThreeComponentCount += position.value == 10 && health.value == 80 && armor.value == 7 ? 1 : 0;
        });
    expectTrue("const three-component query exposes read-only matches", constThreeComponentCount == 1);

    {
        ve::World guardedWorld;
        const ve::World::Entity guardedFirst = guardedWorld.createEntity();
        const ve::World::Entity guardedSecond = guardedWorld.createEntity();
        guardedWorld.emplace<Position>(guardedFirst, 1);
        guardedWorld.emplace<Position>(guardedSecond, 2);

        expectThrows("query rejects entity destruction", [&] {
            guardedWorld.each<Position>([&](const ve::World::Entity entity, Position&) {
                (void)guardedWorld.destroyEntity(entity);
            });
        });
        expectThrows("query rejects entity creation", [&] {
            guardedWorld.each<Position>([&](const ve::World::Entity, Position&) {
                (void)guardedWorld.createEntity();
            });
        });
        expectThrows("query rejects component insertion", [&] {
            guardedWorld.each<Position>([&](const ve::World::Entity entity, Position&) {
                guardedWorld.emplace<Health>(entity, 40);
            });
        });
        expectThrows("query rejects component removal", [&] {
            guardedWorld.each<Position>([&](const ve::World::Entity entity, Position&) {
                (void)guardedWorld.remove<Position>(entity);
            });
        });
        expectThrows("query rejects entity reservation", [&] {
            guardedWorld.each<Position>([&](const ve::World::Entity, Position&) {
                guardedWorld.reserveEntities(64U);
            });
        });
        expectThrows("query rejects component reservation", [&] {
            guardedWorld.each<Position>([&](const ve::World::Entity, Position&) {
                guardedWorld.reserveComponents<Health>(8U);
            });
        });
        expectThrows("query rejects clear", [&] {
            guardedWorld.each<Position>([&](const ve::World::Entity, Position&) {
                guardedWorld.clear();
            });
        });
        expectThrows("query rejects move assignment", [&] {
            guardedWorld.each<Position>([&](const ve::World::Entity, Position&) {
                guardedWorld = ve::World{};
            });
        });
        expectThrows("query rejects move construction", [&] {
            guardedWorld.each<Position>([&](const ve::World::Entity, Position&) {
                ve::World moved{std::move(guardedWorld)};
                (void)moved;
            });
        });
        expectTrue("rejected query mutations preserve world", guardedWorld.entityCount() == 2U &&
                                                               guardedWorld.componentCount<Position>() == 2U &&
                                                               guardedWorld.alive(guardedFirst) &&
                                                               guardedWorld.alive(guardedSecond));

        const ve::World& constGuardedWorld = guardedWorld;
        expectThrows("const query rejects mutation through alias", [&] {
            constGuardedWorld.each<Position>([&](const ve::World::Entity entity, const Position&) {
                (void)guardedWorld.destroyEntity(entity);
            });
        });

        int nestedVisitCount = 0;
        int nestedInnerVisitCount = 0;
        guardedWorld.each<Position>([&](const ve::World::Entity, Position& position) {
            position.value += 10;
            ++nestedVisitCount;
            guardedWorld.each<Position>([&](const ve::World::Entity, Position&) { ++nestedInnerVisitCount; });
        });
        expectTrue("nested queries permit component writes", nestedVisitCount == 2 && nestedInnerVisitCount == 4 &&
                                                           guardedWorld.tryGet<Position>(guardedFirst)->value == 11);

        expectThrows("query guard unwinds callback exceptions", [&] {
            guardedWorld.each<Position>([](const ve::World::Entity, Position&) {
                throw std::logic_error("callback failure");
            });
        });
        expectTrue("mutation works after callback exception", guardedWorld.remove<Position>(guardedFirst) &&
                                                               guardedWorld.componentCount<Position>() == 1U);
    }
    {
        ve::World commandWorld;
        ve::WorldCommandBuffer commands;
        const ve::World::Entity firstCommandEntity = commandWorld.createEntity();
        const ve::World::Entity secondCommandEntity = commandWorld.createEntity();
        commandWorld.emplace<Position>(firstCommandEntity, 10);
        commandWorld.emplace<Armor>(firstCommandEntity, 3);
        commandWorld.emplace<Position>(secondCommandEntity, 20);

        commandWorld.each<Position>([&](const ve::World::Entity entity, Position&) {
            if (entity == firstCommandEntity) {
                commands.remove<Armor>(entity);
                commands.emplace<Health>(entity, Health{25});
            } else {
                commands.destroy(entity);
            }
            expectTrue("deferred mutations remain invisible during a query",
                       commandWorld.alive(entity) && commandWorld.contains<Position>(entity));
        });
        expectTrue("query records every deferred mutation", commands.size() == 3U);
        const ve::WorldCommandBuffer::PlaybackResult applied = commands.playback(commandWorld);
        expectTrue("deferred playback reports applied commands",
                   applied.applied == 3U && applied.rejected == 0U && commands.empty());
        expectTrue("deferred component changes apply after playback",
                   !commandWorld.contains<Armor>(firstCommandEntity) &&
                       commandWorld.tryGet<Health>(firstCommandEntity) != nullptr &&
                       commandWorld.tryGet<Health>(firstCommandEntity)->value == 25);
        expectTrue("deferred destruction applies after playback", !commandWorld.alive(secondCommandEntity));

        commands.destroy(firstCommandEntity);
        commands.remove<Position>(firstCommandEntity);
        commands.emplace<Armor>(firstCommandEntity, Armor{9});
        const ve::WorldCommandBuffer::PlaybackResult conflicts = commands.playback(commandWorld);
        expectTrue("FIFO destruction rejects later commands for the stale handle",
                   conflicts.applied == 1U && conflicts.rejected == 2U && !commandWorld.alive(firstCommandEntity));

        const ve::World::Entity recycledCommandEntity = commandWorld.createEntity();
        expectTrue("deferred stale handle slot is recycled with a new generation",
                   recycledCommandEntity.index == firstCommandEntity.index &&
                       recycledCommandEntity.generation != firstCommandEntity.generation);
        commands.emplace<Position>(firstCommandEntity, Position{99});
        const ve::WorldCommandBuffer::PlaybackResult stale = commands.playback(commandWorld);
        expectTrue("deferred stale generation cannot mutate recycled entity",
                   stale.applied == 0U && stale.rejected == 1U &&
                       !commandWorld.contains<Position>(recycledCommandEntity));

        commands.emplace<MoveOnlyPayload>(
            recycledCommandEntity, MoveOnlyPayload{std::make_unique<int>(77)});
        const ve::WorldCommandBuffer::PlaybackResult owned = commands.playback(commandWorld);
        const MoveOnlyPayload* payload = commandWorld.tryGet<MoveOnlyPayload>(recycledCommandEntity);
        expectTrue("deferred component command owns move-only payload",
                   owned.applied == 1U && payload != nullptr && payload->value != nullptr &&
                       *payload->value == 77);

        commandWorld.emplace<Armor>(recycledCommandEntity, 12);
        commands.emplace<ReentrantPayload>(
            recycledCommandEntity, ReentrantPayload{&commands, recycledCommandEntity});
        ReentrantPayload::enqueueEnabled = true;
        const ve::WorldCommandBuffer::PlaybackResult reentrant = commands.playback(commandWorld);
        expectTrue("commands appended during playback wait for the next batch",
                   reentrant.applied == 1U && commands.size() == 1U &&
                       commandWorld.contains<Armor>(recycledCommandEntity));
        const ve::WorldCommandBuffer::PlaybackResult appended = commands.playback(commandWorld);
        expectTrue("next playback applies reentrant command",
                   appended.applied == 1U && appended.rejected == 0U && commands.empty() &&
                       !commandWorld.contains<Armor>(recycledCommandEntity));

        commands.emplace<RecursivePlaybackPayload>(
            recycledCommandEntity, RecursivePlaybackPayload{&commands, &commandWorld});
        RecursivePlaybackPayload::playbackRejected = false;
        RecursivePlaybackPayload::observedEmpty = false;
        RecursivePlaybackPayload::observedSize = 1U;
        RecursivePlaybackPayload::playbackEnabled = true;
        const ve::WorldCommandBuffer::PlaybackResult recursive = commands.playback(commandWorld);
        expectTrue("recursive playback is rejected without disturbing outer batch",
                   recursive.applied == 1U && RecursivePlaybackPayload::playbackRejected &&
                       RecursivePlaybackPayload::observedEmpty &&
                       RecursivePlaybackPayload::observedSize == 0U && commands.empty() &&
                       commandWorld.contains<RecursivePlaybackPayload>(recycledCommandEntity));

        ve::World failureWorld;
        const ve::World::Entity failureEntity = failureWorld.createEntity();
        failureWorld.emplace<Position>(failureEntity, 5);
        ve::WorldCommandBuffer failingCommands;
        failingCommands.emplace<Health>(failureEntity, Health{60});
        failingCommands.remove<Position>(failureEntity);
        bool allocationFailurePropagated = false;
        gFailNextAllocation = true;
        try {
            (void)failingCommands.playback(failureWorld);
        } catch (const std::bad_alloc&) {
            allocationFailurePropagated = true;
        }
        expectTrue("throwing command is consumed and leaves FIFO tail pending",
                   allocationFailurePropagated && failingCommands.size() == 1U &&
                       failureWorld.contains<Position>(failureEntity) &&
                       !failureWorld.contains<Health>(failureEntity));

        ve::WorldCommandBuffer resumedCommands{std::move(failingCommands)};
        expectTrue("moving paused buffer resets moved-from cursor",
                   failingCommands.empty() && failingCommands.size() == 0U && resumedCommands.size() == 1U);
        const ve::WorldCommandBuffer::PlaybackResult resumed = resumedCommands.playback(failureWorld);
        expectTrue("moved paused buffer resumes unattempted FIFO tail",
                   resumed.applied == 1U && resumed.rejected == 0U && resumedCommands.empty() &&
                       !failureWorld.contains<Position>(failureEntity));

        ve::WorldCommandBuffer emptyCommands;
        expectThrows("empty command playback is rejected during a query", [&] {
            commandWorld.each<MoveOnlyPayload>([&](const ve::World::Entity, MoveOnlyPayload&) {
                (void)emptyCommands.playback(commandWorld);
            });
        });
        expectTrue("query rejection leaves empty command buffer reusable",
                   emptyCommands.playback(commandWorld).applied == 0U && emptyCommands.empty());
    }
    {
        ve::World smallerFirstWorld;
        const ve::World::Entity queryFirst = smallerFirstWorld.createEntity();
        const ve::World::Entity querySecond = smallerFirstWorld.createEntity();
        smallerFirstWorld.emplace<Position>(queryFirst, 3);
        smallerFirstWorld.emplace<Health>(queryFirst, 30);
        smallerFirstWorld.emplace<Health>(querySecond, 40);
        int smallerFirstPositionSum = 0;
        int smallerFirstHealthSum = 0;
        smallerFirstWorld.each<Position, Health>([&](const ve::World::Entity, Position& position, Health& health) {
            smallerFirstPositionSum += position.value;
            smallerFirstHealthSum += health.value;
        });
        expectTrue("two-component query preserves callback order when first pool is smaller",
                   smallerFirstPositionSum == 3 && smallerFirstHealthSum == 30);
    }

    expectTrue("component removal succeeds", world.remove<Health>(first));
    expectTrue("component removal clears lookup", !world.contains<Health>(first) && world.componentCount<Health>() == 0U);
    expectTrue("missing component removal is harmless", !world.remove<Health>(second));

    expectTrue("destroy removes all components", world.destroyEntity(first));
    expectTrue("swap-remove preserves the moved component lookup", world.tryGet<Position>(second) != nullptr && world.tryGet<Position>(second)->value == 20);
    expectTrue("destroy invalidates entity", !world.alive(first) && !world.contains<Position>(first));
    expectTrue("stale destroy is harmless", !world.destroyEntity(first));
    expectTrue("entity count tracks destruction", world.entityCount() == 1U);

    const ve::World::Entity recycled = world.createEntity();
    expectTrue("destroyed slot is recycled", recycled.index == first.index);
    expectTrue("recycled slot receives a new generation", recycled.generation != first.generation && !world.alive(first));
    world.emplace<Position>(recycled, 30);
    expectTrue("recycled entity has independent component storage", world.tryGet<Position>(recycled) != nullptr && world.tryGet<Position>(recycled)->value == 30);

    {
        ve::World clearWorld;
        const ve::World::Entity preClear = clearWorld.createEntity();
        clearWorld.clear();
        expectTrue("clear immediately invalidates the old handle", !clearWorld.alive(preClear));
        const ve::World::Entity postClear = clearWorld.createEntity();
        expectTrue("clear prevents handle resurrection after slot reuse", !clearWorld.alive(preClear) && postClear.generation != preClear.generation);
    }
    ve::World movedWorld = std::move(world);
    expectTrue("moving a world preserves entity handles", movedWorld.alive(recycled) && movedWorld.tryGet<Position>(recycled)->value == 30);
    expectTrue("moved-from world is empty", world.entityCount() == 0U && world.componentCount<Position>() == 0U);
    const ve::World::Entity movedFromReplacement = world.createEntity();
    expectTrue("moved-from world can be reused", world.entityCount() == 1U && world.alive(movedFromReplacement));
    movedWorld.clear();
    expectTrue("clear invalidates all entity handles", !movedWorld.alive(recycled) && movedWorld.entityCount() == 0U && movedWorld.componentCount<Position>() == 0U);

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "World CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
