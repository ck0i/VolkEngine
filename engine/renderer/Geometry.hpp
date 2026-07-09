#pragma once

#include "core/Math.hpp"

#include <cstdint>
#include <vector>

namespace ve {

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

[[nodiscard]] MeshData createCubeMesh();
[[nodiscard]] MeshData createUvSphereMesh(std::uint32_t rings, std::uint32_t segments);
[[nodiscard]] MeshData createPlaneMesh(float halfExtent, float uvScale);
[[nodiscard]] MeshData createGridMesh(float halfExtent, std::uint32_t divisions);

} // namespace ve
