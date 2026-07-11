#pragma once

#include "renderer/Geometry.hpp"

#include <cstdint>
#include <vector>

namespace ve {

inline constexpr std::uint32_t kMaximumClusterVertices = 64U;
inline constexpr std::uint32_t kMaximumClusterTriangles = 126U;
inline constexpr std::uint32_t kInvalidClusterNode = 0xffffffffU;

struct MeshCluster {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t vertexCount = 0;
    MeshBounds bounds{};
};

struct MeshClusterNode {
    MeshBounds bounds{};
    std::uint32_t left = kInvalidClusterNode;
    std::uint32_t right = kInvalidClusterNode;
    std::uint32_t cluster = kInvalidClusterNode;
};

struct MeshClusterData {
    std::vector<MeshCluster> clusters;
    std::vector<MeshClusterNode> hierarchy;
    std::uint32_t root = kInvalidClusterNode;
};

[[nodiscard]] MeshClusterData buildMeshClusters(const MeshData& mesh);
void validateMeshClusters(const MeshData& mesh, const MeshClusterData& clusters);

} // namespace ve
