#pragma once

#include "core/World.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace ve {

struct ScenePersistenceLimits {
  std::size_t maxEntities = 1'000'000U;
  std::size_t maxBytes = 256U * 1024U * 1024U;
};

[[nodiscard]] std::vector<std::byte>
encodeWorldScene(const World &world, const ScenePersistenceLimits &limits = {});
void decodeWorldScene(World &destination, std::span<const std::byte> bytes,
                      const ScenePersistenceLimits &limits = {});
void saveWorldScene(const World &world, const std::filesystem::path &path,
                    const ScenePersistenceLimits &limits = {});
void loadWorldScene(World &destination, const std::filesystem::path &path,
                    const ScenePersistenceLimits &limits = {});

} // namespace ve
