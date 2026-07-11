#pragma once

#include "core/Math.hpp"
#include "renderer/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ve {

enum class LandscapeBiome : std::uint8_t {
  Alpine,
  Meadow,
  Forest,
  Wetland,
};

enum class FoliageSpecies : std::uint8_t {
  Grass,
  Shrub,
  Tree,
};

enum class FoliageDensity : std::uint8_t {
  Low,
  Medium,
  High,
};

struct LandscapeConfig {
  std::uint64_t seed = 1U;
  float baseHeight = 0.0F;
  float relief = 180.0F;
  float featureSize = 2'048.0F;
  float waterLevel = -24.0F;
};

struct LandscapeBrush {
  Vec2 center{};
  float radius = 1.0F;
  float heightDelta = 0.0F;
  float falloffExponent = 2.0F;
};

struct LandscapeSample {
  float height = 0.0F;
  Vec3 normal{0.0F, 1.0F, 0.0F};
  float moisture = 0.0F;
  float temperature = 0.0F;
  LandscapeBiome biome = LandscapeBiome::Meadow;
};

class LandscapeField final {
public:
  static constexpr std::size_t kMaximumBrushes = 256U;
  static constexpr float kMaximumWorldCoordinate = 1'000'000'000.0F;

  explicit LandscapeField(LandscapeConfig config = {});

  void addBrush(LandscapeBrush brush);
  void clearBrushes() noexcept;

  [[nodiscard]] LandscapeSample sample(float worldX, float worldZ) const;
  [[nodiscard]] float height(float worldX, float worldZ) const;
  [[nodiscard]] const LandscapeConfig &config() const noexcept {
    return config_;
  }
  [[nodiscard]] std::span<const LandscapeBrush> brushes() const noexcept {
    return brushes_;
  }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
  [[nodiscard]] float baseHeight(float worldX, float worldZ) const;

  LandscapeConfig config_;
  std::vector<LandscapeBrush> brushes_;
  std::uint64_t revision_ = 1U;
};

struct TerrainPatchDesc {
  Vec2 center{};
  float halfExtent = 512.0F;
  std::uint32_t resolution = 32U;
  std::uint32_t lodLevel = 0U;
  float skirtDepth = 24.0F;
  float uvScale = 1.0F / 128.0F;
};

struct TerrainPatch {
  MeshData mesh;
  std::uint32_t topVertexCount = 0U;
  std::uint32_t skirtVertexCount = 0U;
  std::uint32_t topTriangleCount = 0U;
  std::uint32_t skirtTriangleCount = 0U;
};

[[nodiscard]] TerrainPatch cookTerrainPatch(const LandscapeField &field,
                                            TerrainPatchDesc desc);
[[nodiscard]] MeshData createWaterPatch(Vec2 center, float halfExtent,
                                        float height, float uvScale);

struct FoliageScatterDesc {
  static constexpr std::uint64_t kMaximumCandidates = 4'194'304U;
  static constexpr std::uint32_t kMaximumInstances = 1'000'000U;
  Vec2 center{};
  float halfExtent = 512.0F;
  float spacing = 32.0F;
  float jitter = 0.8F;
  float maximumSlopeDegrees = 38.0F;
  std::uint32_t maximumInstances = 16'384U;
  FoliageDensity density = FoliageDensity::Medium;
  std::uint64_t seed = 1U;
};

struct FoliageInstance {
  Vec3 position{};
  float yawRadians = 0.0F;
  float scale = 1.0F;
  FoliageSpecies species = FoliageSpecies::Grass;
};

[[nodiscard]] std::vector<FoliageInstance>
scatterFoliage(const LandscapeField &field, FoliageScatterDesc desc);

[[nodiscard]] MeshData createGrassClusterMesh();
[[nodiscard]] MeshData createShrubMesh();
[[nodiscard]] MeshData createTreeMesh();

[[nodiscard]] const char *landscapeBiomeName(LandscapeBiome biome) noexcept;

} // namespace ve
