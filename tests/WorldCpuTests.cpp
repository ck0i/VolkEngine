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

struct Armor {
    int value = 0;
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
    movedWorld.clear();
    expectTrue("clear invalidates all entity handles", !movedWorld.alive(recycled) && movedWorld.entityCount() == 0U && movedWorld.componentCount<Position>() == 0U);

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "World CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
