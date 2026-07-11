#pragma once

#include "core/JobSystem.hpp"
#include "editor/EditorSession.hpp"
#include "renderer/Renderer.hpp"

#include <array>
#include <filesystem>
#include <string>

namespace ve::editor {

struct EditorShellActions {
  void *context = nullptr;
  void (*runtimeReloadRequested)(
      void *context, const std::filesystem::path &cookedPath) = nullptr;
};

class EditorShell final {
public:
  EditorShell(EditorSession &session, const ImportedGltfScene &importSource,
              EditorShellActions actions = {});

  void draw(const RendererOverlayFrame &frame, const JobSystemStats &jobs);

private:
  void drawMenu();
  void drawHierarchy();
  void drawHierarchyNode(const AuthoringEntity &entity);
  void drawInspector();
  void drawViewport(const RendererOverlayFrame &frame);
  void drawProfiling(const RendererOverlayFrame &frame,
                     const JobSystemStats &jobs);
  void setStatus(std::string message, bool error = false);
  void save();
  void load();
  void cook();

  EditorSession *session_ = nullptr;
  const ImportedGltfScene *importSource_ = nullptr;
  EditorShellActions actions_;
  std::array<char, 512> authoringPath_{};
  std::array<char, 512> cookedPath_{};
  std::string status_;
  bool statusIsError_ = false;
  float translationSnap_ = 0.25F;
  int gizmoAxis_ = -1;
};

} // namespace ve::editor
