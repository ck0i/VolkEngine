#include "scene/CookedWorld.hpp"

#include "core/BinaryIO.hpp"
#include "core/FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ve {
namespace {

constexpr std::uint32_t kCookedMagic = 0x57434556U;
constexpr std::uint32_t kCookedVersion = 1U;
constexpr std::uint32_t kNoParent = std::numeric_limits<std::uint32_t>::max();

class Writer {
public:
  template <typename T>
    requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
  void pod(const T value) {
    appendLittleEndian(output, value);
  }

  void floating(const float value) { appendLittleEndianFloat(output, value); }

  void string(const std::string_view value) {
    pod(static_cast<std::uint32_t>(value.size()));
    const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
    output.insert(output.end(), bytes.begin(), bytes.end());
  }

  std::vector<std::byte> output;
};

class Reader {
public:
  explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

  template <typename T>
    requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
  T pod() {
    const T value = readLittleEndian<T>(bytes_, offset_);
    offset_ += sizeof(T);
    return value;
  }

  float floating() {
    const float value = readLittleEndianFloat(bytes_, offset_);
    offset_ += sizeof(float);
    return value;
  }

  std::string string(const std::size_t maximum) {
    const std::size_t count = pod<std::uint32_t>();
    require(count);
    if (count > maximum)
      throw std::runtime_error("Cooked world name exceeds configured limit");
    const auto *begin = reinterpret_cast<const char *>(bytes_.data() + offset_);
    std::string result{begin, count};
    offset_ += count;
    return result;
  }

  [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }

private:
  void require(const std::size_t count) const {
    if (count > bytes_.size() - offset_)
      throw std::runtime_error("Cooked world is truncated");
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0U;
};

void writeAssetId(Writer &writer, const AssetId id) {
  writer.pod(id.high);
  writer.pod(id.low);
}

AssetId readAssetId(Reader &reader) {
  return {reader.pod<std::uint64_t>(), reader.pod<std::uint64_t>()};
}

void writeTransform(Writer &writer, const TransformTRS &transform) {
  writer.floating(transform.translation.x);
  writer.floating(transform.translation.y);
  writer.floating(transform.translation.z);
  writer.floating(transform.rotation.x);
  writer.floating(transform.rotation.y);
  writer.floating(transform.rotation.z);
  writer.floating(transform.rotation.w);
  writer.floating(transform.scale.x);
  writer.floating(transform.scale.y);
  writer.floating(transform.scale.z);
}

TransformTRS readTransform(Reader &reader) {
  TransformTRS transform;
  transform.translation = {reader.floating(), reader.floating(),
                           reader.floating()};
  transform.rotation = {reader.floating(), reader.floating(), reader.floating(),
                        reader.floating()};
  transform.scale = {reader.floating(), reader.floating(), reader.floating()};
  return transform;
}

bool validResolvedMaterial(const RenderMaterial &material) {
  const auto finite4 = [](const Vec4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
  };
  return finite4(material.albedoRoughness) &&
         finite4(material.emissiveMetallic) && finite4(material.flags) &&
         material.flags.y >=
             static_cast<float>(RenderMaterialClass::Standard) &&
         material.flags.y <=
             static_cast<float>(RenderMaterialClass::Emissive) &&
         material.flags.z >= 0.0F && material.flags.w >= 0.0F;
}

} // namespace

void validateCookedWorld(const CookedWorld &world,
                         const CookedWorldLimits &limits) {
  const std::size_t count = world.identities.size();
  if (limits.maximumEntities == 0U || limits.maximumBytes == 0U ||
      limits.maximumNameBytes == 0U) {
    throw std::invalid_argument("Cooked world limits must be positive");
  }
  if (count > limits.maximumEntities || world.names.size() != count ||
      world.parentIndices.size() != count || world.transforms.size() != count ||
      world.renderableMask.size() != count ||
      world.renderables.size() != count) {
    throw std::runtime_error(
        "Cooked world structure-of-arrays lengths are inconsistent");
  }
  for (std::size_t index = 0U; index < count; ++index) {
    if (!world.identities[index].valid() ||
        world.names[index].size() > limits.maximumNameBytes ||
        !validWorldSceneName(world.names[index]) ||
        !finite(world.transforms[index]) ||
        world.transforms[index].scale.x == 0.0F ||
        world.transforms[index].scale.y == 0.0F ||
        world.transforms[index].scale.z == 0.0F ||
        world.renderableMask[index] > 1U) {
      throw std::runtime_error("Cooked world entity data is invalid");
    }
    for (std::size_t prior = 0U; prior < index; ++prior) {
      if (world.identities[prior] == world.identities[index]) {
        throw std::runtime_error("Cooked world identity is duplicated");
      }
    }
    const std::uint32_t parent = world.parentIndices[index];
    if (parent != kNoParent && (parent >= count || parent == index)) {
      throw std::runtime_error("Cooked world parent index is invalid");
    }
    if (world.renderableMask[index] != 0U &&
        (!world.renderables[index].mesh.valid() ||
         !world.renderables[index].material.valid())) {
      throw std::runtime_error("Cooked world renderable identity is invalid");
    }
  }
  for (std::size_t index = 0U; index < count; ++index) {
    std::uint32_t parent = world.parentIndices[index];
    std::size_t depth = 0U;
    while (parent != kNoParent) {
      if (++depth > count)
        throw std::runtime_error("Cooked world hierarchy contains a cycle");
      parent = world.parentIndices[parent];
    }
  }
}

std::vector<std::byte> encodeCookedWorld(const CookedWorld &world,
                                         const CookedWorldLimits &limits) {
  validateCookedWorld(world, limits);
  Writer writer;
  writer.pod(kCookedMagic);
  writer.pod(kCookedVersion);
  writer.pod(static_cast<std::uint64_t>(world.identities.size()));
  for (const SceneEntityId id : world.identities) {
    writer.pod(id.high);
    writer.pod(id.low);
  }
  for (const std::string &name : world.names)
    writer.string(name);
  for (const std::uint32_t parent : world.parentIndices)
    writer.pod(parent);
  for (const TransformTRS &transform : world.transforms)
    writeTransform(writer, transform);
  for (const std::uint8_t mask : world.renderableMask)
    writer.pod(mask);
  for (std::size_t index = 0U; index < world.renderables.size(); ++index) {
    if (world.renderableMask[index] == 0U)
      continue;
    writeAssetId(writer, world.renderables[index].mesh);
    writeAssetId(writer, world.renderables[index].material);
    writer.pod(
        static_cast<std::uint8_t>(world.renderables[index].visible ? 1U : 0U));
  }
  if (writer.output.size() > limits.maximumBytes) {
    throw std::runtime_error("Encoded cooked world exceeds configured limit");
  }
  return writer.output;
}

CookedWorld decodeCookedWorld(const std::span<const std::byte> bytes,
                              const CookedWorldLimits &limits) {
  if (bytes.size() > limits.maximumBytes)
    throw std::runtime_error("Cooked world exceeds configured limit");
  Reader reader{bytes};
  if (reader.pod<std::uint32_t>() != kCookedMagic ||
      reader.pod<std::uint32_t>() != kCookedVersion) {
    throw std::runtime_error("Cooked world header is incompatible");
  }
  const std::size_t count = reader.pod<std::uint64_t>();
  if (count > limits.maximumEntities)
    throw std::runtime_error(
        "Cooked world entity count exceeds configured limit");
  CookedWorld world;
  world.identities.resize(count);
  world.names.resize(count);
  world.parentIndices.resize(count);
  world.transforms.resize(count);
  world.renderableMask.resize(count);
  world.renderables.resize(count);
  for (SceneEntityId &id : world.identities) {
    id = {reader.pod<std::uint64_t>(), reader.pod<std::uint64_t>()};
  }
  for (std::string &name : world.names)
    name = reader.string(limits.maximumNameBytes);
  for (std::uint32_t &parent : world.parentIndices)
    parent = reader.pod<std::uint32_t>();
  for (TransformTRS &transform : world.transforms)
    transform = readTransform(reader);
  for (std::uint8_t &mask : world.renderableMask)
    mask = reader.pod<std::uint8_t>();
  for (std::size_t index = 0U; index < count; ++index) {
    if (world.renderableMask[index] == 0U)
      continue;
    world.renderables[index].mesh = readAssetId(reader);
    world.renderables[index].material = readAssetId(reader);
    const std::uint8_t visible = reader.pod<std::uint8_t>();
    if (visible > 1U)
      throw std::runtime_error("Cooked renderable visibility is invalid");
    world.renderables[index].visible = visible != 0U;
  }
  if (!reader.done())
    throw std::runtime_error("Cooked world has trailing data");
  validateCookedWorld(world, limits);
  return world;
}

void saveCookedWorld(const CookedWorld &world,
                     const std::filesystem::path &path,
                     const CookedWorldLimits &limits) {
  writeBinaryFileAtomic(path, encodeCookedWorld(world, limits));
}

CookedWorld loadCookedWorld(const std::filesystem::path &path,
                            const CookedWorldLimits &limits) {
  return decodeCookedWorld(readBinaryFile(path, limits.maximumBytes), limits);
}

void instantiateCookedWorld(World &destination, const CookedWorld &source,
                            const CookedWorldAssetResolver &resolver,
                            const CookedWorldLimits &limits) {
  validateCookedWorld(source, limits);
  if (std::ranges::contains(source.renderableMask, std::uint8_t{1}) &&
      (resolver.mesh == nullptr || resolver.material == nullptr ||
       resolver.bounds == nullptr)) {
    throw std::invalid_argument("Cooked world asset resolver is incomplete");
  }
  World temporary;
  const std::size_t count = source.identities.size();
  temporary.reserveEntities(count);
  temporary.reserveComponents<WorldSceneIdentity>(count);
  temporary.reserveComponents<WorldSceneTransform>(count);
  temporary.reserveComponents<WorldSceneParent>(count);
  temporary.reserveComponents<WorldSceneRenderable>(count);
  std::vector<World::Entity> entities;
  entities.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    const World::Entity entity = temporary.createEntity();
    entities.push_back(entity);
    temporary.emplace<WorldSceneIdentity>(
        entity,
        WorldSceneIdentity{source.identities[index], source.names[index]});
    temporary.emplace<WorldSceneTransform>(
        entity, WorldSceneTransform{source.transforms[index], 0U});
    if (source.renderableMask[index] != 0U) {
      const MeshAssetHandle mesh =
          resolver.mesh(resolver.context, source.renderables[index].mesh);
      const MeshBounds bounds = resolver.bounds(resolver.context, mesh);
      const RenderMaterial material = resolver.material(
          resolver.context, source.renderables[index].material);
      if (!mesh.valid() || !bounds.valid || bounds.radius < 0.0F ||
          !std::isfinite(bounds.radius) || !finite(bounds.center) ||
          !validResolvedMaterial(material) ||
          std::ranges::any_of(
              material.textures,
              [](const TextureAssetHandle texture) {
                return !texture.valid();
              })) {
        throw std::runtime_error(
            "Cooked world resolver returned invalid runtime assets");
      }
      temporary.emplace<WorldSceneRenderable>(
          entity, WorldSceneRenderable{mesh, material, bounds,
                                       source.renderables[index].visible});
    }
  }
  for (std::size_t index = 0U; index < count; ++index) {
    if (source.parentIndices[index] != kNoParent) {
      temporary.emplace<WorldSceneParent>(
          entities[index],
          WorldSceneParent{entities[source.parentIndices[index]]});
    }
  }
  destination = std::move(temporary);
}

} // namespace ve
