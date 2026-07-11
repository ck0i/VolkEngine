#pragma once

#include "core/Camera.hpp"
#include "editor/AuthoringDocument.hpp"

#include <filesystem>
#include <optional>

namespace ve::editor {

struct EditorAssetView {
  void *context = nullptr;
  MeshBounds (*bounds)(void *context, AssetId mesh) = nullptr;
};

class EditorSession final {
public:
  explicit EditorSession(
      const SceneTypeRegistry &registry = builtinSceneTypeRegistry(),
      AuthoringLimits limits = {}, EditorAssetView assets = {});

  [[nodiscard]] AuthoringDocument &document() noexcept { return document_; }
  [[nodiscard]] const AuthoringDocument &document() const noexcept {
    return document_;
  }
  [[nodiscard]] const std::filesystem::path &authoringPath() const noexcept {
    return authoringPath_;
  }
  [[nodiscard]] const std::filesystem::path &cookedPath() const noexcept {
    return cookedPath_;
  }

  void importScene(const ImportedGltfScene &scene);
  void newDocument();
  void save(const std::filesystem::path &path);
  void load(const std::filesystem::path &path);
  void cook(const std::filesystem::path &path);

  [[nodiscard]] std::optional<SceneEntityId>
  pick(const Camera &camera, float viewportX, float viewportY,
       float viewportWidth, float viewportHeight) const;
  [[nodiscard]] Mat4 entityWorldMatrix(SceneEntityId entity) const {
    return worldMatrix(entity);
  }
  bool pickAndSelect(const Camera &camera, float viewportX, float viewportY,
                     float viewportWidth, float viewportHeight, bool additive);

  void beginTranslateGesture();
  void previewTranslation(Vec3 delta, float snap);
  void commitTranslateGesture();
  void cancelTranslateGesture();
  [[nodiscard]] bool translateGestureActive() const noexcept {
    return gesture_.has_value();
  }

private:
  struct TranslateGesture {
    DocumentCommand original;
    DocumentCommand current;
  };

  [[nodiscard]] DocumentCommand translatedCommand(Vec3 delta, float snap) const;
  void restoreGestureCurrent();
  [[nodiscard]] Mat4 worldMatrix(SceneEntityId entity) const;

  const SceneTypeRegistry *registry_ = nullptr;
  AuthoringLimits limits_;
  EditorAssetView assets_;
  AuthoringDocument document_;
  std::filesystem::path authoringPath_;
  std::filesystem::path cookedPath_;
  std::optional<TranslateGesture> gesture_;
};

} // namespace ve::editor
