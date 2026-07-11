#include "landscape/Landscape.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

template <typename Exception, typename Function>
bool throws(Function &&function) {
  try {
    function();
    return false;
  } catch (const Exception &) {
    return true;
  }
}

bool sameVec3(const ve::Vec3 left, const ve::Vec3 right) {
  return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool sameInstance(const ve::FoliageInstance &left,
                  const ve::FoliageInstance &right) {
  return sameVec3(left.position, right.position) &&
         left.yawRadians == right.yawRadians && left.scale == right.scale &&
         left.species == right.species;
}

void validateMesh(const ve::MeshData &mesh) {
  assert(!mesh.vertices.empty());
  assert(!mesh.indices.empty() && mesh.indices.size() % 3U == 0U);
  assert(mesh.bounds.valid && std::isfinite(mesh.bounds.radius) &&
         mesh.bounds.radius > 0.0F);
  for (const ve::Vertex &vertex : mesh.vertices) {
    assert(ve::finite(vertex.position));
    assert(ve::finite(vertex.normal));
    assert(std::isfinite(vertex.uv.x) && std::isfinite(vertex.uv.y));
    assert(std::isfinite(vertex.tangent.x) && std::isfinite(vertex.tangent.y) &&
           std::isfinite(vertex.tangent.z) && std::isfinite(vertex.tangent.w));
    assert(std::abs(ve::length(vertex.normal) - 1.0F) < 0.001F);
  }
  for (const std::uint32_t index : mesh.indices)
    assert(index < mesh.vertices.size());
}

using InstanceKey =
    std::tuple<float, float, float, float, float, ve::FoliageSpecies>;

std::vector<InstanceKey>
keys(const std::vector<ve::FoliageInstance> &instances) {
  std::vector<InstanceKey> result;
  result.reserve(instances.size());
  for (const ve::FoliageInstance &instance : instances) {
    result.emplace_back(instance.position.x, instance.position.y,
                        instance.position.z, instance.yawRadians,
                        instance.scale, instance.species);
  }
  std::ranges::sort(result);
  return result;
}

} // namespace

int main() {
  assert(throws<std::invalid_argument>(
      [] { static_cast<void>(ve::LandscapeField{{.seed = 0U}}); }));
  assert(throws<std::invalid_argument>([] {
    static_cast<void>(ve::LandscapeField{
        {.seed = 1U, .featureSize = std::numeric_limits<float>::infinity()}});
  }));

  const ve::LandscapeConfig config{.seed = 0x4d32544552524149ULL,
                                   .baseHeight = 100.0F,
                                   .relief = 40.0F,
                                   .featureSize = 2'048.0F,
                                   .waterLevel = -100.0F};
  ve::LandscapeField field{config};
  ve::LandscapeField duplicate{config};
  constexpr std::array positions{ve::Vec2{-4'096.0F, -2'048.0F},
                                 ve::Vec2{-1.0F, 7.0F}, ve::Vec2{0.0F, 0.0F},
                                 ve::Vec2{3'333.25F, -9'999.5F}};
  for (const ve::Vec2 position : positions) {
    const ve::LandscapeSample first = field.sample(position.x, position.y);
    const ve::LandscapeSample second = duplicate.sample(position.x, position.y);
    assert(first.height == second.height &&
           sameVec3(first.normal, second.normal) &&
           first.moisture == second.moisture &&
           first.temperature == second.temperature &&
           first.biome == second.biome);
    assert(std::isfinite(first.height));
    assert(std::abs(ve::length(first.normal) - 1.0F) < 0.001F);
    assert(first.moisture >= 0.0F && first.moisture <= 1.0F);
    assert(first.temperature >= 0.0F && first.temperature <= 1.0F);
  }
  const ve::LandscapeField different{{.seed = config.seed + 1U,
                                      .baseHeight = config.baseHeight,
                                      .relief = config.relief,
                                      .featureSize = config.featureSize,
                                      .waterLevel = config.waterLevel}};
  assert(field.height(123.0F, 456.0F) != different.height(123.0F, 456.0F));
  assert(throws<std::invalid_argument>([&] {
    static_cast<void>(field.height(std::numeric_limits<float>::max(), 0.0F));
  }));
  assert(throws<std::invalid_argument>([&] {
    static_cast<void>(
        field.sample(ve::LandscapeField::kMaximumWorldCoordinate, 0.0F));
  }));

  const float originalCenter = field.height(0.0F, 0.0F);
  const float originalOutside = field.height(1'000.0F, 0.0F);
  const std::uint64_t originalRevision = field.revision();
  field.addBrush({.center = {},
                  .radius = 256.0F,
                  .heightDelta = 30.0F,
                  .falloffExponent = 2.0F});
  assert(field.revision() == originalRevision + 1U);
  assert(field.height(0.0F, 0.0F) == originalCenter + 30.0F);
  assert(field.height(1'000.0F, 0.0F) == originalOutside);
  assert(throws<std::invalid_argument>([&] {
    field.addBrush(
        {.center = {ve::LandscapeField::kMaximumWorldCoordinate, 0.0F},
         .radius = 32.0F,
         .heightDelta = 1.0F});
  }));
  field.clearBrushes();
  assert(field.height(0.0F, 0.0F) == originalCenter);
  assert(field.revision() == originalRevision + 2U);

  ve::TerrainPatchDesc patchDesc{.center = {},
                                 .halfExtent = 64.0F,
                                 .resolution = 8U,
                                 .lodLevel = 0U,
                                 .skirtDepth = 12.0F,
                                 .uvScale = 1.0F / 64.0F};
  const ve::TerrainPatch patch = ve::cookTerrainPatch(field, patchDesc);
  validateMesh(patch.mesh);
  assert(patch.topVertexCount == 81U);
  assert(patch.skirtVertexCount == 32U);
  assert(patch.topTriangleCount == 128U);
  assert(patch.skirtTriangleCount == 64U);
  assert(patch.mesh.vertices.size() == 113U);
  assert(patch.mesh.indices.size() == (128U + 64U) * 3U);
  for (std::size_t index = patch.topVertexCount;
       index < patch.mesh.vertices.size(); ++index) {
    assert(patch.mesh.vertices[index].position.y <
           patch.mesh.bounds.center.y + patch.mesh.bounds.radius);
  }

  ve::TerrainPatchDesc leftDesc = patchDesc;
  leftDesc.center = {-64.0F, 0.0F};
  ve::TerrainPatchDesc rightDesc = patchDesc;
  rightDesc.center = {64.0F, 0.0F};
  const ve::TerrainPatch left = ve::cookTerrainPatch(field, leftDesc);
  const ve::TerrainPatch right = ve::cookTerrainPatch(field, rightDesc);
  constexpr std::uint32_t fineSide = 9U;
  for (std::uint32_t row = 0U; row < fineSide; ++row) {
    const ve::Vertex &leftEdge =
        left.mesh.vertices[row * fineSide + patchDesc.resolution];
    const ve::Vertex &rightEdge = right.mesh.vertices[row * fineSide];
    assert(leftEdge.position.x + leftDesc.center.x ==
           rightEdge.position.x + rightDesc.center.x);
    assert(leftEdge.position.y == rightEdge.position.y);
    assert(leftEdge.position.z + leftDesc.center.y ==
           rightEdge.position.z + rightDesc.center.y);
    assert(sameVec3(leftEdge.normal, rightEdge.normal));
    assert(leftEdge.uv.x == rightEdge.uv.x && leftEdge.uv.y == rightEdge.uv.y);
  }

  ve::TerrainPatchDesc coarseDesc = rightDesc;
  coarseDesc.resolution = 4U;
  coarseDesc.lodLevel = 1U;
  const ve::TerrainPatch coarse = ve::cookTerrainPatch(field, coarseDesc);
  constexpr std::uint32_t coarseSide = 5U;
  for (std::uint32_t row = 0U; row < coarseSide; ++row) {
    const ve::Vertex &fineEdge =
        left.mesh.vertices[(row * 2U) * fineSide + patchDesc.resolution];
    const ve::Vertex &coarseEdge = coarse.mesh.vertices[row * coarseSide];
    assert(fineEdge.position.y == coarseEdge.position.y);
    assert(fineEdge.position.z + leftDesc.center.y ==
           coarseEdge.position.z + coarseDesc.center.y);
  }
  assert(throws<std::invalid_argument>([&] {
    ve::TerrainPatchDesc invalid = patchDesc;
    invalid.resolution = 3U;
    static_cast<void>(ve::cookTerrainPatch(field, invalid));
  }));
  assert(throws<std::invalid_argument>([&] {
    ve::TerrainPatchDesc invalid = patchDesc;
    invalid.center.x = ve::LandscapeField::kMaximumWorldCoordinate;
    static_cast<void>(ve::cookTerrainPatch(field, invalid));
  }));

  const ve::FoliageScatterDesc scatterDesc{.center = {},
                                           .halfExtent = 128.0F,
                                           .spacing = 16.0F,
                                           .jitter = 0.75F,
                                           .maximumSlopeDegrees = 80.0F,
                                           .maximumInstances = 10'000U,
                                           .density = ve::FoliageDensity::High,
                                           .seed = config.seed};
  const std::vector<ve::FoliageInstance> foliage =
      ve::scatterFoliage(field, scatterDesc);
  const std::vector<ve::FoliageInstance> repeated =
      ve::scatterFoliage(field, scatterDesc);
  assert(!foliage.empty() && foliage.size() == repeated.size());
  for (std::size_t index = 0U; index < foliage.size(); ++index) {
    assert(sameInstance(foliage[index], repeated[index]));
    const ve::FoliageInstance &instance = foliage[index];
    assert(instance.position.x >= -128.0F && instance.position.x < 128.0F);
    assert(instance.position.z >= -128.0F && instance.position.z < 128.0F);
    assert(instance.position.y ==
           field.height(instance.position.x, instance.position.z));
    assert(instance.yawRadians >= 0.0F &&
           instance.yawRadians < 2.0F * std::numbers::pi_v<float>);
    assert(instance.scale >= 0.78F && instance.scale <= 1.32F);
  }
  ve::FoliageScatterDesc lowDesc = scatterDesc;
  lowDesc.density = ve::FoliageDensity::Low;
  const auto low = ve::scatterFoliage(field, lowDesc);
  assert(low.size() < foliage.size());

  std::vector<ve::FoliageInstance> split;
  for (const ve::Vec2 center :
       std::array{ve::Vec2{-64.0F, -64.0F}, ve::Vec2{64.0F, -64.0F},
                  ve::Vec2{-64.0F, 64.0F}, ve::Vec2{64.0F, 64.0F}}) {
    ve::FoliageScatterDesc cellScatter = scatterDesc;
    cellScatter.center = center;
    cellScatter.halfExtent = 64.0F;
    auto cellFoliage = ve::scatterFoliage(field, cellScatter);
    split.insert(split.end(), cellFoliage.begin(), cellFoliage.end());
  }
  assert(keys(split) == keys(foliage));

  assert(throws<std::invalid_argument>([&] {
    ve::FoliageScatterDesc invalid = scatterDesc;
    invalid.halfExtent = 10'000'000.0F;
    invalid.spacing = 1.0F;
    static_cast<void>(ve::scatterFoliage(field, invalid));
  }));
  assert(throws<std::overflow_error>([&] {
    ve::FoliageScatterDesc invalid = scatterDesc;
    invalid.maximumInstances = 1U;
    static_cast<void>(ve::scatterFoliage(field, invalid));
  }));

  validateMesh(ve::createGrassClusterMesh());
  validateMesh(ve::createShrubMesh());
  validateMesh(ve::createTreeMesh());
  assert(std::string_view{ve::landscapeBiomeName(ve::LandscapeBiome::Alpine)} ==
         "alpine");
  assert(std::string_view{ve::landscapeBiomeName(ve::LandscapeBiome::Meadow)} ==
         "meadow");
  assert(std::string_view{ve::landscapeBiomeName(ve::LandscapeBiome::Forest)} ==
         "forest");
  assert(std::string_view{
             ve::landscapeBiomeName(ve::LandscapeBiome::Wetland)} == "wetland");
  return 0;
}
