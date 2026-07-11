#include "scene/SceneReflection.hpp"

#include "SceneSchema.generated.hpp"
#include "core/BinaryIO.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace ve {
namespace {

constexpr std::size_t kTransformPayloadBytes = 10U * sizeof(float);
constexpr std::size_t kLegacyTransformPayloadBytes = 6U * sizeof(float);
constexpr std::size_t kRenderablePayloadBytes = 4U * sizeof(std::uint64_t) + 1U;

void appendFloat(std::vector<std::byte> &bytes, const float value) {
  appendLittleEndianFloat(bytes, value);
}

float readFloat(const std::span<const std::byte> bytes,
                const std::size_t offset) {
  return readLittleEndianFloat(bytes, offset);
}

void appendU64(std::vector<std::byte> &bytes, const std::uint64_t value) {
  appendLittleEndian(bytes, value);
}

std::uint64_t readU64(const std::span<const std::byte> bytes,
                      const std::size_t offset) {
  return readLittleEndian<std::uint64_t>(bytes, offset);
}

ScenePropertyId propertyId(const std::string_view type,
                           const std::string_view property) {
  return stableSceneId(std::string{type} + "." + std::string{property});
}

void validateTransformPayload(const std::span<const std::byte> payload) {
  static_cast<void>(decodeAuthoringTransform(payload));
}

std::vector<std::byte>
migrateTransformPayload(const std::uint32_t sourceVersion,
                        const std::span<const std::byte> payload) {
  if (sourceVersion != 1U || payload.size() != kLegacyTransformPayloadBytes) {
    throw std::runtime_error("Transform component migration is unsupported");
  }
  TransformTRS transform;
  transform.translation = {readFloat(payload, 0U), readFloat(payload, 4U),
                           readFloat(payload, 8U)};
  transform.scale = {readFloat(payload, 12U), readFloat(payload, 16U),
                     readFloat(payload, 20U)};
  return encodeAuthoringTransform(transform);
}

ScenePropertyValue
readTransformProperty(const std::span<const std::byte> payload,
                      const ScenePropertyId property) {
  const TransformTRS transform = decodeAuthoringTransform(payload);
  if (property == propertyId("ve.scene.transform", "translation")) {
    return transform.translation;
  }
  if (property == propertyId("ve.scene.transform", "rotation")) {
    return transform.rotation;
  }
  if (property == propertyId("ve.scene.transform", "scale")) {
    return transform.scale;
  }
  throw std::invalid_argument("Unknown transform property");
}

std::vector<std::byte>
writeTransformProperty(const std::span<const std::byte> payload,
                       const ScenePropertyId property,
                       const ScenePropertyValue &value) {
  TransformTRS transform = decodeAuthoringTransform(payload);
  if (property == propertyId("ve.scene.transform", "translation")) {
    transform.translation = std::get<Vec3>(value);
  } else if (property == propertyId("ve.scene.transform", "rotation")) {
    transform.rotation = std::get<Quat>(value);
  } else if (property == propertyId("ve.scene.transform", "scale")) {
    transform.scale = std::get<Vec3>(value);
  } else {
    throw std::invalid_argument("Unknown transform property");
  }
  return encodeAuthoringTransform(transform);
}

void validateRenderablePayload(const std::span<const std::byte> payload) {
  static_cast<void>(decodeAuthoringRenderable(payload));
}

std::vector<std::byte>
migrateRenderablePayload(const std::uint32_t,
                         const std::span<const std::byte>) {
  throw std::runtime_error("Renderable component migration is unsupported");
}

ScenePropertyValue
readRenderableProperty(const std::span<const std::byte> payload,
                       const ScenePropertyId property) {
  const AuthoringRenderable renderable = decodeAuthoringRenderable(payload);
  if (property == propertyId("ve.scene.renderable", "mesh")) {
    return renderable.mesh;
  }
  if (property == propertyId("ve.scene.renderable", "material")) {
    return renderable.material;
  }
  if (property == propertyId("ve.scene.renderable", "visible")) {
    return renderable.visible;
  }
  throw std::invalid_argument("Unknown renderable property");
}

std::vector<std::byte>
writeRenderableProperty(const std::span<const std::byte> payload,
                        const ScenePropertyId property,
                        const ScenePropertyValue &value) {
  AuthoringRenderable renderable = decodeAuthoringRenderable(payload);
  if (property == propertyId("ve.scene.renderable", "mesh")) {
    renderable.mesh = std::get<AssetId>(value);
  } else if (property == propertyId("ve.scene.renderable", "material")) {
    renderable.material = std::get<AssetId>(value);
  } else if (property == propertyId("ve.scene.renderable", "visible")) {
    renderable.visible = std::get<bool>(value);
  } else {
    throw std::invalid_argument("Unknown renderable property");
  }
  return encodeAuthoringRenderable(renderable);
}

ScenePropertyKind propertyKind(const std::string_view value) {
  if (value == "Bool")
    return ScenePropertyKind::Bool;
  if (value == "Float")
    return ScenePropertyKind::Float;
  if (value == "Vec3")
    return ScenePropertyKind::Vec3;
  if (value == "Quaternion")
    return ScenePropertyKind::Quaternion;
  if (value == "AssetId")
    return ScenePropertyKind::AssetId;
  if (value == "String")
    return ScenePropertyKind::String;
  throw std::runtime_error(
      "External scene schema has an unknown property kind");
}

std::vector<std::string> split(const std::string_view line) {
  std::vector<std::string> fields;
  std::size_t begin = 0U;
  while (begin <= line.size()) {
    const std::size_t end = line.find('|', begin);
    fields.emplace_back(line.substr(begin, end == std::string_view::npos
                                               ? line.size() - begin
                                               : end - begin));
    if (end == std::string_view::npos)
      break;
    begin = end + 1U;
  }
  return fields;
}

} // namespace

void SceneTypeRegistry::registerType(SceneTypeMetadata metadata) {
  if (metadata.id == 0U || metadata.name.empty() ||
      metadata.displayName.empty() || metadata.version == 0U) {
    throw std::invalid_argument("Scene type metadata is incomplete");
  }
  if (find(metadata.id) != nullptr || find(metadata.name) != nullptr) {
    throw std::invalid_argument("Scene type metadata is duplicated");
  }
  for (std::size_t index = 0U; index < metadata.properties.size(); ++index) {
    const ScenePropertyMetadata &property = metadata.properties[index];
    if (property.id == 0U || property.name.empty() ||
        property.displayName.empty() || property.futureBindingType.empty() ||
        !std::isfinite(property.inspector.minimum) ||
        !std::isfinite(property.inspector.maximum) ||
        !std::isfinite(property.inspector.step) ||
        property.inspector.step < 0.0) {
      throw std::invalid_argument("Scene property metadata is incomplete");
    }
    for (std::size_t prior = 0U; prior < index; ++prior) {
      if (metadata.properties[prior].id == property.id ||
          metadata.properties[prior].name == property.name) {
        throw std::invalid_argument("Scene property metadata is duplicated");
      }
    }
  }
  types_.push_back(std::move(metadata));
}

void SceneTypeRegistry::bindHooks(const SceneTypeId type,
                                  SceneTypeHooks hooks) {
  SceneTypeMetadata *metadata = nullptr;
  for (SceneTypeMetadata &candidate : types_) {
    if (candidate.id == type)
      metadata = &candidate;
  }
  if (metadata == nullptr)
    throw std::invalid_argument("Cannot bind hooks for an unknown scene type");
  if (hooks.validate == nullptr || hooks.migrate == nullptr ||
      hooks.read == nullptr || hooks.write == nullptr) {
    throw std::invalid_argument("Scene type hooks must be complete");
  }
  metadata->hooks = hooks;
}

const SceneTypeMetadata *
SceneTypeRegistry::find(const SceneTypeId type) const noexcept {
  const auto found = std::ranges::find(types_, type, &SceneTypeMetadata::id);
  return found == types_.end() ? nullptr : &*found;
}

const SceneTypeMetadata *
SceneTypeRegistry::find(const std::string_view name) const noexcept {
  const auto found = std::ranges::find(types_, name, &SceneTypeMetadata::name);
  return found == types_.end() ? nullptr : &*found;
}

std::string SceneTypeRegistry::bindingManifest() const {
  std::vector<const SceneTypeMetadata *> ordered;
  ordered.reserve(types_.size());
  for (const SceneTypeMetadata &type : types_)
    ordered.push_back(&type);
  std::ranges::sort(ordered, {},
                    [](const SceneTypeMetadata *type) { return type->id; });
  std::ostringstream output;
  for (const SceneTypeMetadata *type : ordered) {
    output << type->id << '|' << type->name << '|' << type->displayName << '|'
           << type->version << '\n';
    std::vector<const ScenePropertyMetadata *> properties;
    properties.reserve(type->properties.size());
    for (const ScenePropertyMetadata &property : type->properties) {
      properties.push_back(&property);
    }
    std::ranges::sort(
        properties, {},
        [](const ScenePropertyMetadata *property) { return property->id; });
    for (const ScenePropertyMetadata *property : properties) {
      output << "  " << property->id << '|' << property->name << '|'
             << property->displayName << '|'
             << static_cast<unsigned>(property->kind) << '|'
             << property->inspector.minimum << '|'
             << property->inspector.maximum << '|' << property->inspector.step
             << '|' << property->futureBindingType << '\n';
    }
  }
  return output.str();
}

ScenePropertyMetadata
makeScenePropertyMetadata(std::string displayName, std::string name,
                          const ScenePropertyKind kind, const double minimum,
                          const double maximum, const double step,
                          std::string futureBindingType) {
  ScenePropertyMetadata property;
  property.displayName = std::move(displayName);
  property.name = std::move(name);
  property.kind = kind;
  property.inspector = {minimum, maximum, step};
  property.futureBindingType = std::move(futureBindingType);
  return property;
}

SceneTypeMetadata
makeSceneTypeMetadata(std::string displayName, std::string name,
                      const std::uint32_t version,
                      std::vector<ScenePropertyMetadata> properties) {
  SceneTypeMetadata metadata;
  metadata.displayName = std::move(displayName);
  metadata.name = std::move(name);
  metadata.id = stableSceneId(metadata.name);
  metadata.version = version;
  for (ScenePropertyMetadata &property : properties) {
    property.id = propertyId(metadata.name, property.name);
  }
  metadata.properties = std::move(properties);
  return metadata;
}

SceneTypeRegistry explicitSceneTypeRegistry() {
  SceneTypeRegistry registry;
  registry.registerType(makeSceneTypeMetadata(
      "Transform", "ve.scene.transform", 2U,
      {makeScenePropertyMetadata("Translation", "translation",
                                 ScenePropertyKind::Vec3, -100000.0, 100000.0,
                                 0.1, "System.Numerics.Vector3"),
       makeScenePropertyMetadata("Rotation", "rotation",
                                 ScenePropertyKind::Quaternion, -1.0, 1.0, 0.01,
                                 "System.Numerics.Quaternion"),
       makeScenePropertyMetadata("Scale", "scale", ScenePropertyKind::Vec3,
                                 -10000.0, 10000.0, 0.01,
                                 "System.Numerics.Vector3")}));
  registry.registerType(makeSceneTypeMetadata(
      "Renderable", "ve.scene.renderable", 1U,
      {makeScenePropertyMetadata("Mesh", "mesh", ScenePropertyKind::AssetId,
                                 0.0, 0.0, 0.0, "VolkEngine.AssetId"),
       makeScenePropertyMetadata("Material", "material",
                                 ScenePropertyKind::AssetId, 0.0, 0.0, 0.0,
                                 "VolkEngine.AssetId"),
       makeScenePropertyMetadata("Visible", "visible", ScenePropertyKind::Bool,
                                 0.0, 1.0, 1.0, "System.Boolean")}));
  bindBuiltinSceneTypeHooks(registry);
  return registry;
}

SceneTypeRegistry externalSceneTypeRegistry(const std::string_view schema) {
  SceneTypeRegistry registry;
  std::vector<SceneTypeMetadata> types;
  std::size_t begin = 0U;
  while (begin < schema.size()) {
    const std::size_t end = schema.find('\n', begin);
    const std::string_view line = schema.substr(
        begin,
        end == std::string_view::npos ? schema.size() - begin : end - begin);
    begin = end == std::string_view::npos ? schema.size() : end + 1U;
    if (line.empty())
      continue;
    const std::vector<std::string> fields = split(line);
    if (fields[0] == "component") {
      if (fields.size() != 4U)
        throw std::runtime_error(
            "External component schema field count is invalid");
      types.push_back(makeSceneTypeMetadata(
          fields[1], fields[2],
          static_cast<std::uint32_t>(std::stoul(fields[3])), {}));
    } else if (fields[0] == "property") {
      if (fields.size() != 9U)
        throw std::runtime_error(
            "External property schema field count is invalid");
      const auto found =
          std::ranges::find(types, fields[1], &SceneTypeMetadata::displayName);
      if (found == types.end())
        throw std::runtime_error("External property precedes its component");
      ScenePropertyMetadata property = makeScenePropertyMetadata(
          fields[2], fields[3], propertyKind(fields[4]), std::stod(fields[5]),
          std::stod(fields[6]), std::stod(fields[7]), fields[8]);
      property.id = propertyId(found->name, property.name);
      found->properties.push_back(std::move(property));
    } else {
      throw std::runtime_error("External scene schema record is unknown");
    }
  }
  for (SceneTypeMetadata &type : types)
    registry.registerType(std::move(type));
  bindBuiltinSceneTypeHooks(registry);
  return registry;
}

void bindBuiltinSceneTypeHooks(SceneTypeRegistry &registry) {
  registry.bindHooks(kTransformSceneType,
                     {validateTransformPayload, migrateTransformPayload,
                      readTransformProperty, writeTransformProperty});
  registry.bindHooks(kRenderableSceneType,
                     {validateRenderablePayload, migrateRenderablePayload,
                      readRenderableProperty, writeRenderableProperty});
}

const SceneTypeRegistry &builtinSceneTypeRegistry() {
  static const SceneTypeRegistry registry = generatedSceneTypeRegistry();
  return registry;
}

std::vector<std::byte> encodeAuthoringTransform(const TransformTRS &transform) {
  if (!finite(transform.translation) || !finite(transform.rotation) ||
      !finite(transform.scale) || transform.scale.x == 0.0F ||
      transform.scale.y == 0.0F || transform.scale.z == 0.0F) {
    throw std::invalid_argument("Authoring transform is invalid");
  }
  std::vector<std::byte> payload;
  payload.reserve(kTransformPayloadBytes);
  appendFloat(payload, transform.translation.x);
  appendFloat(payload, transform.translation.y);
  appendFloat(payload, transform.translation.z);
  appendFloat(payload, transform.rotation.x);
  appendFloat(payload, transform.rotation.y);
  appendFloat(payload, transform.rotation.z);
  appendFloat(payload, transform.rotation.w);
  appendFloat(payload, transform.scale.x);
  appendFloat(payload, transform.scale.y);
  appendFloat(payload, transform.scale.z);
  return payload;
}

TransformTRS
decodeAuthoringTransform(const std::span<const std::byte> payload) {
  if (payload.size() != kTransformPayloadBytes) {
    throw std::runtime_error("Transform component payload size is invalid");
  }
  TransformTRS transform;
  transform.translation = {readFloat(payload, 0U), readFloat(payload, 4U),
                           readFloat(payload, 8U)};
  transform.rotation = {readFloat(payload, 12U), readFloat(payload, 16U),
                        readFloat(payload, 20U), readFloat(payload, 24U)};
  transform.scale = {readFloat(payload, 28U), readFloat(payload, 32U),
                     readFloat(payload, 36U)};
  if (!finite(transform.translation) || !finite(transform.rotation) ||
      !finite(transform.scale) || transform.scale.x == 0.0F ||
      transform.scale.y == 0.0F || transform.scale.z == 0.0F) {
    throw std::runtime_error(
        "Transform component payload contains invalid values");
  }
  return transform;
}

std::vector<std::byte>
encodeAuthoringRenderable(const AuthoringRenderable &renderable) {
  if (!renderable.mesh.valid() || !renderable.material.valid()) {
    throw std::invalid_argument(
        "Authoring renderable asset identity is invalid");
  }
  std::vector<std::byte> payload;
  payload.reserve(kRenderablePayloadBytes);
  appendU64(payload, renderable.mesh.high);
  appendU64(payload, renderable.mesh.low);
  appendU64(payload, renderable.material.high);
  appendU64(payload, renderable.material.low);
  payload.push_back(renderable.visible ? std::byte{1} : std::byte{0});
  return payload;
}

AuthoringRenderable
decodeAuthoringRenderable(const std::span<const std::byte> payload) {
  if (payload.size() != kRenderablePayloadBytes) {
    throw std::runtime_error("Renderable component payload size is invalid");
  }
  AuthoringRenderable renderable;
  renderable.mesh = {readU64(payload, 0U), readU64(payload, 8U)};
  renderable.material = {readU64(payload, 16U), readU64(payload, 24U)};
  const std::uint8_t visible = std::to_integer<std::uint8_t>(payload[32U]);
  if (!renderable.mesh.valid() || !renderable.material.valid() ||
      visible > 1U) {
    throw std::runtime_error(
        "Renderable component payload contains invalid values");
  }
  renderable.visible = visible != 0U;
  return renderable;
}

} // namespace ve
