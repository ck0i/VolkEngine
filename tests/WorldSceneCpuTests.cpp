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
    if (!std::isfinite(actual) || !std::isfinite(expected) || !std::isfinite(epsilon) ||
        std::fabs(actual - expected) > epsilon) {
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
        ve::World hierarchyWorld;
        const ve::World::Entity root = hierarchyWorld.createEntity();
        const ve::World::Entity child = hierarchyWorld.createEntity();
        const ve::World::Entity grandchild = hierarchyWorld.createEntity();
        const ve::TransformTRS rootLocal{{10.0f, 0.0f, 0.0f}, {}, {2.0f, 3.0f, 4.0f}};
        const ve::TransformTRS childLocal{
            {1.0f, 0.0f, 0.0f}, ve::rotationY(ve::radians(90.0f)), {1.0f, 1.0f, 1.0f}};
        const ve::TransformTRS grandchildLocal{{0.0f, 0.0f, 1.0f}, {}, {1.0f, 1.0f, 1.0f}};
        hierarchyWorld.emplace<ve::WorldSceneTransform>(root, ve::WorldSceneTransform{rootLocal});
        hierarchyWorld.emplace<ve::WorldSceneTransform>(child, ve::WorldSceneTransform{childLocal});
        hierarchyWorld.emplace<ve::WorldSceneTransform>(grandchild, ve::WorldSceneTransform{grandchildLocal});
        hierarchyWorld.emplace<ve::WorldSceneParent>(child);
        hierarchyWorld.tryGet<ve::WorldSceneParent>(child)->parent = root;
        hierarchyWorld.emplace<ve::WorldSceneParent>(grandchild, ve::WorldSceneParent{child});
        hierarchyWorld.emplace<ve::WorldSceneRenderable>(child, makeRenderable(110, {{}, 1.0f, true}));
        hierarchyWorld.emplace<ve::WorldSceneRenderable>(grandchild, makeRenderable(120, {{}, 1.0f, true}));

        ve::WorldSceneExtractor hierarchyExtractor;
        const ve::SceneRenderList& hierarchyList = hierarchyExtractor.build(hierarchyWorld, 0.0);
        expectTrue("non-renderable parent still resolves two-level and three-level children", hierarchyList.size() == 2U);
        if (hierarchyList.size() == 2U) {
            const ve::Mat4 expectedChild = ve::compose(rootLocal) * ve::compose(childLocal);
            const ve::Mat4 expectedGrandchild = expectedChild * ve::compose(grandchildLocal);
            expectNearly("two-level hierarchy remains sorted by child entity", hierarchyList[0].material.flags.x, 110.0f);
            expectNearly("three-level hierarchy remains sorted by grandchild entity", hierarchyList[1].material.flags.x, 120.0f);
            for (std::size_t i = 0; i < expectedChild.m.size(); ++i) {
                expectNearly("two-level hierarchy uses parent matrix times local matrix",
                             hierarchyList[0].model.m[i],
                             expectedChild.m[i]);
                expectNearly("three-level hierarchy uses accumulated parent matrix times local matrix",
                             hierarchyList[1].model.m[i],
                             expectedGrandchild.m[i]);
            }
        }
    }

    {
        ve::World identityNodeWorld;
        const ve::World::Entity root = identityNodeWorld.createEntity();
        const ve::World::Entity identityNode = identityNodeWorld.createEntity();
        const ve::World::Entity child = identityNodeWorld.createEntity();
        identityNodeWorld.emplace<ve::WorldSceneTransform>(
            root, ve::WorldSceneTransform{ve::TransformTRS{{5.0f, 0.0f, 0.0f}}});
        identityNodeWorld.emplace<ve::WorldSceneParent>(identityNode, ve::WorldSceneParent{root});
        identityNodeWorld.emplace<ve::WorldSceneTransform>(
            child, ve::WorldSceneTransform{ve::TransformTRS{{2.0f, 0.0f, 0.0f}}});
        identityNodeWorld.emplace<ve::WorldSceneParent>(child, ve::WorldSceneParent{identityNode});
        identityNodeWorld.emplace<ve::WorldSceneRenderable>(child, makeRenderable(125, {{}, 1.0f, true}));

        ve::WorldSceneExtractor identityNodeExtractor;
        const ve::SceneRenderList& identityNodeList = identityNodeExtractor.build(identityNodeWorld, 0.0);
        expectTrue("transformless hierarchy node contributes identity local transform", identityNodeList.size() == 1U);
        if (identityNodeList.size() == 1U) {
            expectNearly("transformless hierarchy node still propagates its parent",
                         identityNodeList[0].model.m[12],
                         7.0f);
        }
    }

    {
        constexpr std::size_t kHierarchyDepth = 4096U;
        ve::World deepWorld;
        deepWorld.reserveEntities(kHierarchyDepth);
        deepWorld.reserveComponents<ve::WorldSceneTransform>(kHierarchyDepth);
        deepWorld.reserveComponents<ve::WorldSceneParent>(kHierarchyDepth - 1U);
        ve::World::Entity previous = deepWorld.createEntity();
        deepWorld.emplace<ve::WorldSceneTransform>(
            previous, ve::WorldSceneTransform{ve::TransformTRS{{1.0f, 0.0f, 0.0f}}});
        for (std::size_t depth = 1U; depth < kHierarchyDepth; ++depth) {
            const ve::World::Entity current = deepWorld.createEntity();
            deepWorld.emplace<ve::WorldSceneTransform>(
                current, ve::WorldSceneTransform{ve::TransformTRS{{1.0f, 0.0f, 0.0f}}});
            deepWorld.emplace<ve::WorldSceneParent>(current, ve::WorldSceneParent{previous});
            previous = current;
        }
        deepWorld.emplace<ve::WorldSceneRenderable>(previous, makeRenderable(127, {{}, 1.0f, true}));

        ve::WorldSceneExtractor deepExtractor;
        const ve::SceneRenderList& deepList = deepExtractor.build(deepWorld, 0.0);
        expectTrue("iterative hierarchy resolver handles deep parent chains", deepList.size() == 1U);
        if (deepList.size() == 1U) {
            expectNearly("deep hierarchy accumulates every local transform",
                         deepList[0].model.m[12],
                         static_cast<float>(kHierarchyDepth));
        }
    }

    {
        ve::World interpolatedHierarchyWorld;
        const ve::World::Entity parent = interpolatedHierarchyWorld.createEntity();
        const ve::World::Entity child = interpolatedHierarchyWorld.createEntity();
        interpolatedHierarchyWorld.emplace<ve::WorldSceneTransform>(parent);
        interpolatedHierarchyWorld.emplace<ve::WorldSceneTransform>(child);
        auto& parentTransform = *interpolatedHierarchyWorld.tryGet<ve::WorldSceneTransform>(parent);
        auto& childTransform = *interpolatedHierarchyWorld.tryGet<ve::WorldSceneTransform>(child);
        interpolatedHierarchyWorld.emplace<ve::WorldSceneParent>(child, ve::WorldSceneParent{parent});
        interpolatedHierarchyWorld.emplace<ve::WorldSceneRenderable>(child, makeRenderable(130, {{}, 1.0f, true}));

        ve::WorldSceneExtractor interpolationExtractor;
        (void)interpolationExtractor.build(interpolatedHierarchyWorld, 0.0);
        interpolationExtractor.prepareSimulationStep(interpolatedHierarchyWorld);
        parentTransform.current.translation.x = 10.0f;
        childTransform.current.translation.x = 4.0f;
        interpolationExtractor.captureSimulationStep(interpolatedHierarchyWorld);
        const ve::SceneRenderList& midpoint = interpolationExtractor.build(interpolatedHierarchyWorld, 0.5);
        expectTrue("interpolated hierarchy keeps child renderable", midpoint.size() == 1U);
        if (midpoint.size() == 1U) {
            expectNearly("parent and child local poses interpolate before hierarchy multiplication",
                         midpoint[0].model.m[12],
                         7.0f);
        }

        parentTransform.teleport({{100.0f, 0.0f, 0.0f}, {}, {1.0f, 1.0f, 1.0f}});
        const ve::SceneRenderList& parentTeleported = interpolationExtractor.build(interpolatedHierarchyWorld, 0.5);
        expectTrue("parent teleport preserves child renderable", parentTeleported.size() == 1U);
        if (parentTeleported.size() == 1U) {
            expectNearly("parent teleport snaps inherited parent motion while child remains locally interpolated",
                         parentTeleported[0].model.m[12],
                         102.0f);
        }
    }

    {
        ve::World staleParentWorld;
        const ve::World::Entity parent = staleParentWorld.createEntity();
        const ve::World::Entity child = staleParentWorld.createEntity();
        staleParentWorld.emplace<ve::WorldSceneTransform>(
            parent, ve::WorldSceneTransform{ve::TransformTRS{{50.0f, 0.0f, 0.0f}}});
        staleParentWorld.emplace<ve::WorldSceneTransform>(
            child, ve::WorldSceneTransform{ve::TransformTRS{{7.0f, 0.0f, 0.0f}}});
        staleParentWorld.emplace<ve::WorldSceneParent>(child, ve::WorldSceneParent{parent});
        staleParentWorld.emplace<ve::WorldSceneRenderable>(child, makeRenderable(140, {{}, 1.0f, true}));
        expectTrue("hierarchy test destroys parent", staleParentWorld.destroyEntity(parent));
        ve::WorldSceneExtractor staleParentExtractor;
        const ve::SceneRenderList& destroyedParentList = staleParentExtractor.build(staleParentWorld, 0.0);
        expectTrue("destroyed parent policy keeps child as a root renderable", destroyedParentList.size() == 1U);
        if (destroyedParentList.size() == 1U) {
            expectNearly("destroyed parent resolves child with root local transform",
                         destroyedParentList[0].model.m[12],
                         7.0f);
        }

        const ve::World::Entity recycledParent = staleParentWorld.createEntity();
        expectTrue("recycled parent slot changes generation",
                   recycledParent.index == parent.index && recycledParent.generation != parent.generation);
        staleParentWorld.emplace<ve::WorldSceneTransform>(
            recycledParent, ve::WorldSceneTransform{ve::TransformTRS{{100.0f, 0.0f, 0.0f}}});
        const ve::SceneRenderList& staleParentList = staleParentExtractor.build(staleParentWorld, 0.0);
        expectTrue("generation-stale parent policy keeps child as a root renderable", staleParentList.size() == 1U);
        if (staleParentList.size() == 1U) {
            expectNearly("dead or generation-stale parent link resolves child with root local transform",
                         staleParentList[0].model.m[12],
                         7.0f);
        }
    }

    {
        ve::World componentLifetimeWorld;
        const ve::World::Entity parent = componentLifetimeWorld.createEntity();
        const ve::World::Entity child = componentLifetimeWorld.createEntity();
        componentLifetimeWorld.emplace<ve::WorldSceneTransform>(
            parent, ve::WorldSceneTransform{ve::TransformTRS{{10.0f, 0.0f, 0.0f}}});
        componentLifetimeWorld.emplace<ve::WorldSceneTransform>(
            child, ve::WorldSceneTransform{ve::TransformTRS{{1.0f, 0.0f, 0.0f}}});
        componentLifetimeWorld.emplace<ve::WorldSceneParent>(child, ve::WorldSceneParent{parent});
        componentLifetimeWorld.emplace<ve::WorldSceneRenderable>(child, makeRenderable(145, {{}, 1.0f, true}));

        ve::WorldSceneExtractor componentLifetimeExtractor;
        (void)componentLifetimeExtractor.build(componentLifetimeWorld, 0.0);
        componentLifetimeExtractor.prepareSimulationStep(componentLifetimeWorld);
        expectTrue("hierarchy test removes parent transform",
                   componentLifetimeWorld.remove<ve::WorldSceneTransform>(parent));
        componentLifetimeExtractor.captureSimulationStep(componentLifetimeWorld);
        componentLifetimeWorld.emplace<ve::WorldSceneTransform>(
            parent, ve::WorldSceneTransform{ve::TransformTRS{{100.0f, 0.0f, 0.0f}}});
        componentLifetimeExtractor.prepareSimulationStep(componentLifetimeWorld);
        componentLifetimeExtractor.captureSimulationStep(componentLifetimeWorld);

        const ve::SceneRenderList& readdedTransformList =
            componentLifetimeExtractor.build(componentLifetimeWorld, 0.5);
        expectTrue("re-added parent transform keeps child renderable", readdedTransformList.size() == 1U);
        if (readdedTransformList.size() == 1U) {
            expectNearly("re-added parent transform starts fresh interpolation history",
                         readdedTransformList[0].model.m[12],
                         101.0f);
        }
    }

    {
        ve::World cycleWorld;
        const ve::World::Entity cycleA = cycleWorld.createEntity();
        const ve::World::Entity cycleB = cycleWorld.createEntity();
        const ve::World::Entity cycleDescendant = cycleWorld.createEntity();
        const ve::World::Entity selfCycle = cycleWorld.createEntity();
        const ve::World::Entity unrelated = cycleWorld.createEntity();
        cycleWorld.emplace<ve::WorldSceneTransform>(cycleA);
        cycleWorld.emplace<ve::WorldSceneTransform>(cycleB);
        cycleWorld.emplace<ve::WorldSceneTransform>(cycleDescendant);
        cycleWorld.emplace<ve::WorldSceneTransform>(selfCycle);
        cycleWorld.emplace<ve::WorldSceneTransform>(
            unrelated, ve::WorldSceneTransform{ve::TransformTRS{{9.0f, 0.0f, 0.0f}}});
        cycleWorld.emplace<ve::WorldSceneParent>(cycleA, ve::WorldSceneParent{cycleB});
        cycleWorld.emplace<ve::WorldSceneParent>(cycleB, ve::WorldSceneParent{cycleA});
        cycleWorld.emplace<ve::WorldSceneParent>(cycleDescendant, ve::WorldSceneParent{cycleA});
        cycleWorld.emplace<ve::WorldSceneParent>(selfCycle, ve::WorldSceneParent{selfCycle});
        cycleWorld.emplace<ve::WorldSceneRenderable>(cycleA, makeRenderable(150, {{}, 1.0f, true}));
        cycleWorld.emplace<ve::WorldSceneRenderable>(cycleDescendant, makeRenderable(160, {{}, 1.0f, true}));
        cycleWorld.emplace<ve::WorldSceneRenderable>(selfCycle, makeRenderable(170, {{}, 1.0f, true}));
        cycleWorld.emplace<ve::WorldSceneRenderable>(unrelated, makeRenderable(180, {{}, 1.0f, true}));

        ve::WorldSceneExtractor cycleExtractor;
        const ve::SceneRenderList& cycleList = cycleExtractor.build(cycleWorld, 0.0);
        expectTrue("self links, cycles, and their descendants are omitted while unrelated renderables survive",
                   cycleList.size() == 1U);
        if (cycleList.size() == 1U) {
            expectNearly("cycle omission leaves unrelated entity deterministically rendered",
                         cycleList[0].material.flags.x,
                         180.0f);
            expectNearly("unrelated renderable retains its root transform after cycle omission",
                         cycleList[0].model.m[12],
                         9.0f);
        }
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
