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
void appendU16(std::vector<std::byte> &bytes, const std::uint16_t value) {
  for (std::uint32_t shift = 0; shift < 16U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void appendU64(std::vector<std::byte> &bytes, const std::uint64_t value) {
  for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
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

std::vector<std::byte> header(const std::uint32_t count,
                              const std::uint32_t version = 1U) {
  std::vector<std::byte> bytes;
  bytes.reserve(12U);
  bytes.insert(bytes.end(),
               {static_cast<std::byte>('V'), static_cast<std::byte>('E'),
                static_cast<std::byte>('S'), static_cast<std::byte>('N')});
  appendU32(bytes, version);
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
  appendU32(bytes, ve::builtin_assets::kCube.index);
  appendU32(bytes, ve::builtin_assets::kCube.generation);
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
void appendV2Record(std::vector<std::byte> &bytes, const ve::SceneEntityId id,
                    const std::string_view name, const std::uint8_t mask,
                    const ve::SceneEntityId parent = {}) {
  appendU64(bytes, id.high);
  appendU64(bytes, id.low);
  appendU16(bytes, static_cast<std::uint16_t>(name.size()));
  for (const char value : name) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  bytes.push_back(static_cast<std::byte>(mask));
  if ((mask & 1U) != 0U) {
    for (int index = 0; index < 10; ++index) {
      appendF32(bytes, index == 6 ? 1.0f : 0.0f);
    }
  }
  if ((mask & 2U) != 0U) {
    appendU64(bytes, parent.high);
    appendU64(bytes, parent.low);
  }
  if ((mask & 4U) != 0U) {
    const auto record = renderableRecord();
    bytes.insert(bytes.end(), record.begin() + 1, record.end());
  }
}

void setIdentity(ve::World &world, const ve::World::Entity entity,
                 const std::uint64_t low, const std::string_view name = {}) {
  ve::setWorldSceneIdentity(world, entity, {0xabcdefff00000000ULL, low},
                            name);
}

ve::WorldSceneRenderable renderable(const float marker) {
  ve::WorldSceneRenderable value{};
  value.mesh = ve::builtin_assets::kSphere;
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
  setIdentity(world, root, 3U, "root");
  setIdentity(world, child, 1U, "child");
  setIdentity(world, leaf, 2U, "leaf");
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
    setIdentity(source, root, 101U, "Root \xE2\x98\x83");
    setIdentity(source, identityChild, 102U, "duplicate");
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
    const ve::SceneEntityId rootId{0xabcdefff00000000ULL, 101U};
    const ve::SceneEntityId childId{0xabcdefff00000000ULL, 102U};
    expectTrue("roundtrip lookup finds stable root identity",
               ve::findWorldSceneEntity(loaded, rootId) == loadedRoot);
    expectTrue("roundtrip lookup finds stable child identity",
               ve::findWorldSceneEntity(loaded, childId) == loadedChild);
    const auto *loadedIdentity =
        loaded.tryGet<ve::WorldSceneIdentity>(loadedRoot);
    expectTrue("roundtrip retains UTF-8 identity name",
               loadedIdentity != nullptr &&
                   loadedIdentity->name == "Root \xE2\x98\x83");
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
    expectTrue("v3 header records the runtime-handle schema",
               firstBytes.size() > 12U &&
                   std::to_integer<std::uint8_t>(firstBytes[4]) == 3U);
  }

  {
    ve::World identities;
    const auto first = identities.createEntity();
    const auto second = identities.createEntity();
    setIdentity(identities, first, 201U, "same");
    setIdentity(identities, second, 202U, "same");
    const auto bytes = ve::encodeWorldScene(identities);
    ve::World loaded;
    ve::decodeWorldScene(loaded, bytes);
    const auto firstLoaded = ve::findWorldSceneEntity(
        loaded, {0xabcdefff00000000ULL, 201U});
    const auto secondLoaded = ve::findWorldSceneEntity(
        loaded, {0xabcdefff00000000ULL, 202U});
    const auto *firstIdentity =
        loaded.tryGet<ve::WorldSceneIdentity>(firstLoaded);
    const auto *secondIdentity =
        loaded.tryGet<ve::WorldSceneIdentity>(secondLoaded);
    expectTrue("duplicate labels are persisted without identity collision",
               firstIdentity != nullptr && secondIdentity != nullptr &&
                   firstIdentity->name == "same" &&
                   secondIdentity->name == "same");
    const auto recycled = loaded.createEntity();
    setIdentity(loaded, recycled, 203U, "recycled");
    expectTrue("destroyed identity is absent",
               loaded.destroyEntity(recycled) &&
                   !ve::findWorldSceneEntity(
                        loaded, {0xabcdefff00000000ULL, 203U}).valid());
    const auto replacement = loaded.createEntity();
    setIdentity(loaded, replacement, 204U, "replacement");
    expectTrue("lookup ignores recycled runtime slots",
               replacement.index == recycled.index &&
                   replacement.generation != recycled.generation &&
                   ve::findWorldSceneEntity(
                       loaded, {0xabcdefff00000000ULL, 204U}) == replacement);
    const auto duplicate = loaded.createEntity();
    loaded.emplace<ve::WorldSceneIdentity>(
        duplicate, ve::WorldSceneIdentity{{0xabcdefff00000000ULL, 201U}, "bad"});
    expectThrows("lookup detects duplicate identity corruption", [&] {
      (void)ve::findWorldSceneEntity(loaded,
                                     {0xabcdefff00000000ULL, 201U});
    });
  }

  ve::World destination;
  const auto sentinel = destination.createEntity();
  destination.emplace<ve::WorldSceneTransform>(
      sentinel, ve::WorldSceneTransform{{{123.0f, 0.0f, 0.0f}}, 77U});
  destination.emplace<ve::WorldSceneRenderable>(sentinel, renderable(456.0f));
  const DestinationSnapshot before = snapshot(destination, sentinel);

  {
    ve::World invalidDestination;
    auto bytes = header(1U, 3U);
    appendV2Record(bytes, {1U, 1U}, "", 4U);
    // A renderable with valid=false may carry a negative radius.
    const std::size_t radiusOffset =
        12U + 16U + 2U + 1U + 2U * sizeof(std::uint32_t) +
        12U * sizeof(float) + 3U * sizeof(float);
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
  {
    const ve::SceneEntityId root{1U, 1U};
    const ve::SceneEntityId child{1U, 2U};
    auto bytes = header(2U, 3U);
    appendV2Record(bytes, root, "", 0U);
    appendV2Record(bytes, child, "child", 2U, root);
    ve::World loaded;
    ve::decodeWorldScene(loaded, bytes);
    const auto rootEntity = ve::findWorldSceneEntity(loaded, root);
    const auto childEntity = ve::findWorldSceneEntity(loaded, child);
    const auto *parent = loaded.tryGet<ve::WorldSceneParent>(childEntity);
    expectTrue("referenced identity-only v2 parent record is constructed",
               rootEntity.valid() && parent != nullptr &&
                   parent->parent == rootEntity);
  }
  {
    auto bytes = header(1U, 3U);
    appendV2Record(bytes, {2U, 1U}, "", 4U);
    constexpr std::size_t kV3RenderableRadiusOffset =
        12U + 16U + 2U + 1U + 2U * sizeof(std::uint32_t) +
        12U * sizeof(float) + 3U * sizeof(float);
    setU32(bytes, kV3RenderableRadiusOffset,
           std::bit_cast<std::uint32_t>(-1.0f));
    bytes[kV3RenderableRadiusOffset + 4U] = std::byte{0U};
    ve::World loaded;
    ve::decodeWorldScene(loaded, bytes);
    ve::World::Entity entity{};
    loaded.each<ve::WorldSceneRenderable>(
        [&](const ve::World::Entity candidate,
            const ve::WorldSceneRenderable &value) {
          entity = candidate;
          expectTrue("v2 invalid bounds keeps negative radius when invalid",
                     !value.localBounds.valid &&
                         value.localBounds.radius == -1.0f);
        });
    expectTrue("v2 renderable offset fixture decodes", entity.valid());
  }

  {
    auto zero = header(1U, 3U);
    appendV2Record(zero, {}, "", 1U);
    expectRejectedTransactionally("v2 zero identity rejects transactionally",
                                  destination, before, zero);
    auto duplicate = header(2U, 3U);
    appendV2Record(duplicate, {4U, 4U}, "", 1U);
    appendV2Record(duplicate, {4U, 4U}, "", 1U);
    expectRejectedTransactionally(
        "v2 duplicate identities reject transactionally", destination, before,
        duplicate);
    auto invalidUtf8 = header(1U, 3U);
    const std::string invalid{static_cast<char>(0x80U)};
    appendV2Record(invalidUtf8, {5U, 1U}, invalid, 1U);
    expectRejectedTransactionally("v2 invalid UTF-8 rejects transactionally",
                                  destination, before, invalidUtf8);
    auto nul = header(1U, 3U);
    const std::string nulName{"a\0b", 3U};
    appendV2Record(nul, {5U, 2U}, nulName, 1U);
    expectRejectedTransactionally("v2 NUL name rejects transactionally",
                                  destination, before, nul);
    auto longName = header(1U, 3U);
    appendV2Record(longName, {5U, 3U}, "abc", 1U);
    expectRejectedTransactionally("v2 name limit rejects transactionally",
                                  destination, before, longName,
                                  {10U, 1024U, 2U, 10U});
    auto totalNames = header(2U, 3U);
    appendV2Record(totalNames, {5U, 4U}, "ab", 1U);
    appendV2Record(totalNames, {5U, 5U}, "cd", 1U);
    expectRejectedTransactionally(
        "v2 aggregate name limit rejects transactionally", destination, before,
        totalNames, {10U, 1024U, 2U, 3U});
    auto missingParent = header(1U, 3U);
    appendV2Record(missingParent, {6U, 1U}, "", 2U, {});
    expectRejectedTransactionally(
        "v2 missing parent identity rejects transactionally", destination,
        before, missingParent);
    auto unknownParent = header(1U, 3U);
    appendV2Record(unknownParent, {6U, 2U}, "", 2U, {6U, 99U});
    expectRejectedTransactionally(
        "v2 unknown parent identity rejects transactionally", destination,
        before, unknownParent);
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
    setIdentity(dangling, child, 301U);
    expectTrue("dangling fixture destroys parent",
               dangling.destroyEntity(dead));
    dangling.emplace<ve::WorldSceneParent>(child, ve::WorldSceneParent{dead});
    expectThrows("dangling source parent rejects encoding",
                 [&] { (void)ve::encodeWorldScene(dangling); });

    ve::World cycle;
    const auto first = cycle.createEntity();
    const auto second = cycle.createEntity();
    setIdentity(cycle, first, 302U);
    setIdentity(cycle, second, 303U);
    cycle.emplace<ve::WorldSceneParent>(first, ve::WorldSceneParent{second});
    cycle.emplace<ve::WorldSceneParent>(second, ve::WorldSceneParent{first});
    expectThrows("cyclic source parent graph rejects encoding",
                 [&] { (void)ve::encodeWorldScene(cycle); });
    ve::World limited;
    const auto limitedFirst = limited.createEntity();
    const auto limitedSecond = limited.createEntity();
    setIdentity(limited, limitedFirst, 304U);
    setIdentity(limited, limitedSecond, 305U);
    limited.emplace<ve::WorldSceneTransform>(limitedFirst);
    limited.emplace<ve::WorldSceneTransform>(limitedSecond);
    expectThrows("source max entity limit rejects encoding",
                 [&] { (void)ve::encodeWorldScene(limited, {1U, 1024U}); });
    expectThrows("source max byte limit rejects encoding",
                 [&] { (void)ve::encodeWorldScene(limited, {10U, 12U}); });
    ve::World missingIdentity;
    const auto unnamed = missingIdentity.createEntity();
    missingIdentity.emplace<ve::WorldSceneTransform>(unnamed);
    expectThrows("source missing identity rejects encoding",
                 [&] { (void)ve::encodeWorldScene(missingIdentity); });
    ve::World duplicateIdentity;
    const auto duplicateFirst = duplicateIdentity.createEntity();
    const auto duplicateSecond = duplicateIdentity.createEntity();
    duplicateIdentity.emplace<ve::WorldSceneIdentity>(
        duplicateFirst, ve::WorldSceneIdentity{{7U, 7U}, "first"});
    duplicateIdentity.emplace<ve::WorldSceneIdentity>(
        duplicateSecond, ve::WorldSceneIdentity{{7U, 7U}, "second"});
    expectThrows("source duplicate identity rejects encoding",
                 [&] { (void)ve::encodeWorldScene(duplicateIdentity); });
  }

  {
    ve::World fileWorld;
    const auto entity = fileWorld.createEntity();
    setIdentity(fileWorld, entity, 306U, "file");
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
