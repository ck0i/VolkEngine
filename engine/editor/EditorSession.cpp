#include "editor/EditorSession.hpp"

#include "editor/AuthoringCooker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ve::editor {
namespace {

Vec3 transformPoint(const Mat4 &matrix, const Vec3 point) {
  return {matrix.m[0] * point.x + matrix.m[4] * point.y +
              matrix.m[8] * point.z + matrix.m[12],
          matrix.m[1] * point.x + matrix.m[5] * point.y +
              matrix.m[9] * point.z + matrix.m[13],
          matrix.m[2] * point.x + matrix.m[6] * point.y +
              matrix.m[10] * point.z + matrix.m[14]};
}

float maximumScale(const Mat4 &matrix) {
  return std::max({length({matrix.m[0], matrix.m[1], matrix.m[2]}),
                   length({matrix.m[4], matrix.m[5], matrix.m[6]}),
                   length({matrix.m[8], matrix.m[9], matrix.m[10]})});
}

float snapped(const float value, const float step) {
  return step > 0.0F ? std::round(value / step) * step : value;
}

} // namespace

EditorSession::EditorSession(const SceneTypeRegistry &registry,
                             AuthoringLimits limits, EditorAssetView assets)
    : registry_(&registry), limits_(limits), assets_(assets),
      document_(registry, limits) {}

void EditorSession::importScene(const ImportedGltfScene &scene) {
  if (gesture_)
    cancelTranslateGesture();
  document_ = importAuthoringScene(scene, *registry_, limits_);
  authoringPath_.clear();
  cookedPath_.clear();
}

void EditorSession::newDocument() {
  if (gesture_)
    cancelTranslateGesture();
  document_ = AuthoringDocument{*registry_, limits_};
  authoringPath_.clear();
  cookedPath_.clear();
}

void EditorSession::save(const std::filesystem::path &path) {
  if (gesture_)
    throw std::logic_error("Cannot save during a transform gesture");
  saveAuthoringDocument(document_, path);
  authoringPath_ = path;
}

void EditorSession::load(const std::filesystem::path &path) {
  if (gesture_)
    cancelTranslateGesture();
  AuthoringDocument candidate{*registry_, limits_};
  loadAuthoringDocument(candidate, path);
  document_ = std::move(candidate);
  authoringPath_ = path;
  cookedPath_.clear();
}

void EditorSession::cook(const std::filesystem::path &path) {
  if (gesture_)
    throw std::logic_error("Cannot cook during a transform gesture");
  cookAuthoringDocumentToFile(document_, path);
  cookedPath_ = path;
}

Mat4 EditorSession::worldMatrix(const SceneEntityId entity) const {
  const AuthoringEntity *current = document_.find(entity);
  if (current == nullptr)
    throw std::invalid_argument("Cannot resolve an unknown editor entity");
  Mat4 matrix = Mat4::identity();
  std::size_t depth = 0U;
  while (current != nullptr) {
    if (++depth > document_.entities().size()) {
      throw std::runtime_error("Editor hierarchy contains a cycle");
    }
    TransformTRS local{};
    if (const AuthoringComponent *component =
            document_.component(current->id, kTransformSceneType);
        component != nullptr) {
      local = decodeAuthoringTransform(component->payload);
    }
    matrix = compose(local) * matrix;
    current =
        current->parent.valid() ? document_.find(current->parent) : nullptr;
  }
  return matrix;
}

std::optional<SceneEntityId>
EditorSession::pick(const Camera &camera, const float viewportX,
                    const float viewportY, const float viewportWidth,
                    const float viewportHeight) const {
  if (!std::isfinite(viewportX) || !std::isfinite(viewportY) ||
      !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) ||
      viewportWidth <= 0.0F || viewportHeight <= 0.0F) {
    throw std::invalid_argument("Editor viewport pick coordinates are invalid");
  }
  const float ndcX = viewportX / viewportWidth * 2.0F - 1.0F;
  const float ndcY = 1.0F - viewportY / viewportHeight * 2.0F;
  const float tangent = std::tan(camera.verticalFov() * 0.5F);
  const Vec3 forward = camera.forward();
  const Vec3 right = camera.right();
  const Vec3 up = normalize(cross(right, forward));
  const Vec3 direction =
      normalize(forward + right * (ndcX * tangent * camera.aspect()) +
                up * (ndcY * tangent));
  const Vec3 origin = camera.position();
  float nearest = std::numeric_limits<float>::max();
  std::optional<SceneEntityId> selected;
  for (const AuthoringEntity &entity : document_.entities()) {
    const AuthoringComponent *renderableComponent =
        document_.component(entity.id, kRenderableSceneType);
    if (renderableComponent == nullptr)
      continue;
    const AuthoringRenderable renderable =
        decodeAuthoringRenderable(renderableComponent->payload);
    if (!renderable.visible)
      continue;
    MeshBounds bounds{{}, 0.5F, true};
    if (assets_.bounds != nullptr) {
      bounds = assets_.bounds(assets_.context, renderable.mesh);
    }
    if (!bounds.valid || !finite(bounds.center) ||
        !std::isfinite(bounds.radius) || bounds.radius < 0.0F) {
      continue;
    }
    const Mat4 model = worldMatrix(entity.id);
    const Vec3 center = transformPoint(model, bounds.center);
    const float radius = bounds.radius * maximumScale(model);
    const Vec3 offset = origin - center;
    const float b = dot(offset, direction);
    const float c = dot(offset, offset) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0F)
      continue;
    const float root = std::sqrt(discriminant);
    const float distance = -b - root >= 0.0F ? -b - root : -b + root;
    if (distance >= 0.0F && distance < nearest) {
      nearest = distance;
      selected = entity.id;
    }
  }
  return selected;
}

bool EditorSession::pickAndSelect(const Camera &camera, const float viewportX,
                                  const float viewportY,
                                  const float viewportWidth,
                                  const float viewportHeight,
                                  const bool additive) {
  const std::optional<SceneEntityId> selected =
      pick(camera, viewportX, viewportY, viewportWidth, viewportHeight);
  if (!selected) {
    if (!additive)
      document_.clearSelection();
    return false;
  }
  document_.select(*selected, additive);
  return true;
}

void EditorSession::beginTranslateGesture() {
  if (gesture_)
    throw std::logic_error("A transform gesture is already active");
  if (document_.selection().empty()) {
    throw std::invalid_argument("Transform gesture selection is empty");
  }
  DocumentCommand original;
  original.label = "Translate selection";
  for (const SceneEntityId id : document_.selection()) {
    const AuthoringEntity *entity = document_.find(id);
    if (entity == nullptr ||
        document_.component(id, kTransformSceneType) == nullptr) {
      throw std::invalid_argument("Transform gesture entity has no transform");
    }
    original.patches.push_back({id, *entity, *entity});
  }
  gesture_ = TranslateGesture{original, original};
}

DocumentCommand EditorSession::translatedCommand(const Vec3 delta,
                                                 const float snap) const {
  if (!gesture_)
    throw std::logic_error("No transform gesture is active");
  if (!finite(delta) || !std::isfinite(snap) || snap < 0.0F) {
    throw std::invalid_argument("Transform gesture delta or snap is invalid");
  }
  const Vec3 snappedDelta{snapped(delta.x, snap), snapped(delta.y, snap),
                          snapped(delta.z, snap)};
  DocumentCommand command = gesture_->original;
  for (EntityPatch &patch : command.patches) {
    AuthoringEntity after = *patch.before;
    const auto component = std::ranges::find(
        after.components, kTransformSceneType, &AuthoringComponent::type);
    TransformTRS transform = decodeAuthoringTransform(component->payload);
    transform.translation = transform.translation + snappedDelta;
    component->payload = encodeAuthoringTransform(transform);
    patch.after = std::move(after);
  }
  return command;
}

void EditorSession::restoreGestureCurrent() {
  if (!gesture_)
    return;
  DocumentCommand restore;
  restore.label = "Restore transform preview";
  restore.patches.reserve(gesture_->current.patches.size());
  for (std::size_t index = 0U; index < gesture_->current.patches.size();
       ++index) {
    restore.patches.push_back({gesture_->current.patches[index].id,
                               gesture_->current.patches[index].after,
                               gesture_->original.patches[index].before});
  }
  document_.applyPreview(restore);
}

void EditorSession::previewTranslation(const Vec3 delta, const float snap) {
  if (!gesture_)
    throw std::logic_error("No transform gesture is active");
  restoreGestureCurrent();
  gesture_->current = translatedCommand(delta, snap);
  document_.applyPreview(gesture_->current);
}

void EditorSession::commitTranslateGesture() {
  if (!gesture_)
    throw std::logic_error("No transform gesture is active");
  const DocumentCommand command = gesture_->current;
  const bool changed =
      std::ranges::any_of(command.patches, [](const EntityPatch &patch) {
        return patch.before != patch.after;
      });
  restoreGestureCurrent();
  gesture_.reset();
  if (changed)
    document_.execute(command);
}

void EditorSession::cancelTranslateGesture() {
  if (!gesture_)
    throw std::logic_error("No transform gesture is active");
  restoreGestureCurrent();
  gesture_.reset();
}

} // namespace ve::editor
