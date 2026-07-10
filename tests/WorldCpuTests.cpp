#include "core/World.hpp"

#include <iostream>
#include <string_view>
#include <utility>

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

} // namespace

int main() {
    ve::World world;
    const ve::World::Entity first = world.createEntity();
    const ve::World::Entity second = world.createEntity();
    expectTrue("new entities are alive", world.alive(first) && world.alive(second));
    expectTrue("entity count tracks creation", world.entityCount() == 2U);

    world.emplace<Position>(first, 10);
    world.emplace<Health>(first, 80);
    world.emplace<Position>(second, 20);
    expectTrue("component lookup returns first entity data", world.tryGet<Position>(first) != nullptr && world.tryGet<Position>(first)->value == 10);
    expectTrue("component lookup returns second entity data", world.contains<Position>(second) && world.componentCount<Position>() == 2U);
    expectThrows("duplicate component insertion is rejected", [&] { world.emplace<Position>(first, 11); });

    int positionSum = 0;
    world.each<Position>([&](const ve::World::Entity, const Position& position) { positionSum += position.value; });
    expectTrue("dense component iteration visits every component", positionSum == 30);

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
    movedWorld.clear();
    expectTrue("clear invalidates all entity handles", !movedWorld.alive(recycled) && movedWorld.entityCount() == 0U && movedWorld.componentCount<Position>() == 0U);

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "World CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
