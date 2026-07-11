#include "editor/EditorShell.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>

namespace ve::editor {
namespace {

constexpr const char *kEntityPayload = "VOLKENGINE_SCENE_ENTITY";

ImVec2 projectEntity(const Mat4 &viewProjection, const Mat4 &model,
                     const ImVec2 origin, const ImVec2 size) {
  const Vec4 world{model.m[12], model.m[13], model.m[14], 1.0F};
  Vec4 clip;
  clip.x = viewProjection.m[0] * world.x + viewProjection.m[4] * world.y +
           viewProjection.m[8] * world.z + viewProjection.m[12];
  clip.y = viewProjection.m[1] * world.x + viewProjection.m[5] * world.y +
           viewProjection.m[9] * world.z + viewProjection.m[13];
  clip.w = viewProjection.m[3] * world.x + viewProjection.m[7] * world.y +
           viewProjection.m[11] * world.z + viewProjection.m[15];
  if (clip.w <= 0.0001F)
    return {-10000.0F, -10000.0F};
  return {origin.x + (clip.x / clip.w * 0.5F + 0.5F) * size.x,
          origin.y + (-clip.y / clip.w * 0.5F + 0.5F) * size.y};
}

bool nearHandle(const ImVec2 mouse, const ImVec2 handle) {
  const float x = mouse.x - handle.x;
  const float y = mouse.y - handle.y;
  return x * x + y * y <= 100.0F;
}

} // namespace

EditorShell::EditorShell(EditorSession &session,
                         const ImportedGltfScene &importSource,
                         EditorShellActions actions)
    : session_(&session), importSource_(&importSource), actions_(actions) {
  std::snprintf(authoringPath_.data(), authoringPath_.size(),
                "editor_scene.veauthor");
  std::snprintf(cookedPath_.data(), cookedPath_.size(),
                "editor_scene.vecooked");
}

void EditorShell::setStatus(std::string message, const bool error) {
  status_ = std::move(message);
  statusIsError_ = error;
}

void EditorShell::save() {
  try {
    session_->save(authoringPath_.data());
    setStatus("Saved authoring document");
  } catch (const std::exception &error) {
    setStatus(error.what(), true);
  }
}

void EditorShell::load() {
  try {
    session_->load(authoringPath_.data());
    setStatus("Loaded authoring document");
  } catch (const std::exception &error) {
    setStatus(error.what(), true);
  }
}

void EditorShell::cook() {
  try {
    session_->cook(cookedPath_.data());
    if (actions_.runtimeReloadRequested != nullptr) {
      actions_.runtimeReloadRequested(actions_.context, session_->cookedPath());
    }
    setStatus("Cooked runtime world");
  } catch (const std::exception &error) {
    setStatus(error.what(), true);
  }
}

void EditorShell::drawMenu() {
  if (!ImGui::BeginMainMenuBar())
    return;
  if (ImGui::BeginMenu("Scene")) {
    if (ImGui::MenuItem("Import reference scene")) {
      try {
        session_->importScene(*importSource_);
        setStatus("Imported reference scene");
      } catch (const std::exception &error) {
        setStatus(error.what(), true);
      }
    }
    if (ImGui::MenuItem("New"))
      session_->newDocument();
    ImGui::Separator();
    if (ImGui::MenuItem("Save", "Ctrl+S"))
      save();
    if (ImGui::MenuItem("Load"))
      load();
    if (ImGui::MenuItem("Cook runtime world"))
      cook();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false,
                        session_->document().canUndo())) {
      try {
        session_->document().undo();
      } catch (const std::exception &error) {
        setStatus(error.what(), true);
      }
    }
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false,
                        session_->document().canRedo())) {
      try {
        session_->document().redo();
      } catch (const std::exception &error) {
        setStatus(error.what(), true);
      }
    }
    ImGui::EndMenu();
  }
  ImGui::TextUnformatted(session_->document().dirty() ? "Modified" : "Saved");
  if (!status_.empty()) {
    ImGui::SameLine();
    if (statusIsError_)
      ImGui::PushStyleColor(ImGuiCol_Text, {1.0F, 0.35F, 0.3F, 1.0F});
    ImGui::TextUnformatted(status_.c_str());
    if (statusIsError_)
      ImGui::PopStyleColor();
  }
  ImGui::EndMainMenuBar();
}

void EditorShell::drawHierarchyNode(const AuthoringEntity &entity) {
  const bool selected =
      std::ranges::find(session_->document().selection(), entity.id) !=
      session_->document().selection().end();
  const bool hasChildren = std::ranges::any_of(
      session_->document().entities(), [&](const AuthoringEntity &candidate) {
        return candidate.parent == entity.id;
      });
  ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
  if (!hasChildren)
    flags |= ImGuiTreeNodeFlags_Leaf;
  if (selected)
    flags |= ImGuiTreeNodeFlags_Selected;
  ImGui::PushID(static_cast<int>(entity.id.low & 0x7fffffffU));
  const bool open = ImGui::TreeNodeEx(entity.name.c_str(), flags);
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    try {
      session_->document().select(entity.id, ImGui::GetIO().KeyCtrl);
    } catch (const std::exception &error) {
      setStatus(error.what(), true);
    }
  }
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload(kEntityPayload, &entity.id, sizeof(entity.id));
    ImGui::Text("Reparent %s", entity.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload(kEntityPayload);
        payload != nullptr && payload->DataSize == sizeof(SceneEntityId)) {
      SceneEntityId child;
      std::memcpy(&child, payload->Data, sizeof(child));
      try {
        session_->document().reparent(child, entity.id);
      } catch (const std::exception &error) {
        setStatus(error.what(), true);
      }
    }
    ImGui::EndDragDropTarget();
  }
  if (open) {
    for (const AuthoringEntity &child : session_->document().entities()) {
      if (child.parent == entity.id)
        drawHierarchyNode(child);
    }
    ImGui::TreePop();
  }
  ImGui::PopID();
}

void EditorShell::drawHierarchy() {
  ImGui::SetNextWindowPos({0.0F, 19.0F}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize({300.0F, 350.0F}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Scene hierarchy")) {
    ImGui::End();
    return;
  }
  ImGui::InputText("Authoring", authoringPath_.data(), authoringPath_.size());
  ImGui::InputText("Cooked", cookedPath_.data(), cookedPath_.size());
  if (ImGui::Button("Create")) {
    try {
      const SceneEntityId parent =
          session_->document().selection().empty()
              ? SceneEntityId{}
              : session_->document().selection().front();
      const SceneEntityId created =
          session_->document().create("Entity", parent);
      session_->document().select(created);
    } catch (const std::exception &error) {
      setStatus(error.what(), true);
    }
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(session_->document().selection().empty());
  if (ImGui::Button("Delete")) {
    try {
      session_->document().erase(session_->document().selection().front());
    } catch (const std::exception &error) {
      setStatus(error.what(), true);
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Root") && !session_->document().selection().empty()) {
    try {
      session_->document().reparent(session_->document().selection().front(),
                                    {});
    } catch (const std::exception &error) {
      setStatus(error.what(), true);
    }
  }
  ImGui::Separator();
  for (const AuthoringEntity &entity : session_->document().entities()) {
    if (!entity.parent.valid())
      drawHierarchyNode(entity);
  }
  ImGui::End();
}

void EditorShell::drawInspector() {
  ImGui::SetNextWindowPos({0.0F, 369.0F}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize({300.0F, 351.0F}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Reflected inspector")) {
    ImGui::End();
    return;
  }
  if (session_->document().selection().empty()) {
    ImGui::TextUnformatted("No selection");
    ImGui::End();
    return;
  }
  const SceneEntityId primary = session_->document().selection().front();
  const AuthoringEntity *entity = session_->document().find(primary);
  std::array<char, 256> name{};
  std::snprintf(name.data(), name.size(), "%s", entity->name.c_str());
  if (ImGui::InputText("Name", name.data(), name.size(),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    try {
      session_->document().rename(primary, name.data());
    } catch (const std::exception &error) {
      setStatus(error.what(), true);
    }
  }
  ImGui::Text("Selection: %zu", session_->document().selection().size());
  for (const AuthoringComponent &component : entity->components) {
    const SceneTypeMetadata *type =
        session_->document().registry().find(component.type);
    if (type == nullptr) {
      ImGui::TextColored({1.0F, 0.7F, 0.2F, 1.0F},
                         "Unknown component %llu v%u (%zu bytes)",
                         static_cast<unsigned long long>(component.type),
                         component.version, component.payload.size());
      continue;
    }
    if (!ImGui::CollapsingHeader(type->displayName.c_str(),
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
      continue;
    }
    std::vector<SceneEntityId> eligible;
    for (const SceneEntityId selected : session_->document().selection()) {
      if (session_->document().component(selected, type->id) != nullptr) {
        eligible.push_back(selected);
      }
    }
    for (const ScenePropertyMetadata &property : type->properties) {
      ImGui::PushID(static_cast<int>(property.id & 0x7fffffffU));
      ScenePropertyValue value =
          type->hooks.read(component.payload, property.id);
      bool changed = false;
      switch (property.kind) {
      case ScenePropertyKind::Bool: {
        bool current = std::get<bool>(value);
        changed = ImGui::Checkbox(property.displayName.c_str(), &current);
        if (changed)
          value = current;
        break;
      }
      case ScenePropertyKind::Float: {
        float current = std::get<float>(value);
        changed =
            ImGui::DragFloat(property.displayName.c_str(), &current,
                             static_cast<float>(property.inspector.step),
                             static_cast<float>(property.inspector.minimum),
                             static_cast<float>(property.inspector.maximum));
        if (changed)
          value = current;
        break;
      }
      case ScenePropertyKind::Vec3: {
        Vec3 current = std::get<Vec3>(value);
        float values[3]{current.x, current.y, current.z};
        changed =
            ImGui::DragFloat3(property.displayName.c_str(), values,
                              static_cast<float>(property.inspector.step));
        if (changed)
          value = Vec3{values[0], values[1], values[2]};
        break;
      }
      case ScenePropertyKind::Quaternion: {
        Quat current = std::get<Quat>(value);
        float values[4]{current.x, current.y, current.z, current.w};
        changed = ImGui::DragFloat4(property.displayName.c_str(), values,
                                    static_cast<float>(property.inspector.step),
                                    -1.0F, 1.0F);
        if (changed)
          value = normalizeQuat({values[0], values[1], values[2], values[3]});
        break;
      }
      case ScenePropertyKind::AssetId: {
        AssetId current = std::get<AssetId>(value);
        std::uint64_t values[2]{current.high, current.low};
        changed = ImGui::InputScalarN(property.displayName.c_str(),
                                      ImGuiDataType_U64, values, 2);
        if (changed)
          value = AssetId{values[0], values[1]};
        break;
      }
      case ScenePropertyKind::String: {
        std::array<char, 256> text{};
        std::snprintf(text.data(), text.size(), "%s",
                      std::get<std::string>(value).c_str());
        changed = ImGui::InputText(property.displayName.c_str(), text.data(),
                                   text.size());
        if (changed)
          value = std::string{text.data()};
        break;
      }
      }
      if (changed) {
        try {
          session_->document().setProperty(eligible, type->id, property.id,
                                           value,
                                           "Edit " + property.displayName);
        } catch (const std::exception &error) {
          setStatus(error.what(), true);
        }
      }
      ImGui::PopID();
    }
  }
  ImGui::End();
}

void EditorShell::drawViewport(const RendererOverlayFrame &frame) {
  ImGui::SetNextWindowPos({300.0F, 19.0F}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(
      {std::max(static_cast<float>(frame.width) - 600.0F, 320.0F),
       std::max(static_cast<float>(frame.height) - 219.0F, 300.0F)},
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.18F);
  if (!ImGui::Begin("Viewport")) {
    ImGui::End();
    return;
  }
  ImGui::DragFloat("Translation snap", &translationSnap_, 0.01F, 0.0F, 100.0F,
                   "%.2f");
  ImVec2 available = ImGui::GetContentRegionAvail();
  available.x = std::max(available.x, 1.0F);
  available.y = std::max(available.y, 1.0F);
  ImGui::InvisibleButton("viewport-canvas", available,
                         ImGuiButtonFlags_MouseButtonLeft);
  const ImVec2 minimum = ImGui::GetItemRectMin();
  const ImVec2 maximum = ImGui::GetItemRectMax();
  const ImVec2 mouse = ImGui::GetMousePos();
  bool gizmoHit = false;
  const bool canvasHovered = ImGui::IsItemHovered();
  if (!session_->document().selection().empty()) {
    const SceneEntityId selected = session_->document().selection().front();
    const ImVec2 center = projectEntity(
        frame.camera.viewProjectionMatrix(),
        session_->entityWorldMatrix(selected), {},
        {static_cast<float>(frame.width), static_cast<float>(frame.height)});
    constexpr float axisLength = 54.0F;
    const std::array endpoints{
        ImVec2{center.x + axisLength, center.y},
        ImVec2{center.x, center.y - axisLength},
        ImVec2{center.x + axisLength * 0.7F, center.y + axisLength * 0.7F}};
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const std::array colors{IM_COL32(235, 70, 70, 255),
                            IM_COL32(80, 220, 100, 255),
                            IM_COL32(80, 130, 245, 255)};
    for (std::size_t axis = 0U; axis < endpoints.size(); ++axis) {
      draw->AddLine(center, endpoints[axis], colors[axis], 3.0F);
      draw->AddCircleFilled(endpoints[axis], 6.0F, colors[axis]);
      if (canvasHovered && nearHandle(mouse, endpoints[axis])) {
        gizmoHit = true;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !session_->translateGestureActive()) {
          try {
            session_->beginTranslateGesture();
            gizmoAxis_ = static_cast<int>(axis);
          } catch (const std::exception &error) {
            setStatus(error.what(), true);
          }
        }
      }
    }
    if (session_->translateGestureActive()) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        Vec3 delta{};
        if (gizmoAxis_ == 0)
          delta.x = drag.x * 0.01F;
        if (gizmoAxis_ == 1)
          delta.y = -drag.y * 0.01F;
        if (gizmoAxis_ == 2)
          delta.z = (drag.x + drag.y) * 0.007F;
        try {
          session_->previewTranslation(delta, translationSnap_);
        } catch (const std::exception &error) {
          setStatus(error.what(), true);
        }
      } else {
        try {
          session_->commitTranslateGesture();
        } catch (const std::exception &error) {
          setStatus(error.what(), true);
        }
        gizmoAxis_ = -1;
      }
    }
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !gizmoHit &&
      !session_->translateGestureActive()) {
    try {
      session_->pickAndSelect(
          frame.camera, mouse.x, mouse.y, static_cast<float>(frame.width),
          static_cast<float>(frame.height), ImGui::GetIO().KeyCtrl);
    } catch (const std::exception &error) {
      setStatus(error.what(), true);
    }
  }
  ImGui::GetWindowDrawList()->AddRect(minimum, maximum,
                                      IM_COL32(100, 110, 125, 180));
  ImGui::End();
}

void EditorShell::drawProfiling(const RendererOverlayFrame &frame,
                                const JobSystemStats &jobs) {
  ImGui::SetNextWindowPos(
      {std::max(static_cast<float>(frame.width) - 300.0F, 0.0F), 19.0F},
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize({300.0F, 350.0F}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Engine profiling")) {
    ImGui::End();
    return;
  }
  ImGui::Text("CPU render-submit %.3f ms", frame.stats.cpuFrameMs);
  if (frame.stats.gpuTimestampsValid) {
    ImGui::Text("GPU %.3f ms", frame.stats.gpuFrameMs);
  } else {
    ImGui::TextUnformatted("GPU pending/unavailable");
  }
  ImGui::Text("Graph %u passes / %u barriers / %u allocations",
              frame.stats.graphPassCount, frame.stats.graphBarrierCount,
              frame.stats.graphPhysicalAllocationCount);
  ImGui::Separator();
  ImGui::Text("Jobs %llu submitted / %u active / %u running",
              static_cast<unsigned long long>(jobs.submitted), jobs.activeJobs,
              jobs.runningJobs);
  ImGui::Text("Succeeded %llu failed %llu cancelled %llu steals %llu",
              static_cast<unsigned long long>(jobs.succeeded),
              static_cast<unsigned long long>(jobs.failed),
              static_cast<unsigned long long>(jobs.cancelled),
              static_cast<unsigned long long>(jobs.steals));
  ImGui::Text("Queue high-water %u, worker time %.3f ms",
              jobs.queueHighWatermark,
              static_cast<double>(jobs.executedNanoseconds) / 1.0e6);
  ImGui::End();
}

void EditorShell::draw(const RendererOverlayFrame &frame,
                       const JobSystemStats &jobs) {
  const ImGuiIO &io = ImGui::GetIO();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
    save();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) &&
      session_->document().canUndo()) {
    try {
      session_->document().undo();
    } catch (const std::exception &error) {
      setStatus(error.what(), true);
    }
  }
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) &&
      session_->document().canRedo()) {
    try {
      session_->document().redo();
    } catch (const std::exception &error) {
      setStatus(error.what(), true);
    }
  }
  drawMenu();
  drawHierarchy();
  drawInspector();
  drawViewport(frame);
  drawProfiling(frame, jobs);
}

} // namespace ve::editor
