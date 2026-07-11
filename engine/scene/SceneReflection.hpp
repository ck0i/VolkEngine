#pragma once

#include "assets/AssetDatabase.hpp"
#include "core/Math.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ve {

using SceneTypeId = std::uint64_t;
using ScenePropertyId = std::uint64_t;

[[nodiscard]] constexpr std::uint64_t
stableSceneId(const std::string_view value) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char character : value) {
    hash ^= static_cast<std::uint8_t>(character);
    hash *= 1099511628211ULL;
  }
  return hash;
}

enum class ScenePropertyKind : std::uint8_t {
  Bool,
  Float,
  Vec3,
  Quaternion,
  AssetId,
  String,
};

struct SceneInspectorMetadata {
  double minimum = 0.0;
  double maximum = 0.0;
  double step = 0.0;
  friend bool operator==(const SceneInspectorMetadata &,
                         const SceneInspectorMetadata &) = default;
};

struct ScenePropertyMetadata {
  ScenePropertyId id = 0;
  std::string displayName;
  std::string name;
  ScenePropertyKind kind = ScenePropertyKind::Float;
  SceneInspectorMetadata inspector;
  std::string futureBindingType;

  friend bool operator==(const ScenePropertyMetadata &,
                         const ScenePropertyMetadata &) = default;
};

using ScenePropertyValue =
    std::variant<bool, float, Vec3, Quat, AssetId, std::string>;
using ScenePayloadValidator = void (*)(std::span<const std::byte> payload);
using ScenePayloadMigrator = std::vector<std::byte> (*)(
    std::uint32_t sourceVersion, std::span<const std::byte> payload);
using ScenePropertyReader = ScenePropertyValue (*)(
    std::span<const std::byte> payload, ScenePropertyId property);
using ScenePropertyWriter = std::vector<std::byte> (*)(
    std::span<const std::byte> payload, ScenePropertyId property,
    const ScenePropertyValue &value);

struct SceneTypeHooks {
  ScenePayloadValidator validate = nullptr;
  ScenePayloadMigrator migrate = nullptr;
  ScenePropertyReader read = nullptr;
  ScenePropertyWriter write = nullptr;
};

struct SceneTypeMetadata {
  SceneTypeId id = 0;
  std::string displayName;
  std::string name;
  std::uint32_t version = 0;
  std::vector<ScenePropertyMetadata> properties;
  SceneTypeHooks hooks;
};

class SceneTypeRegistry final {
public:
  void registerType(SceneTypeMetadata metadata);
  void bindHooks(SceneTypeId type, SceneTypeHooks hooks);
  [[nodiscard]] const SceneTypeMetadata *find(SceneTypeId type) const noexcept;
  [[nodiscard]] const SceneTypeMetadata *
  find(std::string_view name) const noexcept;
  [[nodiscard]] const std::vector<SceneTypeMetadata> &types() const noexcept {
    return types_;
  }
  [[nodiscard]] std::string bindingManifest() const;

private:
  std::vector<SceneTypeMetadata> types_;
};

[[nodiscard]] ScenePropertyMetadata makeScenePropertyMetadata(
    std::string displayName, std::string name, ScenePropertyKind kind,
    double minimum, double maximum, double step, std::string futureBindingType);
[[nodiscard]] SceneTypeMetadata
makeSceneTypeMetadata(std::string displayName, std::string name,
                      std::uint32_t version,
                      std::vector<ScenePropertyMetadata> properties);

[[nodiscard]] SceneTypeRegistry explicitSceneTypeRegistry();
[[nodiscard]] SceneTypeRegistry
externalSceneTypeRegistry(std::string_view schema);
void bindBuiltinSceneTypeHooks(SceneTypeRegistry &registry);
[[nodiscard]] const SceneTypeRegistry &builtinSceneTypeRegistry();

inline constexpr SceneTypeId kTransformSceneType =
    stableSceneId("ve.scene.transform");
inline constexpr SceneTypeId kRenderableSceneType =
    stableSceneId("ve.scene.renderable");

struct AuthoringRenderable {
  AssetId mesh;
  AssetId material;
  bool visible = true;

  friend bool operator==(const AuthoringRenderable &,
                         const AuthoringRenderable &) = default;
};

[[nodiscard]] std::vector<std::byte>
encodeAuthoringTransform(const TransformTRS &transform);
[[nodiscard]] TransformTRS
decodeAuthoringTransform(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte>
encodeAuthoringRenderable(const AuthoringRenderable &renderable);
[[nodiscard]] AuthoringRenderable
decodeAuthoringRenderable(std::span<const std::byte> payload);

} // namespace ve
