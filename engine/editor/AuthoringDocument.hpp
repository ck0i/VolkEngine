#pragma once

#include "assets/GltfImporter.hpp"
#include "renderer/SceneRenderer.hpp"
#include "scene/SceneReflection.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ve::editor {

struct AuthoringLimits {
  std::size_t maximumEntities = 1'000'000U;
  std::size_t maximumComponentsPerEntity = 256U;
  std::size_t maximumComponentBytes = 16U * 1024U * 1024U;
  std::size_t maximumDocumentBytes = 256U * 1024U * 1024U;
  std::size_t maximumNameBytes = 255U;
  std::size_t maximumHistory = 256U;
};

struct AuthoringComponent {
  SceneTypeId type = 0U;
  std::uint32_t version = 0U;
  std::vector<std::byte> payload;

  friend bool operator==(const AuthoringComponent &,
                         const AuthoringComponent &) = default;
};

struct AuthoringEntity {
  SceneEntityId id;
  std::string name;
  SceneEntityId parent;
  std::vector<AuthoringComponent> components;

  friend bool operator==(const AuthoringEntity &,
                         const AuthoringEntity &) = default;
};

struct EntityPatch {
  SceneEntityId id;
  std::optional<AuthoringEntity> before;
  std::optional<AuthoringEntity> after;
};

struct DocumentCommand {
  std::string label;
  std::vector<EntityPatch> patches;
};

class AuthoringDocument final {
public:
  explicit AuthoringDocument(
      const SceneTypeRegistry &registry = builtinSceneTypeRegistry(),
      AuthoringLimits limits = {});

  [[nodiscard]] const SceneTypeRegistry &registry() const noexcept {
    return *registry_;
  }
  [[nodiscard]] const AuthoringLimits &limits() const noexcept {
    return limits_;
  }
  [[nodiscard]] const std::vector<AuthoringEntity> &entities() const noexcept {
    return entities_;
  }
  [[nodiscard]] const std::vector<SceneEntityId> &selection() const noexcept {
    return selection_;
  }
  [[nodiscard]] const AuthoringEntity *find(SceneEntityId id) const noexcept;
  [[nodiscard]] AuthoringEntity *find(SceneEntityId id) noexcept;
  [[nodiscard]] const AuthoringComponent *
  component(SceneEntityId entity, SceneTypeId type) const noexcept;

  [[nodiscard]] SceneEntityId create(std::string name,
                                     SceneEntityId parent = {});
  void erase(SceneEntityId entity);
  void rename(SceneEntityId entity, std::string name);
  void reparent(SceneEntityId entity, SceneEntityId parent);
  void setComponent(SceneEntityId entity, AuthoringComponent component);
  void removeComponent(SceneEntityId entity, SceneTypeId type);
  void setProperty(std::span<const SceneEntityId> entities, SceneTypeId type,
                   ScenePropertyId property, const ScenePropertyValue &value,
                   std::string label = "Edit property");

  void execute(DocumentCommand command);
  void applyPreview(const DocumentCommand &command);
  [[nodiscard]] bool canUndo() const noexcept { return historyCursor_ != 0U; }
  [[nodiscard]] bool canRedo() const noexcept {
    return historyCursor_ < history_.size();
  }
  void undo();
  void redo();
  void clearHistory() noexcept;

  void select(SceneEntityId entity, bool additive = false);
  void clearSelection() noexcept { selection_.clear(); }
  [[nodiscard]] bool dirty() const;
  void markSaved();

  [[nodiscard]] DocumentCommand
  propertyCommand(std::span<const SceneEntityId> entities, SceneTypeId type,
                  ScenePropertyId property, const ScenePropertyValue &value,
                  std::string label) const;
  void replaceTransactional(std::vector<AuthoringEntity> entities);

private:
  [[nodiscard]] SceneEntityId nextEntityId() const;
  [[nodiscard]] DocumentCommand entityReplacement(std::string label,
                                                  const AuthoringEntity &before,
                                                  AuthoringEntity after) const;
  void apply(const DocumentCommand &command, bool forward);
  void validate(std::span<const AuthoringEntity> entities) const;
  void filterSelection();
  [[nodiscard]] std::vector<std::byte> fingerprint() const;

  const SceneTypeRegistry *registry_ = nullptr;
  AuthoringLimits limits_;
  std::vector<AuthoringEntity> entities_;
  std::vector<SceneEntityId> selection_;
  std::vector<DocumentCommand> history_;
  std::size_t historyCursor_ = 0U;
  std::vector<std::byte> savedFingerprint_;
};

[[nodiscard]] std::vector<std::byte>
encodeAuthoringDocument(const AuthoringDocument &document);
void decodeAuthoringDocument(AuthoringDocument &destination,
                             std::span<const std::byte> bytes);
void saveAuthoringDocument(AuthoringDocument &document,
                           const std::filesystem::path &path);
void loadAuthoringDocument(AuthoringDocument &destination,
                           const std::filesystem::path &path);
[[nodiscard]] AuthoringDocument importAuthoringScene(
    const ImportedGltfScene &scene,
    const SceneTypeRegistry &registry = builtinSceneTypeRegistry(),
    AuthoringLimits limits = {});

} // namespace ve::editor
