#include "scene/ScenePersistence.hpp"

#include "core/FileSystem.hpp"
#include "renderer/SceneRenderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace ve {
namespace {

constexpr std::uint32_t kVersion = 1U;
constexpr std::uint8_t kTransformMask = 1U << 0U;
constexpr std::uint8_t kParentMask = 1U << 1U;
constexpr std::uint8_t kRenderableMask = 1U << 2U;
constexpr std::uint8_t kKnownMask =
    kTransformMask | kParentMask | kRenderableMask;
constexpr std::size_t kHeaderSize = 12U;

[[nodiscard]] std::uint64_t entityKey(const World::Entity entity) noexcept {
  return (static_cast<std::uint64_t>(entity.index) << 32U) | entity.generation;
}

[[nodiscard]] bool entityLess(const World::Entity left,
                              const World::Entity right) noexcept {
  return left.index != right.index ? left.index < right.index
                                   : left.generation < right.generation;
}

[[nodiscard]] bool validMesh(const SceneMeshId mesh) noexcept {
  return static_cast<std::uint8_t>(mesh) <=
         static_cast<std::uint8_t>(SceneMeshId::ImportedModel);
}

[[nodiscard]] bool finite(const Vec4 value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] bool
validRenderable(const WorldSceneRenderable &renderable) noexcept {
  return validMesh(renderable.mesh) &&
         finite(renderable.material.albedoRoughness) &&
         finite(renderable.material.emissiveMetallic) &&
         finite(renderable.material.flags) &&
         finite(renderable.localBounds.center) &&
         std::isfinite(renderable.localBounds.radius) &&
         (!renderable.localBounds.valid ||
          renderable.localBounds.radius >= 0.0F);
}

void validateLimits(const ScenePersistenceLimits &limits) {
  if (limits.maxEntities >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::invalid_argument(
        "Scene persistence entity limit exceeds format range");
  }
  if (limits.maxBytes < kHeaderSize) {
    throw std::invalid_argument(
        "Scene persistence byte limit cannot contain a header");
  }
}

[[noreturn]] void formatError(const char *message) {
  throw std::runtime_error(message);
}

class Writer final {
public:
  explicit Writer(const std::size_t maximumBytes)
      : maximumBytes_(maximumBytes) {}

  void reserve(const std::size_t bytes) {
    bytes_.reserve(std::min(bytes, maximumBytes_));
  }

  void u8(const std::uint8_t value) {
    ensure(1U);
    bytes_.push_back(static_cast<std::byte>(value));
  }

  void u32(const std::uint32_t value) {
    ensure(4U);
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
      bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
  }

  void f32(const float value) { u32(std::bit_cast<std::uint32_t>(value)); }

  [[nodiscard]] std::vector<std::byte> take() && { return std::move(bytes_); }

private:
  void ensure(const std::size_t count) const {
    if (count > maximumBytes_ - bytes_.size()) {
      formatError("Scene persistence output exceeds byte limit");
    }
  }

  std::size_t maximumBytes_;
  std::vector<std::byte> bytes_;
};

class Reader final {
public:
  explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] std::uint8_t u8() {
    require(1U);
    return std::to_integer<std::uint8_t>(bytes_[position_++]);
  }

  [[nodiscard]] std::uint32_t u32() {
    require(4U);
    std::uint32_t value = 0U;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
      value |= static_cast<std::uint32_t>(
                   std::to_integer<std::uint8_t>(bytes_[position_++]))
               << shift;
    }
    return value;
  }

  [[nodiscard]] float f32() { return std::bit_cast<float>(u32()); }
  [[nodiscard]] bool exhausted() const noexcept {
    return position_ == bytes_.size();
  }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - position_;
  }

private:
  void require(const std::size_t count) const {
    if (count > bytes_.size() - position_) {
      formatError("Truncated scene persistence data");
    }
  }

  std::span<const std::byte> bytes_;
  std::size_t position_ = 0U;
};

struct Record {
  std::uint8_t mask = 0U;
  TransformTRS transform{};
  std::uint32_t parent = 0U;
  WorldSceneRenderable renderable{};
};

void writeVec3(Writer &writer, const Vec3 value) {
  writer.f32(value.x);
  writer.f32(value.y);
  writer.f32(value.z);
}

void writeVec4(Writer &writer, const Vec4 value) {
  writer.f32(value.x);
  writer.f32(value.y);
  writer.f32(value.z);
  writer.f32(value.w);
}

[[nodiscard]] Vec3 readVec3(Reader &reader) {
  return {reader.f32(), reader.f32(), reader.f32()};
}
[[nodiscard]] Vec4 readVec4(Reader &reader) {
  return {reader.f32(), reader.f32(), reader.f32(), reader.f32()};
}

void validateGraph(const std::vector<Record> &records) {
  const std::size_t count = records.size();
  std::vector<std::uint8_t> state(count, 0U);
  std::vector<std::uint32_t> path;
  path.reserve(count);
  std::vector<std::uint32_t> incoming(count, 0U);

  for (std::size_t index = 0U; index < count; ++index) {
    const Record &record = records[index];
    if ((record.mask & kParentMask) != 0U) {
      if (record.parent >= count || record.parent == index) {
        formatError("Invalid scene parent reference");
      }
      ++incoming[record.parent];
    }
  }
  for (std::size_t index = 0U; index < count; ++index) {
    if (records[index].mask == 0U && incoming[index] == 0U) {
      formatError("Unreferenced identity-only scene record");
    }
  }

  for (std::size_t start = 0U; start < count; ++start) {
    if (state[start] != 0U) {
      continue;
    }
    path.clear();
    std::size_t current = start;
    while (true) {
      if (state[current] == 1U) {
        formatError("Cyclic scene parent graph");
      }
      if (state[current] == 2U) {
        break;
      }
      state[current] = 1U;
      path.push_back(static_cast<std::uint32_t>(current));
      if ((records[current].mask & kParentMask) == 0U) {
        break;
      }
      current = records[current].parent;
    }
    for (const std::uint32_t index : path) {
      state[index] = 2U;
    }
  }
}

[[nodiscard]] std::size_t encodedRecordSize(const Record &record) {
  std::size_t size = 1U;
  if ((record.mask & kTransformMask) != 0U) {
    size += 10U * sizeof(float);
  }
  if ((record.mask & kParentMask) != 0U) {
    size += sizeof(std::uint32_t);
  }
  if ((record.mask & kRenderableMask) != 0U) {
    size +=
        1U + 12U * sizeof(float) + 3U * sizeof(float) + sizeof(float) + 1U + 1U;
  }
  return size;
}

void writeRecord(Writer &writer, const Record &record) {
  writer.u8(record.mask);
  if ((record.mask & kTransformMask) != 0U) {
    writeVec3(writer, record.transform.translation);
    writer.f32(record.transform.rotation.x);
    writer.f32(record.transform.rotation.y);
    writer.f32(record.transform.rotation.z);
    writer.f32(record.transform.rotation.w);
    writeVec3(writer, record.transform.scale);
  }
  if ((record.mask & kParentMask) != 0U) {
    writer.u32(record.parent);
  }
  if ((record.mask & kRenderableMask) != 0U) {
    writer.u8(static_cast<std::uint8_t>(record.renderable.mesh));
    writeVec4(writer, record.renderable.material.albedoRoughness);
    writeVec4(writer, record.renderable.material.emissiveMetallic);
    writeVec4(writer, record.renderable.material.flags);
    writeVec3(writer, record.renderable.localBounds.center);
    writer.f32(record.renderable.localBounds.radius);
    writer.u8(record.renderable.localBounds.valid ? 1U : 0U);
    writer.u8(record.renderable.visible ? 1U : 0U);
  }
}

[[nodiscard]] Record readRecord(Reader &reader) {
  Record record{};
  record.mask = reader.u8();
  if ((record.mask & ~kKnownMask) != 0U) {
    formatError("Unknown scene component mask");
  }
  if ((record.mask & kTransformMask) != 0U) {
    record.transform.translation = readVec3(reader);
    record.transform.rotation = {reader.f32(), reader.f32(), reader.f32(),
                                 reader.f32()};
    record.transform.scale = readVec3(reader);
    if (!finite(record.transform)) {
      formatError("Non-finite scene transform");
    }
  }
  if ((record.mask & kParentMask) != 0U) {
    record.parent = reader.u32();
  }
  if ((record.mask & kRenderableMask) != 0U) {
    const std::uint8_t mesh = reader.u8();
    if (mesh > static_cast<std::uint8_t>(SceneMeshId::ImportedModel)) {
      formatError("Invalid scene mesh identifier");
    }
    record.renderable.mesh = static_cast<SceneMeshId>(mesh);
    record.renderable.material.albedoRoughness = readVec4(reader);
    record.renderable.material.emissiveMetallic = readVec4(reader);
    record.renderable.material.flags = readVec4(reader);
    record.renderable.localBounds.center = readVec3(reader);
    record.renderable.localBounds.radius = reader.f32();
    const std::uint8_t boundsValid = reader.u8();
    const std::uint8_t visible = reader.u8();
    if (boundsValid > 1U || visible > 1U) {
      formatError("Invalid scene boolean");
    }
    record.renderable.localBounds.valid = boundsValid != 0U;
    record.renderable.visible = visible != 0U;
    if (!validRenderable(record.renderable)) {
      formatError("Invalid scene renderable");
    }
  }
  return record;
}

} // namespace

std::vector<std::byte> encodeWorldScene(const World &world,
                                        const ScenePersistenceLimits &limits) {
  validateLimits(limits);

  std::size_t gatherCapacity = 0U;
  const auto addGatherCapacity = [&](const std::size_t count) {
    gatherCapacity = count > limits.maxEntities - gatherCapacity
                         ? limits.maxEntities
                         : gatherCapacity + count;
  };
  addGatherCapacity(world.componentCount<WorldSceneTransform>());
  addGatherCapacity(world.componentCount<WorldSceneParent>());
  addGatherCapacity(world.componentCount<WorldSceneRenderable>());
  std::vector<World::Entity> entities;
  entities.reserve(gatherCapacity);
  std::unordered_map<std::uint64_t, bool> included;
  included.reserve(gatherCapacity);
  const auto include = [&](const World::Entity entity) {
    if (included.emplace(entityKey(entity), true).second) {
      if (entities.size() == limits.maxEntities) {
        formatError("Scene entity count exceeds limit");
      }
      entities.push_back(entity);
    }
  };
  world.each<WorldSceneTransform>(
      [&](const World::Entity entity, const WorldSceneTransform &) {
        include(entity);
      });
  world.each<WorldSceneParent>(
      [&](const World::Entity entity, const WorldSceneParent &) {
        include(entity);
      });
  world.each<WorldSceneRenderable>(
      [&](const World::Entity entity, const WorldSceneRenderable &) {
        include(entity);
      });

  for (std::size_t index = 0U; index < entities.size(); ++index) {
    const WorldSceneParent *parent =
        world.tryGet<WorldSceneParent>(entities[index]);
    if (parent != nullptr) {
      if (!world.alive(parent->parent)) {
        formatError("Dangling scene parent");
      }
      include(parent->parent);
    }
  }
  std::sort(entities.begin(), entities.end(), entityLess);

  std::unordered_map<std::uint64_t, std::uint32_t> sceneIds;
  sceneIds.reserve(entities.size());
  for (std::size_t index = 0U; index < entities.size(); ++index) {
    sceneIds.emplace(entityKey(entities[index]),
                     static_cast<std::uint32_t>(index));
  }

  std::vector<Record> records;
  records.reserve(entities.size());
  std::size_t totalSize = kHeaderSize;
  for (const World::Entity entity : entities) {
    Record record{};
    if (const WorldSceneTransform *transform =
            world.tryGet<WorldSceneTransform>(entity);
        transform != nullptr) {
      if (!finite(transform->current)) {
        formatError("Non-finite scene transform");
      }
      record.mask |= kTransformMask;
      record.transform = transform->current;
    }
    if (const WorldSceneParent *parent = world.tryGet<WorldSceneParent>(entity);
        parent != nullptr) {
      const auto parentId = sceneIds.find(entityKey(parent->parent));
      if (parentId == sceneIds.end()) {
        formatError("Dangling scene parent");
      }
      record.mask |= kParentMask;
      record.parent = parentId->second;
    }
    if (const WorldSceneRenderable *renderable =
            world.tryGet<WorldSceneRenderable>(entity);
        renderable != nullptr) {
      if (!validRenderable(*renderable)) {
        formatError("Invalid scene renderable");
      }
      record.mask |= kRenderableMask;
      record.renderable = *renderable;
    }
    const std::size_t recordSize = encodedRecordSize(record);
    if (recordSize > limits.maxBytes - totalSize) {
      formatError("Scene persistence output exceeds byte limit");
    }
    totalSize += recordSize;
    records.push_back(record);
  }
  validateGraph(records);

  Writer writer(limits.maxBytes);
  writer.reserve(totalSize);
  writer.u8(static_cast<std::uint8_t>('V'));
  writer.u8(static_cast<std::uint8_t>('E'));
  writer.u8(static_cast<std::uint8_t>('S'));
  writer.u8(static_cast<std::uint8_t>('N'));
  writer.u32(kVersion);
  writer.u32(static_cast<std::uint32_t>(records.size()));
  for (const Record &record : records) {
    writeRecord(writer, record);
  }
  return std::move(writer).take();
}

void decodeWorldScene(World &destination,
                      const std::span<const std::byte> bytes,
                      const ScenePersistenceLimits &limits) {
  validateLimits(limits);
  if (bytes.size() > limits.maxBytes) {
    formatError("Scene persistence input exceeds byte limit");
  }
  Reader reader(bytes);
  if (reader.u8() != static_cast<std::uint8_t>('V') ||
      reader.u8() != static_cast<std::uint8_t>('E') ||
      reader.u8() != static_cast<std::uint8_t>('S') ||
      reader.u8() != static_cast<std::uint8_t>('N')) {
    formatError("Invalid scene persistence magic");
  }
  if (reader.u32() != kVersion) {
    formatError("Unsupported scene persistence version");
  }
  const std::uint32_t count = reader.u32();
  if (count > limits.maxEntities) {
    formatError("Scene entity count exceeds limit");
  }
  if (count > reader.remaining()) {
    formatError("Scene entity count exceeds available record data");
  }

  std::vector<Record> records;
  records.reserve(count);
  for (std::uint32_t index = 0U; index < count; ++index) {
    records.push_back(readRecord(reader));
  }
  if (!reader.exhausted()) {
    formatError("Trailing scene persistence data");
  }
  validateGraph(records);

  World temporary;
  temporary.reserveEntities(count);
  std::size_t transformCount = 0U;
  std::size_t parentCount = 0U;
  std::size_t renderableCount = 0U;
  for (const Record &record : records) {
    transformCount += (record.mask & kTransformMask) != 0U;
    parentCount += (record.mask & kParentMask) != 0U;
    renderableCount += (record.mask & kRenderableMask) != 0U;
  }
  temporary.reserveComponents<WorldSceneTransform>(transformCount);
  temporary.reserveComponents<WorldSceneParent>(parentCount);
  temporary.reserveComponents<WorldSceneRenderable>(renderableCount);

  std::vector<World::Entity> entities;
  entities.reserve(count);
  for (const Record &record : records) {
    const World::Entity entity = temporary.createEntity();
    entities.push_back(entity);
    if ((record.mask & kTransformMask) != 0U) {
      temporary.emplace<WorldSceneTransform>(
          entity, WorldSceneTransform{record.transform, 0U});
    }
    if ((record.mask & kRenderableMask) != 0U) {
      temporary.emplace<WorldSceneRenderable>(entity, record.renderable);
    }
  }
  for (std::size_t index = 0U; index < records.size(); ++index) {
    if ((records[index].mask & kParentMask) != 0U) {
      temporary.emplace<WorldSceneParent>(
          entities[index], WorldSceneParent{entities[records[index].parent]});
    }
  }
  destination = std::move(temporary);
}

void saveWorldScene(const World &world, const std::filesystem::path &path,
                    const ScenePersistenceLimits &limits) {
  const std::vector<std::byte> bytes = encodeWorldScene(world, limits);
  writeBinaryFileAtomic(path, bytes);
}

void loadWorldScene(World &destination, const std::filesystem::path &path,
                    const ScenePersistenceLimits &limits) {
  decodeWorldScene(destination, readBinaryFile(path, limits.maxBytes), limits);
}

} // namespace ve
