#pragma once

#include "assets/RuntimeAssets.hpp"
#include "core/World.hpp"
#include "renderer/SceneRenderer.hpp"
#include "scene/SceneReflection.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ve {

struct CookedWorldLimits {
  std::size_t maximumEntities = 1'000'000U;
  std::size_t maximumBytes = 256U * 1024U * 1024U;
  std::size_t maximumNameBytes = 255U;
};

struct CookedWorld {
  std::vector<SceneEntityId> identities;
  std::vector<std::string> names;
  std::vector<std::uint32_t> parentIndices;
  std::vector<TransformTRS> transforms;
  std::vector<std::uint8_t> renderableMask;
  std::vector<AuthoringRenderable> renderables;
};

struct CookedWorldAssetResolver {
  void *context = nullptr;
  MeshAssetHandle (*mesh)(void *context, AssetId id) = nullptr;
  RenderMaterial (*material)(void *context, AssetId id) = nullptr;
  MeshBounds (*bounds)(void *context, MeshAssetHandle mesh) = nullptr;
};

void validateCookedWorld(const CookedWorld &world,
                         const CookedWorldLimits &limits = {});
[[nodiscard]] std::vector<std::byte>
encodeCookedWorld(const CookedWorld &world,
                  const CookedWorldLimits &limits = {});
[[nodiscard]] CookedWorld
decodeCookedWorld(std::span<const std::byte> bytes,
                  const CookedWorldLimits &limits = {});
void saveCookedWorld(const CookedWorld &world,
                     const std::filesystem::path &path,
                     const CookedWorldLimits &limits = {});
[[nodiscard]] CookedWorld loadCookedWorld(const std::filesystem::path &path,
                                          const CookedWorldLimits &limits = {});
void instantiateCookedWorld(World &destination, const CookedWorld &source,
                            const CookedWorldAssetResolver &resolver,
                            const CookedWorldLimits &limits = {});

} // namespace ve
