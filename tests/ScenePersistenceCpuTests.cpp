#include "scene/ScenePersistence.hpp"

#include "renderer/SceneRenderer.hpp"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

int gFailureCount = 0;

void expectTrue(const std::string_view context, const bool value) {
  if (!value) {
    std::cerr << "[FAILED] " << context << '\n';
    ++gFailureCount;
  }
}

void expectEqual(const std::string_view context, const float actual,
                 const float expected) {
  expectTrue(context, std::isfinite(actual) && std::isfinite(expected) &&
                          actual == expected);
}

void appendU32(std::vector<std::byte> &bytes, const std::uint32_t value) {
  for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void appendF32(std::vector<std::byte> &bytes, const float value) {
  appendU32(bytes, std::bit_cast<std::uint32_t>(value));
}

void setU32(std::vector<std::byte> &bytes, const std::size_t offset,
            const std::uint32_t value) {
  if (offset + 4U > bytes.size()) {
    return;
  }
  for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
    bytes[offset + shift / 8U] =
        static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

std::vector<std::byte> header(const std::uint32_t count) {
  std::vector<std::byte> bytes;
  bytes.reserve(12U);
  bytes.insert(bytes.end(),
               {static_cast<std::byte>('V'), static_cast<std::byte>('E'),
                static_cast<std::byte>('S'), static_cast<std::byte>('N')});
  appendU32(bytes, 1U);
  appendU32(bytes, count);
  return bytes;
}

std::vector<std::byte> transformRecord(const std::uint8_t mask = 1U) {
  std::vector<std::byte> bytes;
  bytes.push_back(static_cast<std::byte>(mask));
  for (int index = 0; index < 10; ++index) {
    appendF32(bytes, index == 6 ? 1.0f : 0.0f);
  }
  return bytes;
}

std::vector<std::byte> renderableRecord() {
  std::vector<std::byte> bytes;
  bytes.push_back(std::byte{4U});
  bytes.push_back(std::byte{0U});
  for (int index = 0; index < 12; ++index) {
    appendF32(bytes, 0.0f);
  }
  for (int index = 0; index < 3; ++index) {
    appendF32(bytes, 0.0f);
  }
  appendF32(bytes, 1.0f);
  bytes.push_back(std::byte{1U});
  bytes.push_back(std::byte{1U});
  return bytes;
}

ve::WorldSceneRenderable renderable(const float marker) {
  ve::WorldSceneRenderable value{};
  value.mesh = ve::SceneMeshId::Sphere;
  value.material.albedoRoughness = {marker, 0.25f, 0.5f, 0.75f};
  value.material.emissiveMetallic = {0.1f, 0.2f, 0.3f, 0.4f};
  value.material.flags = {marker + 1.0f, 0.0f, 0.0f, 0.0f};
  value.localBounds = {{1.0f, 2.0f, 3.0f}, 4.0f, true};
  value.visible = false;
  return value;
}

struct DestinationSnapshot {
  std::uint64_t token = 0;
  std::size_t entities = 0;
  std::size_t transforms = 0;
  std::size_t parents = 0;
  std::size_t renderables = 0;
  ve::World::Entity sentinel{};
  float translation = 0.0f;
  float materialMarker = 0.0f;
};

DestinationSnapshot snapshot(const ve::World &world,
                             const ve::World::Entity sentinel) {
  DestinationSnapshot result{};
  result.token = world.instanceToken();
  result.entities = world.entityCount();
  result.transforms = world.componentCount<ve::WorldSceneTransform>();
  result.parents = world.componentCount<ve::WorldSceneParent>();
  result.renderables = world.componentCount<ve::WorldSceneRenderable>();
  result.sentinel = sentinel;
  if (const auto *transform = world.tryGet<ve::WorldSceneTransform>(sentinel);
      transform != nullptr) {
    result.translation = transform->current.translation.x;
  }
  if (const auto *value = world.tryGet<ve::WorldSceneRenderable>(sentinel);
      value != nullptr) {
    result.materialMarker = value->material.flags.x;
  }
  return result;
}

void expectSnapshot(const std::string_view context, const ve::World &world,
                    const DestinationSnapshot &expected) {
  expectTrue(std::string(context) + " preserves instance token",
             world.instanceToken() == expected.token);
  expectTrue(std::string(context) + " preserves entity count",
             world.entityCount() == expected.entities);
  expectTrue(std::string(context) + " preserves transform count",
             world.componentCount<ve::WorldSceneTransform>() ==
                 expected.transforms);
  expectTrue(std::string(context) + " preserves parent count",
             world.componentCount<ve::WorldSceneParent>() == expected.parents);
  expectTrue(std::string(context) + " preserves renderable count",
             world.componentCount<ve::WorldSceneRenderable>() ==
                 expected.renderables);
  expectTrue(std::string(context) + " preserves sentinel liveness",
             world.alive(expected.sentinel));
  const auto *transform =
      world.tryGet<ve::WorldSceneTransform>(expected.sentinel);
  const auto *value = world.tryGet<ve::WorldSceneRenderable>(expected.sentinel);
  expectTrue(std::string(context) + " preserves sentinel components",
             transform != nullptr && value != nullptr);
  if (transform != nullptr) {
    expectEqual(std::string(context) + " preserves sentinel transform",
                transform->current.translation.x, expected.translation);
  }
  if (value != nullptr) {
    expectEqual(std::string(context) + " preserves sentinel material",
                value->material.flags.x, expected.materialMarker);
  }
}

template <typename Function>
void expectThrows(const std::string_view context, Function &&function) {
  bool threw = false;
  try {
    function();
  } catch (const std::exception &) {
    threw = true;
  }
  expectTrue(context, threw);
}

void expectRejectedTransactionally(
    const std::string_view context, ve::World &destination,
    const DestinationSnapshot &before, const std::vector<std::byte> &bytes,
    const ve::ScenePersistenceLimits &limits = {}) {
  expectThrows(context,
               [&] { ve::decodeWorldScene(destination, bytes, limits); });
  expectSnapshot(context, destination, before);
}

void populateCanonical(ve::World &world, const bool churnPools) {
  if (churnPools) {
    const auto ignored = world.createEntity();
    const auto stale = world.createEntity();
    world.emplace<ve::WorldSceneTransform>(stale);
    world.emplace<ve::WorldSceneRenderable>(stale, renderable(-9.0f));
    expectTrue("canonical fixture removes stale entity",
               world.destroyEntity(stale));
    expectTrue("canonical fixture retains unrelated entity",
               world.alive(ignored));
  }
  const auto root = world.createEntity();
  const auto child = world.createEntity();
  const auto leaf = world.createEntity();
  if (churnPools) {
    world.emplace<ve::WorldSceneRenderable>(leaf, renderable(30.0f));
    world.emplace<ve::WorldSceneTransform>(
        child, ve::WorldSceneTransform{{{2.0f, 0.0f, 0.0f}}});
    world.emplace<ve::WorldSceneParent>(child, ve::WorldSceneParent{root});
    world.emplace<ve::WorldSceneTransform>(
        root, ve::WorldSceneTransform{{{1.0f, 0.0f, 0.0f}}});
    world.emplace<ve::WorldSceneRenderable>(root, renderable(10.0f));
    world.emplace<ve::WorldSceneParent>(leaf, ve::WorldSceneParent{child});
  } else {
    world.emplace<ve::WorldSceneTransform>(
        root, ve::WorldSceneTransform{{{1.0f, 0.0f, 0.0f}}});
    world.emplace<ve::WorldSceneRenderable>(root, renderable(10.0f));
    world.emplace<ve::WorldSceneTransform>(
        child, ve::WorldSceneTransform{{{2.0f, 0.0f, 0.0f}}});
    world.emplace<ve::WorldSceneParent>(child, ve::WorldSceneParent{root});
    world.emplace<ve::WorldSceneParent>(leaf, ve::WorldSceneParent{child});
    world.emplace<ve::WorldSceneRenderable>(leaf, renderable(30.0f));
  }
}

} // namespace

int main() {
  {
    ve::World source;
    const auto root = source.createEntity();
    const auto identityChild = source.createEntity();
    source.emplace<ve::WorldSceneTransform>(
        root, ve::WorldSceneTransform{{{7.0f, 8.0f, 9.0f}}, 42U});
    source.emplace<ve::WorldSceneRenderable>(root, renderable(11.0f));
    source.emplace<ve::WorldSceneParent>(identityChild,
                                         ve::WorldSceneParent{root});

    ve::World loaded;
    ve::decodeWorldScene(loaded, ve::encodeWorldScene(source));
    expectTrue(
        "roundtrip retains transform, parent, and renderable entity counts",
        loaded.entityCount() == 2U &&
            loaded.componentCount<ve::WorldSceneTransform>() == 1U &&
            loaded.componentCount<ve::WorldSceneParent>() == 1U &&
            loaded.componentCount<ve::WorldSceneRenderable>() == 1U);
    ve::World::Entity loadedRoot{};
    ve::World::Entity loadedChild{};
    loaded.each<ve::WorldSceneTransform>(
        [&](const ve::World::Entity entity,
            const ve::WorldSceneTransform &transform) {
          loadedRoot = entity;
          expectEqual("roundtrip preserves transform translation",
                      transform.current.translation.x, 7.0f);
          expectTrue("roundtrip resets discontinuity revision",
                     transform.discontinuityRevision == 0U);
        });
    loaded.each<ve::WorldSceneParent>([&](const ve::World::Entity entity,
                                          const ve::WorldSceneParent &parent) {
      loadedChild = entity;
      expectTrue("loaded parent references a loaded live entity",
                 loaded.alive(parent.parent));
      expectTrue("loaded parent remaps to loaded root",
                 parent.parent == loadedRoot);
    });
    const auto *loadedRenderable =
        loaded.tryGet<ve::WorldSceneRenderable>(loadedRoot);
    expectTrue("roundtrip root and identity-only child are identified",
               loadedRoot.valid() && loadedChild.valid());
    if (loadedRenderable != nullptr) {
      expectEqual("roundtrip preserves renderable material",
                  loadedRenderable->material.albedoRoughness.x, 11.0f);
      expectTrue("roundtrip preserves renderable visibility",
                 !loadedRenderable->visible);
    } else {
      expectTrue("roundtrip retains renderable", false);
    }
  }

  {
    ve::World empty;
    const auto bytes = ve::encodeWorldScene(empty);
    expectTrue("empty scene has only format header", bytes.size() == 12U);
    ve::World loaded;
    (void)loaded.createEntity();
    ve::decodeWorldScene(loaded, bytes);
    expectTrue("empty scene replaces destination with empty world",
               loaded.entityCount() == 0U);
  }

  {
    ve::World first;
    ve::World second;
    populateCanonical(first, false);
    populateCanonical(second, true);
    ve::World::Entity firstRoot{};
    ve::World::Entity secondRoot{};
    first.each<ve::WorldSceneTransform>(
        [&](const ve::World::Entity entity,
            const ve::WorldSceneTransform &transform) {
          if (transform.current.translation.x == 1.0f) {
            firstRoot = entity;
          }
        });
    second.each<ve::WorldSceneTransform>(
        [&](const ve::World::Entity entity,
            const ve::WorldSceneTransform &transform) {
          if (transform.current.translation.x == 1.0f) {
            secondRoot = entity;
          }
        });
    expectTrue(
        "canonical fixtures use distinct root runtime indices and generations",
        firstRoot.valid() && secondRoot.valid() &&
            firstRoot.index != secondRoot.index &&
            firstRoot.generation != secondRoot.generation);
    const auto firstBytes = ve::encodeWorldScene(first);
    const auto secondBytes = ve::encodeWorldScene(second);
    expectTrue("encoding ignores dense-pool insertion removal order and "
               "runtime generation differences",
               firstBytes == secondBytes);
  }

  ve::World destination;
  const auto sentinel = destination.createEntity();
  destination.emplace<ve::WorldSceneTransform>(
      sentinel, ve::WorldSceneTransform{{{123.0f, 0.0f, 0.0f}}, 77U});
  destination.emplace<ve::WorldSceneRenderable>(sentinel, renderable(456.0f));
  const DestinationSnapshot before = snapshot(destination, sentinel);

  {
    ve::World invalidDestination;
    auto bytes = header(1U);
    const auto record = renderableRecord();
    bytes.insert(bytes.end(), record.begin(), record.end());
    // A renderable with valid=false may carry a negative radius.
    const std::size_t radiusOffset = 12U + 1U + 1U + 12U * 4U + 3U * 4U;
    setU32(bytes, radiusOffset, std::bit_cast<std::uint32_t>(-1.0f));
    bytes[radiusOffset + 4U] = std::byte{0U};
    ve::decodeWorldScene(invalidDestination, bytes);
    expectTrue("invalid bounds record is accepted",
               invalidDestination.entityCount() == 1U);
    ve::World::Entity entity{};
    invalidDestination.each<ve::WorldSceneRenderable>(
        [&](const ve::World::Entity candidate,
            const ve::WorldSceneRenderable &value) {
          entity = candidate;
          expectTrue("invalid bounds remains invalid",
                     !value.localBounds.valid);
          expectEqual("invalid bounds retains radius", value.localBounds.radius,
                      -1.0f);
        });
    expectTrue("invalid bounds decoded entity exists", entity.valid());
  }

  auto malformed = header(1U);
  const auto validTransform = transformRecord();
  malformed.insert(malformed.end(), validTransform.begin(),
                   validTransform.end());
  {
    auto bytes = malformed;
    bytes[0] = static_cast<std::byte>('X');
    expectRejectedTransactionally("malformed magic rejects transactionally",
                                  destination, before, bytes);
  }
  {
    auto bytes = malformed;
    setU32(bytes, 4U, 2U);
    expectRejectedTransactionally("unsupported version rejects transactionally",
                                  destination, before, bytes);
  }
  {
    auto bytes = malformed;
    bytes.pop_back();
    expectRejectedTransactionally("truncated record rejects transactionally",
                                  destination, before, bytes);
  }
  {
    auto bytes = malformed;
    bytes.push_back(std::byte{0U});
    expectRejectedTransactionally("trailing data rejects transactionally",
                                  destination, before, bytes);
  }
  {
    auto bytes = header(1U);
    bytes.push_back(std::byte{0x80U});
    expectRejectedTransactionally(
        "unknown component mask rejects transactionally", destination, before,
        bytes);
  }
  {
    auto bytes = header(1U);
    auto record = renderableRecord();
    record.back() = std::byte{2U};
    bytes.insert(bytes.end(), record.begin(), record.end());
    expectRejectedTransactionally(
        "invalid visible boolean rejects transactionally", destination, before,
        bytes);
  }
  {
    auto bytes = header(1U);
    auto record = renderableRecord();
    record[1] = std::byte{255U};
    bytes.insert(bytes.end(), record.begin(), record.end());
    expectRejectedTransactionally("invalid mesh rejects transactionally",
                                  destination, before, bytes);
  }
  {
    auto bytes = malformed;
    setU32(bytes, 13U, 0x7fc00000U);
    expectRejectedTransactionally("nonfinite transform rejects transactionally",
                                  destination, before, bytes);
  }
  {
    auto bytes = header(1U);
    auto record = renderableRecord();
    const std::size_t radius = 1U + 1U + 12U * 4U + 3U * 4U;
    setU32(record, radius, std::bit_cast<std::uint32_t>(-1.0f));
    bytes.insert(bytes.end(), record.begin(), record.end());
    expectRejectedTransactionally(
        "negative valid bounds radius rejects transactionally", destination,
        before, bytes);
  }
  {
    auto bytes = header(1U);
    bytes.push_back(std::byte{2U});
    appendU32(bytes, 1U);
    expectRejectedTransactionally("out of range parent rejects transactionally",
                                  destination, before, bytes);
  }
  {
    auto bytes = header(1U);
    bytes.push_back(std::byte{2U});
    appendU32(bytes, 0U);
    expectRejectedTransactionally("self parent rejects transactionally",
                                  destination, before, bytes);
  }
  {
    auto bytes = header(2U);
    bytes.push_back(std::byte{2U});
    appendU32(bytes, 1U);
    bytes.push_back(std::byte{2U});
    appendU32(bytes, 0U);
    expectRejectedTransactionally("parent cycle rejects transactionally",
                                  destination, before, bytes);
  }
  {
    auto bytes = header(1U);
    bytes.push_back(std::byte{0U});
    expectRejectedTransactionally(
        "unreferenced mask-zero record rejects transactionally", destination,
        before, bytes);
  }
  {
    auto bytes = malformed;
    expectRejectedTransactionally("max entity limit rejects transactionally",
                                  destination, before, bytes, {0U, 1024U});
    expectRejectedTransactionally("max byte limit rejects transactionally",
                                  destination, before, bytes, {10U, 12U});
  }
  {
    const auto bytes = header(1'000'000U);
    expectRejectedTransactionally(
        "impossible record count rejects before record allocation", destination,
        before, bytes);
  }

  {
    ve::World dangling;
    const auto child = dangling.createEntity();
    const auto dead = dangling.createEntity();
    expectTrue("dangling fixture destroys parent",
               dangling.destroyEntity(dead));
    dangling.emplace<ve::WorldSceneParent>(child, ve::WorldSceneParent{dead});
    expectThrows("dangling source parent rejects encoding",
                 [&] { (void)ve::encodeWorldScene(dangling); });

    ve::World cycle;
    const auto first = cycle.createEntity();
    const auto second = cycle.createEntity();
    cycle.emplace<ve::WorldSceneParent>(first, ve::WorldSceneParent{second});
    cycle.emplace<ve::WorldSceneParent>(second, ve::WorldSceneParent{first});
    expectThrows("cyclic source parent graph rejects encoding",
                 [&] { (void)ve::encodeWorldScene(cycle); });
    ve::World limited;
    const auto limitedFirst = limited.createEntity();
    const auto limitedSecond = limited.createEntity();
    limited.emplace<ve::WorldSceneTransform>(limitedFirst);
    limited.emplace<ve::WorldSceneTransform>(limitedSecond);
    expectThrows("source max entity limit rejects encoding",
                 [&] { (void)ve::encodeWorldScene(limited, {1U, 1024U}); });
    expectThrows("source max byte limit rejects encoding",
                 [&] { (void)ve::encodeWorldScene(limited, {10U, 12U}); });
  }

  {
    ve::World fileWorld;
    const auto entity = fileWorld.createEntity();
    fileWorld.emplace<ve::WorldSceneTransform>(
        entity, ve::WorldSceneTransform{{{3.0f, 0.0f, 0.0f}}});
    std::filesystem::path directory;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (std::uint32_t suffix = 0U;; ++suffix) {
      directory = std::filesystem::temp_directory_path() /
                  ("volk-engine-scene-persistence-" + std::to_string(nonce) +
                   "-" + std::to_string(suffix));
      if (std::filesystem::create_directory(directory)) {
        break;
      }
    }
    const std::filesystem::path path = directory / "scene.vescene";
    ve::saveWorldScene(fileWorld, path);
    expectTrue("save creates scene file",
               std::filesystem::is_regular_file(path));
    fileWorld.tryGet<ve::WorldSceneTransform>(entity)->current.translation.x =
        99.0f;
    ve::saveWorldScene(fileWorld, path);
    ve::World loaded;
    ve::loadWorldScene(loaded, path);
    ve::World::Entity loadedEntity{};
    loaded.each<ve::WorldSceneTransform>(
        [&](const ve::World::Entity candidate,
            const ve::WorldSceneTransform &transform) {
          loadedEntity = candidate;
          expectEqual("overwrite load returns latest scene",
                      transform.current.translation.x, 99.0f);
        });
    expectTrue("file load creates entity", loadedEntity.valid());
    expectThrows("file load enforces byte limit before decode",
                 [&] { ve::loadWorldScene(destination, path, {10U, 12U}); });
    expectSnapshot("bounded file load", destination, before);
    bool temporarySibling = false;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
      const std::string name = entry.path().filename().string();
      temporarySibling = temporarySibling ||
                         name.starts_with(path.filename().string() + ".tmp.");
    }
    expectTrue("successful atomic save leaves no temporary sibling",
               !temporarySibling);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  return gFailureCount == 0 ? 0 : 1;
}
