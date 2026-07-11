#include "landscape/Landscape.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace ve {
namespace {

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
  value ^= value >> 30U;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27U;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

[[nodiscard]] std::uint64_t hashGrid(const std::uint64_t seed,
                                     const std::int64_t x, const std::int64_t z,
                                     const std::uint64_t stream = 0U) noexcept {
  return mix64(seed ^ mix64(static_cast<std::uint64_t>(x)) ^
               std::rotl(mix64(static_cast<std::uint64_t>(z)), 29) ^
               mix64(stream));
}

[[nodiscard]] float unitFloat(const std::uint64_t value) noexcept {
  return static_cast<float>(value >> 40U) * (1.0F / 16'777'216.0F);
}

[[nodiscard]] float smooth(const float value) noexcept {
  return value * value * value * (value * (value * 6.0F - 15.0F) + 10.0F);
}

[[nodiscard]] float valueNoise(const std::uint64_t seed, const float x,
                               const float z) noexcept {
  const auto ix = static_cast<std::int64_t>(std::floor(x));
  const auto iz = static_cast<std::int64_t>(std::floor(z));
  const float tx = smooth(x - static_cast<float>(ix));
  const float tz = smooth(z - static_cast<float>(iz));
  const auto value = [&](const std::int64_t dx, const std::int64_t dz) {
    return unitFloat(hashGrid(seed, ix + dx, iz + dz)) * 2.0F - 1.0F;
  };
  const float a = std::lerp(value(0, 0), value(1, 0), tx);
  const float b = std::lerp(value(0, 1), value(1, 1), tx);
  return std::lerp(a, b, tz);
}

[[nodiscard]] float fractalNoise(const std::uint64_t seed, float x,
                                 float z) noexcept {
  float value = 0.0F;
  float amplitude = 0.53333336F;
  for (std::uint64_t octave = 0U; octave < 4U; ++octave) {
    value += valueNoise(mix64(seed + octave), x, z) * amplitude;
    x = x * 2.031F + 17.0F;
    z = z * 2.017F - 11.0F;
    amplitude *= 0.5F;
  }
  return value;
}

void validateConfig(const LandscapeConfig &config) {
  if (config.seed == 0U || !std::isfinite(config.baseHeight) ||
      std::abs(config.baseHeight) > 1'000'000.0F ||
      !std::isfinite(config.relief) || config.relief < 0.0F ||
      config.relief > 1'000'000.0F || !std::isfinite(config.featureSize) ||
      config.featureSize < 32.0F || config.featureSize > 10'000'000.0F ||
      !std::isfinite(config.waterLevel) ||
      std::abs(config.waterLevel) > 1'000'000.0F) {
    throw std::invalid_argument("Landscape configuration is invalid");
  }
}

[[nodiscard]] bool finite(const Vec2 value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] bool supportedRange(const Vec2 center, const float extent,
                                  const float margin = 0.0F) noexcept {
  const double limit =
      static_cast<double>(LandscapeField::kMaximumWorldCoordinate) -
      static_cast<double>(margin) - static_cast<double>(extent);
  return finite(center) && std::isfinite(extent) && extent >= 0.0F &&
         std::abs(static_cast<double>(center.x)) <= limit &&
         std::abs(static_cast<double>(center.y)) <= limit;
}

[[nodiscard]] MeshBounds calculateBounds(const std::vector<Vertex> &vertices) {
  if (vertices.empty())
    throw std::invalid_argument("Generated mesh has no vertices");
  Vec3 minimum = vertices.front().position;
  Vec3 maximum = minimum;
  for (const Vertex &vertex : vertices) {
    if (!ve::finite(vertex.position))
      throw std::runtime_error("Generated mesh contains a non-finite vertex");
    minimum.x = std::min(minimum.x, vertex.position.x);
    minimum.y = std::min(minimum.y, vertex.position.y);
    minimum.z = std::min(minimum.z, vertex.position.z);
    maximum.x = std::max(maximum.x, vertex.position.x);
    maximum.y = std::max(maximum.y, vertex.position.y);
    maximum.z = std::max(maximum.z, vertex.position.z);
  }
  const Vec3 center = (minimum + maximum) * 0.5F;
  float radius = 0.0F;
  for (const Vertex &vertex : vertices)
    radius = std::max(radius, length(vertex.position - center));
  return {center, radius, true};
}

void appendQuad(MeshData &mesh, const std::array<Vec3, 4> positions,
                const Vec3 normal) {
  if (mesh.vertices.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) -
          4U) {
    throw std::overflow_error("Vegetation mesh vertex range exhausted");
  }
  const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
  const Vec3 tangent = normalize(positions[1] - positions[0]);
  const Vec4 packedTangent{tangent.x, tangent.y, tangent.z, 1.0F};
  mesh.vertices.push_back({positions[0], normal, {0.0F, 0.0F}, packedTangent});
  mesh.vertices.push_back({positions[1], normal, {1.0F, 0.0F}, packedTangent});
  mesh.vertices.push_back({positions[2], normal, {0.0F, 1.0F}, packedTangent});
  mesh.vertices.push_back({positions[3], normal, {1.0F, 1.0F}, packedTangent});
  mesh.indices.insert(mesh.indices.end(), {first, first + 2U, first + 1U,
                                           first + 1U, first + 2U, first + 3U});
}

void appendTriangle(MeshData &mesh, const Vec3 a, const Vec3 b, const Vec3 c,
                    const Vec3 normal) {
  if (mesh.vertices.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) -
          3U) {
    throw std::overflow_error("Vegetation mesh vertex range exhausted");
  }
  const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
  const Vec3 tangent = normalize(b - a);
  const Vec4 packedTangent{tangent.x, tangent.y, tangent.z, 1.0F};
  mesh.vertices.push_back({a, normal, {0.0F, 0.0F}, packedTangent});
  mesh.vertices.push_back({b, normal, {1.0F, 0.0F}, packedTangent});
  mesh.vertices.push_back({c, normal, {0.5F, 1.0F}, packedTangent});
  mesh.indices.insert(mesh.indices.end(), {first, first + 1U, first + 2U});
}

[[nodiscard]] float densityProbability(const FoliageDensity density) noexcept {
  switch (density) {
  case FoliageDensity::Low:
    return 0.22F;
  case FoliageDensity::Medium:
    return 0.46F;
  case FoliageDensity::High:
    return 0.78F;
  }
  return 0.0F;
}

} // namespace

LandscapeField::LandscapeField(const LandscapeConfig config) : config_(config) {
  validateConfig(config_);
  brushes_.reserve(kMaximumBrushes);
}

void LandscapeField::addBrush(const LandscapeBrush brush) {
  if (!finite(brush.center) || !std::isfinite(brush.radius) ||
      brush.radius <= 0.0F || !supportedRange(brush.center, brush.radius) ||
      !std::isfinite(brush.heightDelta) ||
      std::abs(brush.heightDelta) > 1'000'000.0F ||
      !std::isfinite(brush.falloffExponent) || brush.falloffExponent < 1.0F ||
      brush.falloffExponent > 16.0F) {
    throw std::invalid_argument("Landscape brush is invalid");
  }
  if (brushes_.size() == kMaximumBrushes)
    throw std::overflow_error("Landscape brush capacity exhausted");
  brushes_.push_back(brush);
  ++revision_;
}

void LandscapeField::clearBrushes() noexcept {
  if (brushes_.empty())
    return;
  brushes_.clear();
  ++revision_;
}

float LandscapeField::baseHeight(const float worldX, const float worldZ) const {
  const float scale = 1.0F / config_.featureSize;
  const float continental =
      fractalNoise(config_.seed, worldX * scale, worldZ * scale);
  const float ridgeSource =
      fractalNoise(config_.seed ^ 0xa5a5a5a55a5a5a5aULL, worldX * scale * 2.3F,
                   worldZ * scale * 2.3F);
  const float ridge = 1.0F - std::abs(ridgeSource);
  const float broad = continental * 0.72F + ridge * ridge * 0.38F - 0.24F;
  return config_.baseHeight + config_.relief * broad;
}

float LandscapeField::height(const float worldX, const float worldZ) const {
  if (!std::isfinite(worldX) || !std::isfinite(worldZ) ||
      std::abs(worldX) > kMaximumWorldCoordinate ||
      std::abs(worldZ) > kMaximumWorldCoordinate)
    throw std::invalid_argument(
        "Landscape sample position is outside the supported world range");
  float result = baseHeight(worldX, worldZ);
  for (const LandscapeBrush &brush : brushes_) {
    const float dx = worldX - brush.center.x;
    const float dz = worldZ - brush.center.y;
    const float distance = std::sqrt(dx * dx + dz * dz);
    if (distance >= brush.radius)
      continue;
    const float weight =
        std::pow(1.0F - distance / brush.radius, brush.falloffExponent);
    result += brush.heightDelta * weight;
  }
  if (!std::isfinite(result))
    throw std::runtime_error("Landscape height exceeded finite range");
  return result;
}

LandscapeSample LandscapeField::sample(const float worldX,
                                       const float worldZ) const {
  if (!supportedRange({worldX, worldZ}, 0.0F, 4.0F))
    throw std::invalid_argument(
        "Landscape normal sample is outside the supported world range");
  const float epsilon = std::clamp(config_.featureSize / 2'048.0F, 0.5F, 4.0F);
  const float centerHeight = height(worldX, worldZ);
  const float dx =
      (height(worldX + epsilon, worldZ) - height(worldX - epsilon, worldZ)) /
      (2.0F * epsilon);
  const float dz =
      (height(worldX, worldZ + epsilon) - height(worldX, worldZ - epsilon)) /
      (2.0F * epsilon);
  const Vec3 normal = normalize({-dx, 1.0F, -dz});
  const float scale = 1.0F / config_.featureSize;
  const float moisture =
      std::clamp(fractalNoise(config_.seed ^ 0x51ed270b33ac9f17ULL,
                              worldX * scale * 1.7F + 37.0F,
                              worldZ * scale * 1.7F - 19.0F) *
                         0.5F +
                     0.5F,
                 0.0F, 1.0F);
  const float latitude = std::min(std::abs(worldZ) / 32'000.0F, 1.0F);
  const float elevationCooling =
      std::max(centerHeight - config_.waterLevel, 0.0F) /
      std::max(config_.relief * 3.0F, 1.0F);
  const float temperature =
      std::clamp(0.86F - latitude * 0.48F - elevationCooling, 0.0F, 1.0F);
  LandscapeBiome biome = LandscapeBiome::Meadow;
  if (centerHeight > config_.baseHeight + config_.relief * 0.58F ||
      normal.y < 0.74F) {
    biome = LandscapeBiome::Alpine;
  } else if (centerHeight <= config_.waterLevel + 12.0F || moisture > 0.76F) {
    biome = LandscapeBiome::Wetland;
  } else if (moisture > 0.43F && temperature > 0.24F) {
    biome = LandscapeBiome::Forest;
  }
  return {centerHeight, normal, moisture, temperature, biome};
}

TerrainPatch cookTerrainPatch(const LandscapeField &field,
                              const TerrainPatchDesc desc) {
  if (!supportedRange(desc.center, desc.halfExtent, 4.0F) ||
      desc.halfExtent <= 0.0F || desc.resolution < 2U ||
      desc.resolution > 256U || !std::has_single_bit(desc.resolution) ||
      !std::isfinite(desc.skirtDepth) || desc.skirtDepth <= 0.0F ||
      !std::isfinite(desc.uvScale) || desc.uvScale <= 0.0F) {
    throw std::invalid_argument("Terrain patch description is invalid");
  }

  TerrainPatch result;
  const std::uint32_t side = desc.resolution + 1U;
  const std::size_t topVertexCount =
      static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
  const std::size_t boundaryCount =
      static_cast<std::size_t>(desc.resolution) * 4U;
  result.mesh.vertices.reserve(topVertexCount + boundaryCount);
  result.mesh.indices.reserve(static_cast<std::size_t>(desc.resolution) *
                                  desc.resolution * 6U +
                              boundaryCount * 6U);
  const float minimumX = desc.center.x - desc.halfExtent;
  const float minimumZ = desc.center.y - desc.halfExtent;
  const float step =
      desc.halfExtent * 2.0F / static_cast<float>(desc.resolution);
  for (std::uint32_t row = 0U; row < side; ++row) {
    const float z = minimumZ + static_cast<float>(row) * step;
    for (std::uint32_t column = 0U; column < side; ++column) {
      const float x = minimumX + static_cast<float>(column) * step;
      const LandscapeSample sample = field.sample(x, z);
      const Vec3 tangent = normalize(
          {1.0F, -sample.normal.x / std::max(sample.normal.y, 0.001F), 0.0F});
      result.mesh.vertices.push_back(
          {{x - desc.center.x, sample.height, z - desc.center.y},
           sample.normal,
           {x * desc.uvScale, z * desc.uvScale},
           {tangent.x, tangent.y, tangent.z, 1.0F}});
    }
  }
  for (std::uint32_t row = 0U; row < desc.resolution; ++row) {
    for (std::uint32_t column = 0U; column < desc.resolution; ++column) {
      const std::uint32_t a = row * side + column;
      const std::uint32_t b = a + 1U;
      const std::uint32_t c = a + side;
      const std::uint32_t d = c + 1U;
      result.mesh.indices.insert(result.mesh.indices.end(), {a, c, b, b, c, d});
    }
  }

  std::vector<std::uint32_t> boundary;
  boundary.reserve(boundaryCount);
  for (std::uint32_t column = 0U; column < desc.resolution; ++column)
    boundary.push_back(column);
  for (std::uint32_t row = 0U; row < desc.resolution; ++row)
    boundary.push_back(row * side + desc.resolution);
  for (std::uint32_t column = desc.resolution; column > 0U; --column)
    boundary.push_back(desc.resolution * side + column);
  for (std::uint32_t row = desc.resolution; row > 0U; --row)
    boundary.push_back(row * side);

  const auto skirtFirst =
      static_cast<std::uint32_t>(result.mesh.vertices.size());
  for (const std::uint32_t topIndex : boundary) {
    Vertex vertex = result.mesh.vertices[topIndex];
    vertex.position.y -= desc.skirtDepth;
    result.mesh.vertices.push_back(vertex);
  }
  for (std::size_t index = 0U; index < boundary.size(); ++index) {
    const std::size_t next = (index + 1U) % boundary.size();
    const std::uint32_t topA = boundary[index];
    const std::uint32_t topB = boundary[next];
    const std::uint32_t bottomA =
        skirtFirst + static_cast<std::uint32_t>(index);
    const std::uint32_t bottomB = skirtFirst + static_cast<std::uint32_t>(next);
    result.mesh.indices.insert(result.mesh.indices.end(),
                               {topA, bottomA, topB, topB, bottomA, bottomB});
  }

  result.mesh.bounds = calculateBounds(result.mesh.vertices);
  result.topVertexCount = static_cast<std::uint32_t>(topVertexCount);
  result.skirtVertexCount = static_cast<std::uint32_t>(boundaryCount);
  result.topTriangleCount = desc.resolution * desc.resolution * 2U;
  result.skirtTriangleCount = static_cast<std::uint32_t>(boundaryCount * 2U);
  return result;
}

MeshData createWaterPatch(const Vec2 center, const float halfExtent,
                          const float height, const float uvScale) {
  if (!supportedRange(center, halfExtent) || halfExtent <= 0.0F ||
      !std::isfinite(height) || std::abs(height) > 1'000'000.0F ||
      !std::isfinite(uvScale) || uvScale <= 0.0F) {
    throw std::invalid_argument("Water patch description is invalid");
  }
  const float minimumX = center.x - halfExtent;
  const float maximumX = center.x + halfExtent;
  const float minimumZ = center.y - halfExtent;
  const float maximumZ = center.y + halfExtent;
  MeshData mesh;
  mesh.vertices = {
      {{-halfExtent, height, -halfExtent},
       {0.0F, 1.0F, 0.0F},
       {minimumX * uvScale, minimumZ * uvScale},
       {1.0F, 0.0F, 0.0F, 1.0F}},
      {{halfExtent, height, -halfExtent},
       {0.0F, 1.0F, 0.0F},
       {maximumX * uvScale, minimumZ * uvScale},
       {1.0F, 0.0F, 0.0F, 1.0F}},
      {{-halfExtent, height, halfExtent},
       {0.0F, 1.0F, 0.0F},
       {minimumX * uvScale, maximumZ * uvScale},
       {1.0F, 0.0F, 0.0F, 1.0F}},
      {{halfExtent, height, halfExtent},
       {0.0F, 1.0F, 0.0F},
       {maximumX * uvScale, maximumZ * uvScale},
       {1.0F, 0.0F, 0.0F, 1.0F}},
  };
  mesh.indices = {0U, 2U, 1U, 1U, 2U, 3U};
  mesh.bounds = calculateBounds(mesh.vertices);
  return mesh;
}

std::vector<FoliageInstance> scatterFoliage(const LandscapeField &field,
                                            const FoliageScatterDesc desc) {
  if (!supportedRange(desc.center, desc.halfExtent, 4.0F) ||
      desc.halfExtent <= 0.0F || !std::isfinite(desc.spacing) ||
      desc.spacing < 1.0F || desc.spacing > 1'000'000.0F ||
      !std::isfinite(desc.jitter) || desc.jitter < 0.0F || desc.jitter > 1.0F ||
      !std::isfinite(desc.maximumSlopeDegrees) ||
      desc.maximumSlopeDegrees < 0.0F || desc.maximumSlopeDegrees >= 90.0F ||
      desc.maximumInstances == 0U ||
      desc.maximumInstances > FoliageScatterDesc::kMaximumInstances ||
      desc.seed == 0U) {
    throw std::invalid_argument("Foliage scatter description is invalid");
  }
  const float minimumX = desc.center.x - desc.halfExtent;
  const float maximumX = desc.center.x + desc.halfExtent;
  const float minimumZ = desc.center.y - desc.halfExtent;
  const float maximumZ = desc.center.y + desc.halfExtent;
  const auto beginX =
      static_cast<std::int64_t>(std::floor(minimumX / desc.spacing)) - 1;
  const auto endX =
      static_cast<std::int64_t>(std::ceil(maximumX / desc.spacing)) + 1;
  const auto beginZ =
      static_cast<std::int64_t>(std::floor(minimumZ / desc.spacing)) - 1;
  const auto endZ =
      static_cast<std::int64_t>(std::ceil(maximumZ / desc.spacing)) + 1;
  const auto candidateColumns = static_cast<std::uint64_t>(endX - beginX + 1);
  const auto candidateRows = static_cast<std::uint64_t>(endZ - beginZ + 1);
  if (candidateRows == 0U || candidateColumns == 0U ||
      candidateColumns >
          FoliageScatterDesc::kMaximumCandidates / candidateRows) {
    throw std::invalid_argument(
        "Foliage scatter candidate grid exceeds bounded capacity");
  }
  const float minimumNormalY =
      std::cos(desc.maximumSlopeDegrees * std::numbers::pi_v<float> / 180.0F);
  const float baseProbability = densityProbability(desc.density);
  std::vector<FoliageInstance> result;
  result.reserve(std::min<std::size_t>(desc.maximumInstances, 4'096U));
  for (std::int64_t zCell = beginZ; zCell <= endZ; ++zCell) {
    for (std::int64_t xCell = beginX; xCell <= endX; ++xCell) {
      const std::uint64_t hash = hashGrid(desc.seed, xCell, zCell);
      const float jitterX =
          (unitFloat(hash) - 0.5F) * desc.spacing * desc.jitter;
      const float jitterZ =
          (unitFloat(mix64(hash ^ 0x9e3779b97f4a7c15ULL)) - 0.5F) *
          desc.spacing * desc.jitter;
      const float x =
          (static_cast<float>(xCell) + 0.5F) * desc.spacing + jitterX;
      const float z =
          (static_cast<float>(zCell) + 0.5F) * desc.spacing + jitterZ;
      if (x < minimumX || x >= maximumX || z < minimumZ || z >= maximumZ)
        continue;
      const LandscapeSample sample = field.sample(x, z);
      if (sample.height <= field.config().waterLevel + 0.5F ||
          sample.normal.y < minimumNormalY ||
          sample.biome == LandscapeBiome::Alpine)
        continue;
      float biomeProbability = 1.0F;
      if (sample.biome == LandscapeBiome::Forest)
        biomeProbability = 1.18F;
      else if (sample.biome == LandscapeBiome::Wetland)
        biomeProbability = 0.68F;
      if (unitFloat(mix64(hash ^ 0xd1b54a32d192ed03ULL)) >=
          std::min(baseProbability * biomeProbability, 0.96F))
        continue;
      if (result.size() == desc.maximumInstances)
        throw std::overflow_error(
            "Foliage scatter instance capacity exhausted");

      const float speciesValue = unitFloat(mix64(hash ^ 0x94d049bb133111ebULL));
      FoliageSpecies species = FoliageSpecies::Grass;
      if (sample.biome == LandscapeBiome::Forest) {
        species = speciesValue < 0.34F
                      ? FoliageSpecies::Tree
                      : (speciesValue < 0.68F ? FoliageSpecies::Shrub
                                              : FoliageSpecies::Grass);
      } else if (sample.biome == LandscapeBiome::Wetland) {
        species = speciesValue < 0.86F ? FoliageSpecies::Grass
                                       : FoliageSpecies::Shrub;
      } else {
        species = speciesValue < 0.78F ? FoliageSpecies::Grass
                                       : FoliageSpecies::Shrub;
      }
      const float yaw = unitFloat(mix64(hash ^ 0x243f6a8885a308d3ULL)) * 2.0F *
                        std::numbers::pi_v<float>;
      const float scale =
          0.78F + unitFloat(mix64(hash ^ 0x13198a2e03707344ULL)) * 0.54F;
      result.push_back({{x, sample.height, z}, yaw, scale, species});
    }
  }
  return result;
}

MeshData createGrassClusterMesh() {
  MeshData mesh;
  appendQuad(mesh,
             {{{-0.35F, 0.0F, 0.0F},
               {0.35F, 0.0F, 0.0F},
               {-0.28F, 1.5F, 0.0F},
               {0.28F, 1.5F, 0.0F}}},
             {0.0F, 0.0F, 1.0F});
  appendQuad(mesh,
             {{{0.0F, 0.0F, -0.35F},
               {0.0F, 0.0F, 0.35F},
               {0.0F, 1.5F, -0.28F},
               {0.0F, 1.5F, 0.28F}}},
             {1.0F, 0.0F, 0.0F});
  mesh.bounds = calculateBounds(mesh.vertices);
  return mesh;
}

MeshData createShrubMesh() {
  MeshData mesh;
  constexpr std::array<Vec3, 4> ring{{{-0.75F, 0.45F, -0.75F},
                                      {0.75F, 0.45F, -0.75F},
                                      {0.75F, 0.45F, 0.75F},
                                      {-0.75F, 0.45F, 0.75F}}};
  const Vec3 top{0.0F, 1.65F, 0.0F};
  const Vec3 bottom{0.0F, 0.0F, 0.0F};
  for (std::size_t index = 0U; index < ring.size(); ++index) {
    const Vec3 a = ring[index];
    const Vec3 b = ring[(index + 1U) % ring.size()];
    appendTriangle(mesh, a, top, b, normalize(cross(top - a, b - a)));
    appendTriangle(mesh, a, b, bottom, normalize(cross(b - a, bottom - a)));
  }
  mesh.bounds = calculateBounds(mesh.vertices);
  return mesh;
}

MeshData createTreeMesh() {
  MeshData mesh;
  constexpr std::uint32_t sides = 6U;
  constexpr float trunkRadius = 0.24F;
  constexpr float trunkHeight = 2.4F;
  for (std::uint32_t side = 0U; side < sides; ++side) {
    const float a0 = static_cast<float>(side) * 2.0F *
                     std::numbers::pi_v<float> / static_cast<float>(sides);
    const float a1 = static_cast<float>(side + 1U) * 2.0F *
                     std::numbers::pi_v<float> / static_cast<float>(sides);
    const Vec3 p0{std::cos(a0) * trunkRadius, 0.0F, std::sin(a0) * trunkRadius};
    const Vec3 p1{std::cos(a1) * trunkRadius, 0.0F, std::sin(a1) * trunkRadius};
    const Vec3 p2{p0.x, trunkHeight, p0.z};
    const Vec3 p3{p1.x, trunkHeight, p1.z};
    appendQuad(mesh, {{p0, p1, p2, p3}},
               normalize({p0.x + p1.x, 0.0F, p0.z + p1.z}));
  }
  constexpr float canopyRadius = 1.15F;
  const Vec3 top{0.0F, 5.2F, 0.0F};
  const Vec3 lower{0.0F, 1.7F, 0.0F};
  for (std::uint32_t side = 0U; side < sides; ++side) {
    const float a0 = static_cast<float>(side) * 2.0F *
                     std::numbers::pi_v<float> / static_cast<float>(sides);
    const float a1 = static_cast<float>(side + 1U) * 2.0F *
                     std::numbers::pi_v<float> / static_cast<float>(sides);
    const Vec3 p0{std::cos(a0) * canopyRadius, 2.25F,
                  std::sin(a0) * canopyRadius};
    const Vec3 p1{std::cos(a1) * canopyRadius, 2.25F,
                  std::sin(a1) * canopyRadius};
    appendTriangle(mesh, p0, top, p1, normalize(cross(top - p0, p1 - p0)));
    appendTriangle(mesh, p0, p1, lower, normalize(cross(p1 - p0, lower - p0)));
  }
  mesh.bounds = calculateBounds(mesh.vertices);
  return mesh;
}

const char *landscapeBiomeName(const LandscapeBiome biome) noexcept {
  switch (biome) {
  case LandscapeBiome::Alpine:
    return "alpine";
  case LandscapeBiome::Meadow:
    return "meadow";
  case LandscapeBiome::Forest:
    return "forest";
  case LandscapeBiome::Wetland:
    return "wetland";
  }
  return "unknown";
}

} // namespace ve
