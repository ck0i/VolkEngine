#pragma once

#include "core/Math.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace ve {

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
};
static_assert(sizeof(Vertex) == 48, "CPU mesh Vertex layout changed");
static_assert(offsetof(Vertex, position) == 0, "Vertex position offset changed");
static_assert(offsetof(Vertex, normal) == 12, "Vertex normal offset changed");
static_assert(offsetof(Vertex, uv) == 24, "Vertex uv offset changed");
static_assert(offsetof(Vertex, tangent) == 32, "Vertex tangent offset changed");

struct MeshBounds {
    Vec3 center{};
    float radius = 0.0f;
    bool valid = false;
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    MeshBounds bounds{};
};

[[nodiscard]] MeshData createCubeMesh();
[[nodiscard]] MeshData createUvSphereMesh(std::uint32_t rings, std::uint32_t segments);
[[nodiscard]] MeshData createPlaneMesh(float halfExtent, float uvScale);
[[nodiscard]] MeshData createGridMesh(float halfExtent, std::uint32_t divisions);
void optimizeTriangleIndexOrderForVertexCache(std::vector<std::uint32_t>& indices, std::size_t vertexCount, std::uint32_t cacheSize = 32);
[[nodiscard]] MeshData loadObjMesh(const std::filesystem::path& path);

} // namespace ve
