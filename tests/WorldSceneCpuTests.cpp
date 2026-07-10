#include "renderer/SceneRenderer.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace {

int gFailureCount = 0;

void expectTrue(const std::string_view context, const bool value) {
    if (!value) {
        std::cerr << "[FAILED] " << context << '\n';
        ++gFailureCount;
    }
}

void expectNearly(const std::string_view context, const float actual, const float expected, const float epsilon = 1.0e-5f) {
    if (std::fabs(actual - expected) > epsilon) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}

void expectVec3Nearly(const std::string_view context, const ve::Vec3 actual, const ve::Vec3 expected) {
    expectNearly(std::string(context) + " x", actual.x, expected.x);
    expectNearly(std::string(context) + " y", actual.y, expected.y);
    expectNearly(std::string(context) + " z", actual.z, expected.z);
}

ve::WorldSceneRenderable makeRenderable(const int marker, const ve::MeshBounds bounds, const bool visible = true) {
    ve::WorldSceneRenderable renderable{};
    renderable.material.flags.x = static_cast<float>(marker);
    renderable.localBounds = bounds;
    renderable.visible = visible;
    return renderable;
}

} // namespace

int main() {
    ve::World world;
    const ve::World::Entity first = world.createEntity();
    const ve::World::Entity second = world.createEntity();
    const ve::World::Entity third = world.createEntity();
    const ve::World::Entity invisible = world.createEntity();
    const ve::World::Entity invalid = world.createEntity();
    const ve::World::Entity missingTransform = world.createEntity();
    const ve::World::Entity nonFinite = world.createEntity();

    const ve::MeshBounds unitBounds{{0.5f, -1.0f, 2.0f}, 1.0f, true};
    world.emplace<ve::WorldSceneTransform>(
        first, ve::WorldSceneTransform{ve::TransformTRS{{10.0f, 2.0f, -4.0f}, {}, {2.0f, 3.0f, 4.0f}}});
    world.emplace<ve::WorldSceneRenderable>(first, makeRenderable(10, unitBounds));

    world.emplace<ve::WorldSceneTransform>(
        second, ve::WorldSceneTransform{ve::TransformTRS{{-5.0f, 0.0f, 0.0f}, {}, {2.0f, 1.0f, 1.0f}}});
    world.emplace<ve::WorldSceneRenderable>(second, makeRenderable(20, {{}, 2.0f, true}));

    world.emplace<ve::WorldSceneTransform>(
        third, ve::WorldSceneTransform{ve::TransformTRS{{2.0f, 0.0f, 1.0f}, {}, {1.0f, 1.0f, 1.0f}}});
    world.emplace<ve::WorldSceneRenderable>(third, makeRenderable(30, {{}, 0.5f, true}));

    world.emplace<ve::WorldSceneTransform>(invisible, ve::WorldSceneTransform{});
    world.emplace<ve::WorldSceneRenderable>(invisible, makeRenderable(40, unitBounds, false));
    world.emplace<ve::WorldSceneTransform>(invalid, ve::WorldSceneTransform{});
    world.emplace<ve::WorldSceneRenderable>(invalid, makeRenderable(50, {{}, -1.0f, true}));
    world.emplace<ve::WorldSceneTransform>(missingTransform, ve::WorldSceneTransform{});

    world.emplace<ve::WorldSceneTransform>(
        nonFinite,
        ve::WorldSceneTransform{ve::TransformTRS{{std::numeric_limits<float>::infinity(), 0.0f, 0.0f}}});
    world.emplace<ve::WorldSceneRenderable>(nonFinite, makeRenderable(60, unitBounds));
    ve::WorldSceneExtractor extractor;
    const ve::World& readOnlyWorld = world;
    const ve::SceneRenderList& firstList = extractor.build(readOnlyWorld, 0.0);
    expectTrue("extractor includes only visible entities with valid bounds and both components", firstList.size() == 3U);
    if (firstList.size() == 3U) {
        expectNearly("extractor sorts first item by entity index", firstList[0].material.flags.x, 10.0f);
        expectNearly("extractor sorts second item by entity index", firstList[1].material.flags.x, 20.0f);
        expectNearly("extractor sorts third item by entity index", firstList[2].material.flags.x, 30.0f);
        expectVec3Nearly("extractor transforms local bounds center", firstList[0].boundsCenter, {11.0f, -1.0f, 4.0f});
        expectNearly("extractor uses conservative non-uniform scale radius", firstList[0].boundsRadius, std::sqrt(29.0f));
        expectNearly("extractor uses conservative TRS radius", firstList[1].boundsRadius, 2.0f * std::sqrt(6.0f));
        expectVec3Nearly("extractor applies TRS translation", firstList[1].boundsCenter, {-5.0f, 0.0f, 0.0f});
    }

    expectTrue("destroying an extracted entity removes it", world.destroyEntity(second));
    const ve::World::Entity recycled = world.createEntity();
    expectTrue("destroyed extracted slot is recycled", recycled.index == second.index && recycled.generation != second.generation);
    world.emplace<ve::WorldSceneTransform>(recycled, ve::WorldSceneTransform{});
    world.emplace<ve::WorldSceneRenderable>(recycled, makeRenderable(40, {{}, 1.0f, true}));

    const std::size_t previousCapacity = firstList.capacity();
    const ve::SceneRenderList& secondList = extractor.build(world, 0.0);
    expectTrue("extractor reuses render list capacity", secondList.capacity() >= previousCapacity);
    expectTrue("extractor clears stale entities and keeps recycled entities", secondList.size() == 3U);
    if (secondList.size() == 3U) {
        expectNearly("extractor keeps first entity first after pool swap-remove", secondList[0].material.flags.x, 10.0f);
        expectNearly("extractor sorts recycled entity by index", secondList[1].material.flags.x, 40.0f);
        expectNearly("recycled generation resets stale interpolation history", secondList[1].model.m[12], 0.0f);
        expectNearly("extractor keeps third entity last after pool swap-remove", secondList[2].material.flags.x, 30.0f);
    }

    {
        ve::World interpolationWorld;
        const ve::World::Entity entity = interpolationWorld.createEntity();
        auto& transform = interpolationWorld.emplace<ve::WorldSceneTransform>(entity);
        transform.current = {{0.0f, 0.0f, 0.0f}, ve::rotationY(ve::radians(170.0f)), {1.0f, 1.0f, 1.0f}};
        interpolationWorld.emplace<ve::WorldSceneRenderable>(
            entity, makeRenderable(70, {{1.0f, 0.0f, 0.0f}, 1.0f, true}));

        ve::WorldSceneExtractor interpolationExtractor;
        (void)interpolationExtractor.build(interpolationWorld, 0.0);
        interpolationExtractor.prepareSimulationStep(interpolationWorld);
        transform.current = {{10.0f, 0.0f, 0.0f}, ve::rotationY(ve::radians(-170.0f)), {3.0f, 3.0f, 3.0f}};
        interpolationExtractor.captureSimulationStep(interpolationWorld);

        const ve::SceneRenderList& previous = interpolationExtractor.build(interpolationWorld, 0.0);
        expectNearly("interpolation alpha zero selects previous translation", previous[0].model.m[12], 0.0f);
        const ve::SceneRenderList& current = interpolationExtractor.build(interpolationWorld, 1.0);
        expectNearly("interpolation alpha one selects current translation", current[0].model.m[12], 10.0f);
        const ve::SceneRenderList& extremeAlpha =
            interpolationExtractor.build(interpolationWorld, std::numeric_limits<double>::max());
        expectNearly("extreme finite interpolation alpha clamps before narrowing",
                     extremeAlpha[0].model.m[12],
                     10.0f);
        const ve::SceneRenderList& halfway = interpolationExtractor.build(interpolationWorld, 0.5);
        expectNearly("interpolation midpoint blends translation", halfway[0].model.m[12], 5.0f);
        expectNearly("interpolation midpoint blends scale", halfway[0].model.m[5], 2.0f, 1.0e-4f);
        expectNearly("shortest-path quaternion interpolation crosses 180 degrees", halfway[0].model.m[0], -2.0f, 1.0e-4f);
        expectVec3Nearly("interpolated bounds use submitted midpoint model", halfway[0].boundsCenter, {3.0f, 0.0f, 0.0f});
        expectNearly("interpolated bounds use midpoint scale", halfway[0].boundsRadius, std::sqrt(12.0f), 1.0e-4f);

        transform.teleport({{100.0f, 0.0f, 0.0f}, {}, {1.0f, 1.0f, 1.0f}});
        interpolationExtractor.prepareSimulationStep(interpolationWorld);
        const ve::SceneRenderList& teleported = interpolationExtractor.build(interpolationWorld, 0.5);
        expectNearly("teleport resets interpolation history", teleported[0].model.m[12], 100.0f);

        interpolationExtractor.prepareSimulationStep(interpolationWorld);
        transform.current.translation.x = 110.0f;
        interpolationExtractor.captureSimulationStep(interpolationWorld);
        interpolationExtractor.prepareSimulationStep(interpolationWorld);
        transform.current.translation.x = 120.0f;
        interpolationExtractor.captureSimulationStep(interpolationWorld);
        const ve::SceneRenderList& multiStep = interpolationExtractor.build(interpolationWorld, 0.5);
        expectNearly("multiple simulation steps retain penultimate and latest poses", multiStep[0].model.m[12], 115.0f);
        const ve::SceneRenderList& zeroStep = interpolationExtractor.build(interpolationWorld, 0.25);
        expectNearly("zero-step render reuses captured history", zeroStep[0].model.m[12], 112.5f);

        ve::World otherWorld;
        const ve::World::Entity otherEntity = otherWorld.createEntity();
        auto& otherTransform = otherWorld.emplace<ve::WorldSceneTransform>(otherEntity);
        otherTransform.current.translation.x = 500.0f;
        otherWorld.emplace<ve::WorldSceneRenderable>(
            otherEntity, makeRenderable(80, {{}, 1.0f, true}));
        const ve::SceneRenderList& otherList = interpolationExtractor.build(otherWorld, 0.5);
        expectNearly("extractor histories do not leak across World instances",
                     otherList[0].model.m[12],
                     500.0f);

        interpolationExtractor.prepareSimulationStep(otherWorld);
        otherTransform.current.translation.x = 600.0f;
        interpolationExtractor.invalidateSimulationState();
        const ve::SceneRenderList& invalidated = interpolationExtractor.build(otherWorld, 0.5);
        expectNearly("invalidated history cannot smear an aborted update",
                     invalidated[0].model.m[12],
                     600.0f);

        otherTransform.current.translation.x = 700.0f;
        interpolationExtractor.resetSimulationState(otherWorld);
        const ve::SceneRenderList& reset = interpolationExtractor.build(otherWorld, 0.5);
        expectNearly("explicit history reset snaps to current pose", reset[0].model.m[12], 700.0f);
    }

    {
        alignas(ve::World) std::byte worldStorage[sizeof(ve::World)]{};
        ve::World* firstLifetime = std::construct_at(reinterpret_cast<ve::World*>(worldStorage));
        const std::uint64_t firstToken = firstLifetime->instanceToken();
        const ve::World::Entity firstEntity = firstLifetime->createEntity();
        auto& firstTransform = firstLifetime->emplace<ve::WorldSceneTransform>(firstEntity);
        firstLifetime->emplace<ve::WorldSceneRenderable>(
            firstEntity, makeRenderable(90, {{}, 1.0f, true}));
        ve::WorldSceneExtractor lifetimeExtractor;
        (void)lifetimeExtractor.build(*firstLifetime, 0.0);
        lifetimeExtractor.prepareSimulationStep(*firstLifetime);
        firstTransform.current.translation.x = 20.0f;
        lifetimeExtractor.captureSimulationStep(*firstLifetime);
        std::destroy_at(firstLifetime);

        ve::World* secondLifetime = std::construct_at(reinterpret_cast<ve::World*>(worldStorage));
        const ve::World::Entity secondEntity = secondLifetime->createEntity();
        auto& secondTransform = secondLifetime->emplace<ve::WorldSceneTransform>(secondEntity);
        secondTransform.current.translation.x = 100.0f;
        secondLifetime->emplace<ve::WorldSceneRenderable>(
            secondEntity, makeRenderable(100, {{}, 1.0f, true}));
        const ve::SceneRenderList& reusedAddress = lifetimeExtractor.build(*secondLifetime, 0.5);
        expectTrue("World lifetime token changes when storage address is reused",
                   secondLifetime->instanceToken() != firstToken);
        expectNearly("reused World address cannot inherit prior lifetime history",
                     reusedAddress[0].model.m[12],
                     100.0f);
        std::destroy_at(secondLifetime);
    }

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "World scene CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
