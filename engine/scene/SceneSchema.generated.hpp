#pragma once

#include "scene/SceneReflection.hpp"

namespace ve {

inline SceneTypeRegistry generatedSceneTypeRegistry() {
  SceneTypeRegistry registry;
  registry.registerType(makeSceneTypeMetadata(
      "Transform", "ve.scene.transform", 2U, {
          makeScenePropertyMetadata(
              "Translation", "translation", ScenePropertyKind::Vec3,
              -100000, 100000, 0.1, "System.Numerics.Vector3"),
          makeScenePropertyMetadata(
              "Rotation", "rotation", ScenePropertyKind::Quaternion,
              -1, 1, 0.01, "System.Numerics.Quaternion"),
          makeScenePropertyMetadata(
              "Scale", "scale", ScenePropertyKind::Vec3,
              -10000, 10000, 0.01, "System.Numerics.Vector3"),
      }));
  registry.registerType(makeSceneTypeMetadata(
      "Renderable", "ve.scene.renderable", 1U, {
          makeScenePropertyMetadata(
              "Mesh", "mesh", ScenePropertyKind::AssetId,
              0, 0, 0, "VolkEngine.AssetId"),
          makeScenePropertyMetadata(
              "Material", "material", ScenePropertyKind::AssetId,
              0, 0, 0, "VolkEngine.AssetId"),
          makeScenePropertyMetadata(
              "Visible", "visible", ScenePropertyKind::Bool,
              0, 1, 1, "System.Boolean"),
      }));
  bindBuiltinSceneTypeHooks(registry);
  return registry;
}

} // namespace ve
