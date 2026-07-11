#include "editor/AuthoringCooker.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ve::editor {

CookedWorld cookAuthoringDocument(const AuthoringDocument &document,
                                  const CookedWorldLimits &limits) {
  std::vector<const AuthoringEntity *> ordered;
  ordered.reserve(document.entities().size());
  for (const AuthoringEntity &entity : document.entities())
    ordered.push_back(&entity);
  std::ranges::sort(ordered, {},
                    [](const AuthoringEntity *entity) { return entity->id; });

  CookedWorld cooked;
  const std::size_t count = ordered.size();
  cooked.identities.reserve(count);
  cooked.names.reserve(count);
  cooked.parentIndices.reserve(count);
  cooked.transforms.reserve(count);
  cooked.renderableMask.reserve(count);
  cooked.renderables.reserve(count);
  for (const AuthoringEntity *entity : ordered) {
    cooked.identities.push_back(entity->id);
    cooked.names.push_back(entity->name);
    std::uint32_t parent = std::numeric_limits<std::uint32_t>::max();
    if (entity->parent.valid()) {
      const auto found = std::ranges::find(
          ordered, entity->parent,
          [](const AuthoringEntity *value) { return value->id; });
      if (found == ordered.end())
        throw std::runtime_error("Authoring parent disappeared during cook");
      parent = static_cast<std::uint32_t>(found - ordered.begin());
    }
    cooked.parentIndices.push_back(parent);
    TransformTRS transform{};
    AuthoringRenderable renderable{};
    bool hasRenderable = false;
    for (const AuthoringComponent &component : entity->components) {
      if (component.type == kTransformSceneType) {
        transform = decodeAuthoringTransform(component.payload);
      } else if (component.type == kRenderableSceneType) {
        renderable = decodeAuthoringRenderable(component.payload);
        hasRenderable = true;
      } else {
        throw std::runtime_error(
            "Cannot cook an unknown authoring component type " +
            std::to_string(component.type));
      }
    }
    cooked.transforms.push_back(transform);
    cooked.renderableMask.push_back(hasRenderable ? 1U : 0U);
    cooked.renderables.push_back(renderable);
  }
  validateCookedWorld(cooked, limits);
  return cooked;
}

void cookAuthoringDocumentToFile(const AuthoringDocument &document,
                                 const std::filesystem::path &path,
                                 const CookedWorldLimits &limits) {
  saveCookedWorld(cookAuthoringDocument(document, limits), path, limits);
}

} // namespace ve::editor
