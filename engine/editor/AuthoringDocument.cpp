#include "editor/AuthoringDocument.hpp"

#include "core/BinaryIO.hpp"
#include "core/FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace ve::editor {
namespace {

constexpr std::uint32_t kAuthoringMagic = 0x55414556U;
constexpr std::uint32_t kAuthoringVersion = 1U;
constexpr std::uint64_t kEditorEntityNamespace = 0x5645415545444954ULL;

static_assert(std::is_nothrow_move_constructible_v<DocumentCommand>);

class Writer {
public:
  template <typename T>
    requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
  void pod(const T value) {
    appendLittleEndian(output, value);
  }

  void bytes(const std::span<const std::byte> value) {
    output.insert(output.end(), value.begin(), value.end());
  }

  void string(const std::string_view value) {
    pod(static_cast<std::uint32_t>(value.size()));
    bytes(std::as_bytes(std::span{value.data(), value.size()}));
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

  std::span<const std::byte> bytes(const std::size_t count) {
    require(count);
    const auto result = bytes_.subspan(offset_, count);
    offset_ += count;
    return result;
  }

  std::string string(const std::size_t maximum) {
    const std::size_t size = pod<std::uint32_t>();
    if (size > maximum)
      throw std::runtime_error("Authoring name exceeds configured limit");
    const auto value = bytes(size);
    return {reinterpret_cast<const char *>(value.data()), value.size()};
  }

  [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }

private:
  void require(const std::size_t count) const {
    if (count > bytes_.size() - offset_) {
      throw std::runtime_error("Authoring document is truncated");
    }
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0U;
};

bool contains(const std::span<const SceneEntityId> ids,
              const SceneEntityId id) {
  return std::ranges::find(ids, id) != ids.end();
}

TransformTRS decomposeTransform(const Mat4 &matrix) {
  if (!std::ranges::all_of(
          matrix.m, [](const float value) { return std::isfinite(value); })) {
    throw std::runtime_error("Imported node transform is non-finite");
  }
  TransformTRS transform;
  transform.translation = {matrix.m[12], matrix.m[13], matrix.m[14]};
  Vec3 x{matrix.m[0], matrix.m[1], matrix.m[2]};
  Vec3 y{matrix.m[4], matrix.m[5], matrix.m[6]};
  Vec3 z{matrix.m[8], matrix.m[9], matrix.m[10]};
  transform.scale = {length(x), length(y), length(z)};
  if (transform.scale.x <= 0.000001F || transform.scale.y <= 0.000001F ||
      transform.scale.z <= 0.000001F) {
    throw std::runtime_error("Imported node transform has a singular scale");
  }
  x = x / transform.scale.x;
  y = y / transform.scale.y;
  z = z / transform.scale.z;
  if (dot(x, cross(y, z)) < 0.0F) {
    transform.scale.x = -transform.scale.x;
    x = x * -1.0F;
  }
  const float trace = x.x + y.y + z.z;
  Quat rotation;
  if (trace > 0.0F) {
    const float s = std::sqrt(trace + 1.0F) * 2.0F;
    rotation = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25F * s};
  } else if (x.x > y.y && x.x > z.z) {
    const float s = std::sqrt(1.0F + x.x - y.y - z.z) * 2.0F;
    rotation = {0.25F * s, (x.y + y.x) / s, (z.x + x.z) / s, (y.z - z.y) / s};
  } else if (y.y > z.z) {
    const float s = std::sqrt(1.0F + y.y - x.x - z.z) * 2.0F;
    rotation = {(x.y + y.x) / s, 0.25F * s, (y.z + z.y) / s, (z.x - x.z) / s};
  } else {
    const float s = std::sqrt(1.0F + z.z - x.x - y.y) * 2.0F;
    rotation = {(z.x + x.z) / s, (y.z + z.y) / s, 0.25F * s, (x.y - y.x) / s};
  }
  transform.rotation = normalizeQuat(rotation);
  return transform;
}

SceneEntityId importedId(const AssetId scene, const std::string &suffix) {
  const AssetId id = AssetId::derive(scene, suffix);
  return {id.high, id.low};
}

} // namespace

AuthoringDocument::AuthoringDocument(const SceneTypeRegistry &registry,
                                     AuthoringLimits limits)
    : registry_(&registry), limits_(limits) {
  if (limits_.maximumEntities == 0U ||
      limits_.maximumComponentsPerEntity == 0U ||
      limits_.maximumComponentBytes == 0U ||
      limits_.maximumDocumentBytes == 0U || limits_.maximumNameBytes == 0U ||
      limits_.maximumHistory == 0U) {
    throw std::invalid_argument("Authoring limits must be positive");
  }
  markSaved();
}

const AuthoringEntity *
AuthoringDocument::find(const SceneEntityId id) const noexcept {
  const auto found = std::ranges::find(entities_, id, &AuthoringEntity::id);
  return found == entities_.end() ? nullptr : &*found;
}

AuthoringEntity *AuthoringDocument::find(const SceneEntityId id) noexcept {
  const auto found = std::ranges::find(entities_, id, &AuthoringEntity::id);
  return found == entities_.end() ? nullptr : &*found;
}

const AuthoringComponent *
AuthoringDocument::component(const SceneEntityId entity,
                             const SceneTypeId type) const noexcept {
  const AuthoringEntity *owner = find(entity);
  if (owner == nullptr)
    return nullptr;
  const auto found =
      std::ranges::find(owner->components, type, &AuthoringComponent::type);
  return found == owner->components.end() ? nullptr : &*found;
}

SceneEntityId AuthoringDocument::nextEntityId() const {
  std::uint64_t low = 1U;
  for (const AuthoringEntity &entity : entities_) {
    if (entity.id.high == kEditorEntityNamespace) {
      low = std::max(low, entity.id.low + 1U);
    }
  }
  if (low == 0U)
    throw std::runtime_error("Authoring entity identity range is exhausted");
  return {kEditorEntityNamespace, low};
}

SceneEntityId AuthoringDocument::create(std::string name,
                                        const SceneEntityId parent) {
  AuthoringEntity entity;
  entity.id = nextEntityId();
  entity.name = std::move(name);
  entity.parent = parent;
  entity.components.push_back(
      {kTransformSceneType, 2U, encodeAuthoringTransform({})});
  DocumentCommand command;
  command.label = "Create entity";
  command.patches.push_back({entity.id, std::nullopt, entity});
  execute(std::move(command));
  return entity.id;
}

void AuthoringDocument::erase(const SceneEntityId entity) {
  if (find(entity) == nullptr)
    throw std::invalid_argument("Cannot delete an unknown entity");
  std::vector<SceneEntityId> deleted{entity};
  bool changed = true;
  while (changed) {
    changed = false;
    for (const AuthoringEntity &candidate : entities_) {
      if (!contains(deleted, candidate.id) &&
          contains(deleted, candidate.parent)) {
        deleted.push_back(candidate.id);
        changed = true;
      }
    }
  }
  DocumentCommand command;
  command.label = "Delete entities";
  for (const AuthoringEntity &candidate : entities_) {
    if (contains(deleted, candidate.id)) {
      command.patches.push_back({candidate.id, candidate, std::nullopt});
    }
  }
  execute(std::move(command));
}

DocumentCommand
AuthoringDocument::entityReplacement(std::string label,
                                     const AuthoringEntity &before,
                                     AuthoringEntity after) const {
  DocumentCommand command;
  command.label = std::move(label);
  command.patches.push_back({before.id, before, std::move(after)});
  return command;
}

void AuthoringDocument::rename(const SceneEntityId entity, std::string name) {
  const AuthoringEntity *before = find(entity);
  if (before == nullptr)
    throw std::invalid_argument("Cannot rename an unknown entity");
  AuthoringEntity after = *before;
  after.name = std::move(name);
  execute(entityReplacement("Rename entity", *before, std::move(after)));
}

void AuthoringDocument::reparent(const SceneEntityId entity,
                                 const SceneEntityId parent) {
  const AuthoringEntity *before = find(entity);
  if (before == nullptr)
    throw std::invalid_argument("Cannot reparent an unknown entity");
  if (parent.valid() && find(parent) == nullptr) {
    throw std::invalid_argument("Cannot reparent to an unknown entity");
  }
  AuthoringEntity after = *before;
  after.parent = parent;
  execute(entityReplacement("Reparent entity", *before, std::move(after)));
}

void AuthoringDocument::setComponent(const SceneEntityId entity,
                                     AuthoringComponent componentValue) {
  const AuthoringEntity *before = find(entity);
  if (before == nullptr)
    throw std::invalid_argument("Cannot edit an unknown entity");
  AuthoringEntity after = *before;
  const auto found = std::ranges::find(after.components, componentValue.type,
                                       &AuthoringComponent::type);
  if (found == after.components.end()) {
    after.components.push_back(std::move(componentValue));
  } else {
    *found = std::move(componentValue);
  }
  execute(entityReplacement("Set component", *before, std::move(after)));
}

void AuthoringDocument::removeComponent(const SceneEntityId entity,
                                        const SceneTypeId type) {
  const AuthoringEntity *before = find(entity);
  if (before == nullptr)
    throw std::invalid_argument("Cannot edit an unknown entity");
  AuthoringEntity after = *before;
  const auto found =
      std::ranges::find(after.components, type, &AuthoringComponent::type);
  if (found == after.components.end())
    return;
  after.components.erase(found);
  execute(entityReplacement("Remove component", *before, std::move(after)));
}

DocumentCommand AuthoringDocument::propertyCommand(
    const std::span<const SceneEntityId> entities, const SceneTypeId type,
    const ScenePropertyId property, const ScenePropertyValue &value,
    std::string label) const {
  const SceneTypeMetadata *metadata = registry_->find(type);
  if (metadata == nullptr || metadata->hooks.write == nullptr) {
    throw std::invalid_argument("Cannot edit an unknown reflected component");
  }
  if (entities.empty())
    throw std::invalid_argument("Property edit selection is empty");
  DocumentCommand command;
  command.label = std::move(label);
  command.patches.reserve(entities.size());
  for (const SceneEntityId id : entities) {
    const AuthoringEntity *before = find(id);
    if (before == nullptr)
      throw std::invalid_argument("Property edit references an unknown entity");
    AuthoringEntity after = *before;
    const auto found =
        std::ranges::find(after.components, type, &AuthoringComponent::type);
    if (found == after.components.end()) {
      throw std::invalid_argument("Property edit component is absent");
    }
    found->payload = metadata->hooks.write(found->payload, property, value);
    command.patches.push_back({id, *before, std::move(after)});
  }
  return command;
}

void AuthoringDocument::setProperty(
    const std::span<const SceneEntityId> entities, const SceneTypeId type,
    const ScenePropertyId property, const ScenePropertyValue &value,
    std::string label) {
  execute(propertyCommand(entities, type, property, value, std::move(label)));
}

void AuthoringDocument::validate(
    const std::span<const AuthoringEntity> entities) const {
  if (entities.size() > limits_.maximumEntities) {
    throw std::runtime_error("Authoring entity limit exceeded");
  }
  for (std::size_t index = 0U; index < entities.size(); ++index) {
    const AuthoringEntity &entity = entities[index];
    if (!entity.id.valid() || entity.name.size() > limits_.maximumNameBytes ||
        !validWorldSceneName(entity.name) ||
        entity.components.size() > limits_.maximumComponentsPerEntity) {
      throw std::runtime_error("Authoring entity metadata is invalid");
    }
    for (std::size_t prior = 0U; prior < index; ++prior) {
      if (entities[prior].id == entity.id) {
        throw std::runtime_error("Authoring entity identity is duplicated");
      }
    }
    if (entity.parent.valid() && entity.parent == entity.id) {
      throw std::runtime_error("Authoring entity cannot parent itself");
    }
    for (std::size_t componentIndex = 0U;
         componentIndex < entity.components.size(); ++componentIndex) {
      const AuthoringComponent &componentValue =
          entity.components[componentIndex];
      if (componentValue.type == 0U || componentValue.version == 0U ||
          componentValue.payload.size() > limits_.maximumComponentBytes) {
        throw std::runtime_error("Authoring component metadata is invalid");
      }
      for (std::size_t prior = 0U; prior < componentIndex; ++prior) {
        if (entity.components[prior].type == componentValue.type) {
          throw std::runtime_error("Authoring component type is duplicated");
        }
      }
      if (const SceneTypeMetadata *type = registry_->find(componentValue.type);
          type != nullptr) {
        if (componentValue.version != type->version) {
          throw std::runtime_error(
              "Known authoring component version is incompatible");
        }
        type->hooks.validate(componentValue.payload);
      }
    }
  }
  for (const AuthoringEntity &entity : entities) {
    if (entity.parent.valid() &&
        std::ranges::find(entities, entity.parent, &AuthoringEntity::id) ==
            entities.end()) {
      throw std::runtime_error("Authoring parent identity is missing");
    }
    SceneEntityId ancestor = entity.parent;
    std::size_t depth = 0U;
    while (ancestor.valid()) {
      if (++depth > entities.size()) {
        throw std::runtime_error("Authoring hierarchy contains a cycle");
      }
      const auto found =
          std::ranges::find(entities, ancestor, &AuthoringEntity::id);
      if (found == entities.end())
        break;
      ancestor = found->parent;
    }
  }
}

void AuthoringDocument::apply(const DocumentCommand &command,
                              const bool forward) {
  if (command.label.empty() || command.patches.empty()) {
    throw std::invalid_argument("Authoring command is empty");
  }
  std::vector<AuthoringEntity> candidate = entities_;
  for (std::size_t patchIndex = 0U; patchIndex < command.patches.size();
       ++patchIndex) {
    const EntityPatch &patch = command.patches[patchIndex];
    if (!patch.id.valid())
      throw std::invalid_argument("Authoring patch identity is invalid");
    for (std::size_t prior = 0U; prior < patchIndex; ++prior) {
      if (command.patches[prior].id == patch.id) {
        throw std::invalid_argument(
            "Authoring command patches an entity twice");
      }
    }
    const std::optional<AuthoringEntity> &expected =
        forward ? patch.before : patch.after;
    const std::optional<AuthoringEntity> &replacement =
        forward ? patch.after : patch.before;
    const auto found =
        std::ranges::find(candidate, patch.id, &AuthoringEntity::id);
    if (!expected.has_value()) {
      if (found != candidate.end())
        throw std::runtime_error("Authoring command creation state diverged");
    } else if (found == candidate.end() || *found != *expected) {
      throw std::runtime_error("Authoring command state diverged");
    }
    if (!replacement.has_value()) {
      if (found != candidate.end())
        candidate.erase(found);
    } else if (found == candidate.end()) {
      candidate.push_back(*replacement);
    } else {
      *found = *replacement;
    }
  }
  validate(candidate);
  std::ranges::sort(candidate, {}, &AuthoringEntity::id);
  entities_.swap(candidate);
  filterSelection();
}

void AuthoringDocument::execute(DocumentCommand command) {
  if (historyCursor_ == history_.size() &&
      history_.size() < limits_.maximumHistory &&
      history_.capacity() == history_.size()) {
    const std::size_t doubled =
        history_.capacity() > limits_.maximumHistory / 2U
            ? limits_.maximumHistory
            : std::min(limits_.maximumHistory,
                       std::max<std::size_t>(8U, history_.capacity() * 2U));
    history_.reserve(std::max(history_.size() + 1U, doubled));
  }
  apply(command, true);
  if (historyCursor_ < history_.size())
    history_.erase(history_.begin() +
                       static_cast<std::ptrdiff_t>(historyCursor_),
                   history_.end());
  if (history_.size() == limits_.maximumHistory) {
    history_.erase(history_.begin());
    if (historyCursor_ != 0U)
      --historyCursor_;
  }
  history_.push_back(std::move(command));
  historyCursor_ = history_.size();
}

void AuthoringDocument::applyPreview(const DocumentCommand &command) {
  apply(command, true);
}

void AuthoringDocument::undo() {
  if (!canUndo())
    throw std::logic_error("Authoring undo history is empty");
  const std::size_t target = historyCursor_ - 1U;
  apply(history_[target], false);
  historyCursor_ = target;
}

void AuthoringDocument::redo() {
  if (!canRedo())
    throw std::logic_error("Authoring redo history is empty");
  apply(history_[historyCursor_], true);
  ++historyCursor_;
}

void AuthoringDocument::clearHistory() noexcept {
  history_.clear();
  historyCursor_ = 0U;
}

void AuthoringDocument::select(const SceneEntityId entity,
                               const bool additive) {
  if (find(entity) == nullptr)
    throw std::invalid_argument("Cannot select an unknown entity");
  if (!additive)
    selection_.clear();
  if (!contains(selection_, entity))
    selection_.push_back(entity);
}

void AuthoringDocument::filterSelection() {
  std::erase_if(selection_,
                [&](const SceneEntityId id) { return find(id) == nullptr; });
}

std::vector<std::byte> AuthoringDocument::fingerprint() const {
  return encodeAuthoringDocument(*this);
}

bool AuthoringDocument::dirty() const {
  return fingerprint() != savedFingerprint_;
}

void AuthoringDocument::markSaved() { savedFingerprint_ = fingerprint(); }

void AuthoringDocument::replaceTransactional(
    std::vector<AuthoringEntity> entities) {
  validate(entities);
  std::ranges::sort(entities, {}, &AuthoringEntity::id);
  entities_.swap(entities);
  selection_.clear();
  clearHistory();
}

std::vector<std::byte>
encodeAuthoringDocument(const AuthoringDocument &document) {
  Writer writer;
  writer.pod(kAuthoringMagic);
  writer.pod(kAuthoringVersion);
  writer.pod(static_cast<std::uint64_t>(document.entities().size()));
  std::vector<const AuthoringEntity *> entities;
  entities.reserve(document.entities().size());
  for (const AuthoringEntity &entity : document.entities())
    entities.push_back(&entity);
  std::ranges::sort(entities, {},
                    [](const AuthoringEntity *entity) { return entity->id; });
  for (const AuthoringEntity *entity : entities) {
    writer.pod(entity->id.high);
    writer.pod(entity->id.low);
    writer.string(entity->name);
    writer.pod(entity->parent.high);
    writer.pod(entity->parent.low);
    std::vector<const AuthoringComponent *> components;
    components.reserve(entity->components.size());
    for (const AuthoringComponent &componentValue : entity->components)
      components.push_back(&componentValue);
    std::ranges::sort(components, {}, [](const AuthoringComponent *value) {
      return value->type;
    });
    writer.pod(static_cast<std::uint32_t>(components.size()));
    for (const AuthoringComponent *componentValue : components) {
      writer.pod(componentValue->type);
      writer.pod(componentValue->version);
      writer.pod(static_cast<std::uint64_t>(componentValue->payload.size()));
      writer.bytes(componentValue->payload);
    }
  }
  if (writer.output.size() > document.limits().maximumDocumentBytes) {
    throw std::runtime_error(
        "Encoded authoring document exceeds configured limit");
  }
  return writer.output;
}

void decodeAuthoringDocument(AuthoringDocument &destination,
                             const std::span<const std::byte> bytes) {
  if (bytes.size() > destination.limits().maximumDocumentBytes) {
    throw std::runtime_error("Authoring document exceeds configured limit");
  }
  Reader reader{bytes};
  if (reader.pod<std::uint32_t>() != kAuthoringMagic ||
      reader.pod<std::uint32_t>() != kAuthoringVersion) {
    throw std::runtime_error("Authoring document header is incompatible");
  }
  const std::size_t count = reader.pod<std::uint64_t>();
  if (count > destination.limits().maximumEntities) {
    throw std::runtime_error("Authoring entity count exceeds configured limit");
  }
  std::vector<AuthoringEntity> entities;
  entities.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    AuthoringEntity entity;
    entity.id = {reader.pod<std::uint64_t>(), reader.pod<std::uint64_t>()};
    entity.name = reader.string(destination.limits().maximumNameBytes);
    entity.parent = {reader.pod<std::uint64_t>(), reader.pod<std::uint64_t>()};
    const std::size_t componentCount = reader.pod<std::uint32_t>();
    if (componentCount > destination.limits().maximumComponentsPerEntity) {
      throw std::runtime_error(
          "Authoring component count exceeds configured limit");
    }
    entity.components.reserve(componentCount);
    for (std::size_t componentIndex = 0U; componentIndex < componentCount;
         ++componentIndex) {
      AuthoringComponent componentValue;
      componentValue.type = reader.pod<SceneTypeId>();
      componentValue.version = reader.pod<std::uint32_t>();
      const std::size_t payloadSize = reader.pod<std::uint64_t>();
      if (payloadSize > destination.limits().maximumComponentBytes) {
        throw std::runtime_error(
            "Authoring component payload exceeds configured limit");
      }
      const auto payload = reader.bytes(payloadSize);
      if (const SceneTypeMetadata *type =
              destination.registry().find(componentValue.type);
          type != nullptr) {
        if (componentValue.version > type->version) {
          throw std::runtime_error(
              "Authoring component version is newer than the registry");
        }
        if (componentValue.version < type->version) {
          componentValue.payload =
              type->hooks.migrate(componentValue.version, payload);
          componentValue.version = type->version;
        } else {
          componentValue.payload.assign(payload.begin(), payload.end());
        }
        type->hooks.validate(componentValue.payload);
      } else {
        componentValue.payload.assign(payload.begin(), payload.end());
      }
      entity.components.push_back(std::move(componentValue));
    }
    entities.push_back(std::move(entity));
  }
  if (!reader.done())
    throw std::runtime_error("Authoring document has trailing data");
  destination.replaceTransactional(std::move(entities));
  destination.markSaved();
}

void saveAuthoringDocument(AuthoringDocument &document,
                           const std::filesystem::path &path) {
  writeBinaryFileAtomic(path, encodeAuthoringDocument(document));
  document.markSaved();
}

void loadAuthoringDocument(AuthoringDocument &destination,
                           const std::filesystem::path &path) {
  decodeAuthoringDocument(
      destination,
      readBinaryFile(path, destination.limits().maximumDocumentBytes));
}

AuthoringDocument importAuthoringScene(const ImportedGltfScene &scene,
                                       const SceneTypeRegistry &registry,
                                       AuthoringLimits limits) {
  if (!scene.sceneId.valid())
    throw std::invalid_argument("Imported scene identity is invalid");
  AuthoringDocument document{registry, limits};
  std::vector<AuthoringEntity> entities;
  std::vector<SceneEntityId> nodeIds;
  nodeIds.reserve(scene.nodes.size());
  for (std::size_t index = 0U; index < scene.nodes.size(); ++index) {
    nodeIds.push_back(
        importedId(scene.sceneId, "node/" + std::to_string(index)));
  }
  for (std::size_t nodeIndex = 0U; nodeIndex < scene.nodes.size();
       ++nodeIndex) {
    const ImportedSceneNode &node = scene.nodes[nodeIndex];
    AuthoringEntity entity;
    entity.id = nodeIds[nodeIndex];
    entity.name =
        node.name.empty() ? "Node " + std::to_string(nodeIndex) : node.name;
    if (node.parent != std::numeric_limits<std::uint32_t>::max()) {
      if (node.parent >= nodeIds.size())
        throw std::runtime_error("Imported scene parent is invalid");
      entity.parent = nodeIds[node.parent];
    }
    entity.components.push_back(
        {kTransformSceneType, 2U,
         encodeAuthoringTransform(decomposeTransform(node.localTransform))});
    entities.push_back(std::move(entity));
    for (std::size_t primitiveIndex = 0U;
         primitiveIndex < node.meshPrimitives.size(); ++primitiveIndex) {
      const AssetId meshId = node.meshPrimitives[primitiveIndex];
      const auto mesh =
          std::ranges::find(scene.meshes, meshId, &ImportedMeshPrimitive::id);
      if (mesh == scene.meshes.end() || !mesh->material.valid()) {
        throw std::runtime_error(
            "Imported node references an unknown mesh or material");
      }
      AuthoringEntity *owner = &entities.back();
      if (primitiveIndex != 0U) {
        AuthoringEntity primitive;
        primitive.id = importedId(
            scene.sceneId, "node/" + std::to_string(nodeIndex) + "/primitive/" +
                               std::to_string(primitiveIndex));
        primitive.name = mesh->name.empty()
                             ? "Primitive " + std::to_string(primitiveIndex)
                             : mesh->name;
        primitive.parent = nodeIds[nodeIndex];
        primitive.components.push_back(
            {kTransformSceneType, 2U, encodeAuthoringTransform({})});
        entities.push_back(std::move(primitive));
        owner = &entities.back();
      }
      owner->components.push_back(
          {kRenderableSceneType, 1U,
           encodeAuthoringRenderable({meshId, mesh->material, true})});
    }
  }
  document.replaceTransactional(std::move(entities));
  document.markSaved();
  return document;
}

} // namespace ve::editor
