#include "renderer/SceneRenderer.hpp"

#include <cmath>
#include <string>
#include <iostream>
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
    const ve::World::Entity projective = world.createEntity();

    const ve::MeshBounds unitBounds{{0.5f, -1.0f, 2.0f}, 1.0f, true};
    world.emplace<ve::WorldSceneTransform>(first, ve::WorldSceneTransform{ve::translate({10.0f, 2.0f, -4.0f}) * ve::scale({2.0f, 3.0f, 4.0f})});
    world.emplace<ve::WorldSceneRenderable>(first, makeRenderable(10, unitBounds));

    ve::Mat4 shear = ve::Mat4::identity();
    shear.m[4] = 2.0f;
    shear.m[12] = -5.0f;
    world.emplace<ve::WorldSceneTransform>(second, ve::WorldSceneTransform{shear});
    world.emplace<ve::WorldSceneRenderable>(second, makeRenderable(20, {{}, 2.0f, true}));

    world.emplace<ve::WorldSceneTransform>(third, ve::WorldSceneTransform{ve::translate({2.0f, 0.0f, 1.0f})});
    world.emplace<ve::WorldSceneRenderable>(third, makeRenderable(30, {{}, 0.5f, true}));

    world.emplace<ve::WorldSceneTransform>(invisible, ve::WorldSceneTransform{});
    world.emplace<ve::WorldSceneRenderable>(invisible, makeRenderable(40, unitBounds, false));
    world.emplace<ve::WorldSceneTransform>(invalid, ve::WorldSceneTransform{});
    world.emplace<ve::WorldSceneRenderable>(invalid, makeRenderable(50, {{}, -1.0f, true}));
    world.emplace<ve::WorldSceneTransform>(missingTransform, ve::WorldSceneTransform{});

    world.emplace<ve::WorldSceneTransform>(projective, ve::WorldSceneTransform{ve::perspective(ve::radians(60.0f), 1.0f, 0.1f, 100.0f)});
    world.emplace<ve::WorldSceneRenderable>(projective, makeRenderable(60, unitBounds));
    ve::WorldSceneExtractor extractor;
    const ve::World& readOnlyWorld = world;
    const ve::SceneRenderList& firstList = extractor.build(readOnlyWorld);
    expectTrue("extractor includes only visible entities with valid bounds and both components", firstList.size() == 3U);
    if (firstList.size() == 3U) {
        expectNearly("extractor sorts first item by entity index", firstList[0].material.flags.x, 10.0f);
        expectNearly("extractor sorts second item by entity index", firstList[1].material.flags.x, 20.0f);
        expectNearly("extractor sorts third item by entity index", firstList[2].material.flags.x, 30.0f);
        expectVec3Nearly("extractor transforms local bounds center", firstList[0].boundsCenter, {11.0f, -1.0f, 4.0f});
        expectNearly("extractor uses conservative non-uniform scale radius", firstList[0].boundsRadius, std::sqrt(29.0f));
        expectNearly("extractor uses conservative shear radius", firstList[1].boundsRadius, 2.0f * std::sqrt(7.0f));
        expectVec3Nearly("extractor applies shear translation", firstList[1].boundsCenter, {-5.0f, 0.0f, 0.0f});
    }

    expectTrue("destroying an extracted entity removes it", world.destroyEntity(second));
    const ve::World::Entity recycled = world.createEntity();
    expectTrue("destroyed extracted slot is recycled", recycled.index == second.index && recycled.generation != second.generation);
    world.emplace<ve::WorldSceneTransform>(recycled, ve::WorldSceneTransform{});
    world.emplace<ve::WorldSceneRenderable>(recycled, makeRenderable(40, {{}, 1.0f, true}));

    const std::size_t previousCapacity = firstList.capacity();
    const ve::SceneRenderList& secondList = extractor.build(world);
    expectTrue("extractor reuses render list capacity", secondList.capacity() >= previousCapacity);
    expectTrue("extractor clears stale entities and keeps recycled entities", secondList.size() == 3U);
    if (secondList.size() == 3U) {
        expectNearly("extractor keeps first entity first after pool swap-remove", secondList[0].material.flags.x, 10.0f);
        expectNearly("extractor sorts recycled entity by index", secondList[1].material.flags.x, 40.0f);
        expectNearly("extractor keeps third entity last after pool swap-remove", secondList[2].material.flags.x, 30.0f);
    }

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "World scene CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
