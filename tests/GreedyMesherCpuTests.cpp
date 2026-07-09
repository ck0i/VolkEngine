#include "renderer/GreedyMesher.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int gFailureCount = 0;

void expectTrue(const std::string_view context, const bool value) {
    if (!value) {
        std::cerr << "[FAILED] " << context << ": expected true\n";
        ++gFailureCount;
    }
}

void expectEqual(const std::string_view context, const std::size_t actual, const std::size_t expected) {
    if (actual != expected) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}

void expectNearly(const std::string_view context, const float actual, const float expected, const float epsilon = 1.0e-5f) {
    if (std::fabs(actual - expected) > epsilon) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}

void expectVec3Nearly(const std::string_view context, const ve::Vec3 actual, const ve::Vec3 expected) {
    expectNearly(std::string(context) + " x", actual.x, expected.x);
    expectNearly(std::string(context) + " y", actual.y, expected.y);
    expectNearly(std::string(context) + " z", actual.z, expected.z);
}

template <typename Callable>
void expectRuntimeError(const std::string_view context, Callable&& callable) {
    try {
        callable();
        std::cerr << "[FAILED] " << context << ": expected runtime_error\n";
        ++gFailureCount;
    } catch (const std::runtime_error&) {
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": expected runtime_error, got another exception\n";
        ++gFailureCount;
    }
}

ve::GreedyMeshResult mesh(const std::array<std::uint32_t, 3> dimensions,
                          const std::vector<std::uint32_t>& cells,
                          const bool meshExterior = true) {
    ve::GreedyMeshingVolume volume{};
    volume.dimensions = dimensions;
    volume.cells = cells;
    volume.meshExterior = meshExterior;
    return ve::generateGreedyMesh(volume);
}

void expectTangentBasis(const ve::GreedyMeshResult& result) {
    expectTrue("quad output is four-vertex indexed quads", result.mesh.vertices.size() % 4U == 0U);
    for (std::size_t base = 0; base + 3U < result.mesh.vertices.size(); base += 4U) {
        const ve::Vertex& v0 = result.mesh.vertices[base];
        const ve::Vertex& v1 = result.mesh.vertices[base + 1U];
        const ve::Vertex& v2 = result.mesh.vertices[base + 2U];
        const ve::Vec3 uDirection = ve::normalize(v2.position - v0.position);
        const ve::Vec3 vDirection = ve::normalize(v1.position - v0.position);
        const ve::Vec3 tangent{v0.tangent.x, v0.tangent.y, v0.tangent.z};
        const ve::Vec3 bitangent = ve::cross(v0.normal, tangent) * v0.tangent.w;
        const ve::Vec3 geometricNormal = ve::normalize(ve::cross(v1.position - v0.position, v2.position - v0.position));
        expectNearly("quad winding follows normal", ve::dot(geometricNormal, v0.normal), 1.0f);
        expectNearly("tangent follows U edge", ve::dot(tangent, uDirection), 1.0f);
        expectNearly("bitangent follows V edge", ve::dot(bitangent, vDirection), 1.0f);
        expectNearly("tangent is orthogonal to normal", ve::dot(tangent, v0.normal), 0.0f);
    }
}

void expectQuadBounds(const std::string_view context,
                      const ve::GreedyMeshResult& result,
                      const ve::Vec3 expectedNormal,
                      const ve::Vec3 expectedMinimum,
                      const ve::Vec3 expectedMaximum) {
    std::size_t matches = 0U;
    for (std::size_t base = 0; base + 3U < result.mesh.vertices.size(); base += 4U) {
        if (ve::dot(result.mesh.vertices[base].normal, expectedNormal) < 0.9999f) {
            continue;
        }
        ve::Vec3 minimum = result.mesh.vertices[base].position;
        ve::Vec3 maximum = minimum;
        for (std::size_t vertex = base + 1U; vertex <= base + 3U; ++vertex) {
            const ve::Vec3 position = result.mesh.vertices[vertex].position;
            minimum.x = std::min(minimum.x, position.x);
            minimum.y = std::min(minimum.y, position.y);
            minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x);
            maximum.y = std::max(maximum.y, position.y);
            maximum.z = std::max(maximum.z, position.z);
        }
        if (std::fabs(minimum.x - expectedMinimum.x) <= 1.0e-5f &&
            std::fabs(minimum.y - expectedMinimum.y) <= 1.0e-5f &&
            std::fabs(minimum.z - expectedMinimum.z) <= 1.0e-5f &&
            std::fabs(maximum.x - expectedMaximum.x) <= 1.0e-5f &&
            std::fabs(maximum.y - expectedMaximum.y) <= 1.0e-5f &&
            std::fabs(maximum.z - expectedMaximum.z) <= 1.0e-5f) {
            ++matches;
        }
    }
    expectEqual(context, matches, 1U);
}

void expectEquivalent(const std::string_view context,
                      const ve::GreedyMeshResult& actual,
                      const ve::GreedyMeshResult& expected) {
    expectEqual(std::string(context) + " vertex count", actual.mesh.vertices.size(), expected.mesh.vertices.size());
    expectEqual(std::string(context) + " index count", actual.mesh.indices.size(), expected.mesh.indices.size());
    expectEqual(std::string(context) + " material range count", actual.materialRanges.size(), expected.materialRanges.size());
    expectEqual(std::string(context) + " visible faces", actual.visibleFaceCount, expected.visibleFaceCount);
    expectEqual(std::string(context) + " merged quads", actual.mergedQuadCount, expected.mergedQuadCount);
    for (std::size_t index = 0; index < actual.mesh.vertices.size() && index < expected.mesh.vertices.size(); ++index) {
        expectVec3Nearly(std::string(context) + " vertex position", actual.mesh.vertices[index].position,
                         expected.mesh.vertices[index].position);
    }
    for (std::size_t index = 0; index < actual.mesh.indices.size() && index < expected.mesh.indices.size(); ++index) {
        expectEqual(std::string(context) + " index", actual.mesh.indices[index], expected.mesh.indices[index]);
    }
    for (std::size_t range = 0; range < actual.materialRanges.size() && range < expected.materialRanges.size(); ++range) {
        expectEqual(std::string(context) + " material", actual.materialRanges[range].material,
                    expected.materialRanges[range].material);
        expectEqual(std::string(context) + " range first index", actual.materialRanges[range].firstIndex,
                    expected.materialRanges[range].firstIndex);
        expectEqual(std::string(context) + " range index count", actual.materialRanges[range].indexCount,
                    expected.materialRanges[range].indexCount);
    }
}

} // namespace

int main() {
    {
        const ve::GreedyMeshResult result = mesh({1, 1, 1}, {1U});
        expectEqual("single cell visible faces", result.visibleFaceCount, 6U);
        expectEqual("single cell greedy quads", result.mergedQuadCount, 6U);
        expectEqual("single cell vertices", result.mesh.vertices.size(), 24U);
        expectEqual("single cell indices", result.mesh.indices.size(), 36U);
        expectEqual("single material range count", result.materialRanges.size(), 1U);
        expectVec3Nearly("single cell bounds center", result.mesh.bounds.center, {0.5f, 0.5f, 0.5f});
        expectNearly("single cell bounds radius", result.mesh.bounds.radius, std::sqrt(3.0f) * 0.5f);
        expectTangentBasis(result);
    }

    {
        const ve::GreedyMeshResult result = mesh({2, 1, 1}, {1U, 1U});
        expectEqual("two equal cells visible faces", result.visibleFaceCount, 10U);
        expectEqual("two equal cells merged quads", result.mergedQuadCount, 6U);
        expectEqual("two equal cells indices", result.mesh.indices.size(), 36U);
        expectTangentBasis(result);
    }

    {
        const ve::GreedyMeshResult result = mesh({2, 1, 1}, {1U, 2U});
        expectEqual("different materials still cull internal face", result.visibleFaceCount, 10U);
        expectEqual("different materials stay separate", result.mergedQuadCount, 10U);
        std::size_t rangedIndices = 0U;
        for (const ve::GreedyMeshMaterialRange& range : result.materialRanges) {
            rangedIndices += range.indexCount;
        }
        expectEqual("material ranges cover indices", rangedIndices, result.mesh.indices.size());
    }

    {
        const std::vector<std::uint32_t> solidNeighbor{2U};
        ve::GreedyMeshingVolume volume{};
        volume.dimensions = {1, 1, 1};
        const std::vector<std::uint32_t> cells{1U};
        volume.cells = cells;
        volume.neighbors[1] = solidNeighbor;
        const ve::GreedyMeshResult culled = ve::generateGreedyMesh(volume);
        expectEqual("solid boundary neighbor culls one face", culled.visibleFaceCount, 5U);
        volume.emitBoundaryFaces = true;
        const ve::GreedyMeshResult emitted = ve::generateGreedyMesh(volume);
        expectEqual("boundary override emits all faces", emitted.visibleFaceCount, 6U);
    }

    {
        const std::vector<std::uint32_t> cells(4U, 1U);
        const std::vector<std::uint32_t> negativeXBoundary{0U, 0U, 0U, 2U};
        ve::GreedyMeshingVolume volume{};
        volume.dimensions = {1, 2, 2};
        volume.cells = cells;
        volume.neighbors[0] = negativeXBoundary;
        const ve::GreedyMeshResult result = ve::generateGreedyMesh(volume);
        expectEqual("boundary plane indexing culls matching cell", result.visibleFaceCount, 15U);
    }
    {
        ve::GreedyMeshingVolume volume{};
        volume.dimensions = {2, 1, 1};
        const std::vector<std::uint32_t> cells{1U, 1U};
        const std::vector<std::uint32_t> invalidBoundary{0U, 0U};
        volume.cells = cells;
        volume.neighbors[0] = invalidBoundary;
        expectRuntimeError("boundary plane dimensions are validated", [&] { (void)ve::generateGreedyMesh(volume); });
    }

    {
        ve::GreedyMeshingVolume volume{};
        volume.dimensions = {0, 1, 1};
        const std::vector<std::uint32_t> cells{};
        volume.cells = cells;
        expectRuntimeError("zero dimensions are rejected", [&] { (void)ve::generateGreedyMesh(volume); });
        volume.dimensions = {1, 1, 1};
        expectRuntimeError("cell count mismatch is rejected", [&] { (void)ve::generateGreedyMesh(volume); });
    }

    {
        const ve::GreedyMeshResult result = mesh({2, 3, 4}, std::vector<std::uint32_t>(24U, 7U));
        expectEqual("asymmetric solid visible faces", result.visibleFaceCount, 52U);
        expectEqual("asymmetric solid merged quads", result.mergedQuadCount, 6U);
        expectQuadBounds("negative X merged extent", result, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 3.0f, 4.0f});
        expectQuadBounds("positive X merged extent", result, {1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {2.0f, 3.0f, 4.0f});
        expectQuadBounds("negative Y merged extent", result, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 4.0f});
        expectQuadBounds("positive Y merged extent", result, {0.0f, 1.0f, 0.0f}, {0.0f, 3.0f, 0.0f}, {2.0f, 3.0f, 4.0f});
        expectQuadBounds("negative Z merged extent", result, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, {2.0f, 3.0f, 0.0f});
        expectQuadBounds("positive Z merged extent", result, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 4.0f}, {2.0f, 3.0f, 4.0f});
        expectTangentBasis(result);
    }

    {
        std::vector<std::uint32_t> cells(27U, 1U);
        cells[13U] = 0U;
        const ve::GreedyMeshResult result = mesh({3, 3, 3}, cells);
        expectEqual("hollow center visible faces", result.visibleFaceCount, 60U);
        expectEqual("hollow center merged quads", result.mergedQuadCount, 12U);
        expectQuadBounds("cavity negative X", result, {-1.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f});
        expectQuadBounds("cavity positive X", result, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 2.0f, 2.0f});
        expectQuadBounds("cavity negative Y", result, {0.0f, -1.0f, 0.0f}, {1.0f, 2.0f, 1.0f}, {2.0f, 2.0f, 2.0f});
        expectQuadBounds("cavity positive Y", result, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {2.0f, 1.0f, 2.0f});
        expectQuadBounds("cavity negative Z", result, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 2.0f}, {2.0f, 2.0f, 2.0f});
        expectQuadBounds("cavity positive Z", result, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 1.0f});
    }

    {
        constexpr std::array<ve::Vec3, 6> normals{{
            {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
            {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}}};
        for (std::size_t face = 0; face < normals.size(); ++face) {
            ve::GreedyMeshingVolume volume{};
            volume.dimensions = {1, 1, 1};
            const std::vector<std::uint32_t> cells{1U};
            const std::vector<std::uint32_t> neighbor{2U};
            volume.cells = cells;
            volume.neighbors[face] = neighbor;
            const ve::GreedyMeshResult result = ve::generateGreedyMesh(volume);
            expectEqual("each neighbor face culls one face", result.visibleFaceCount, 5U);
            bool culledNormalPresent = false;
            for (const ve::Vertex& vertex : result.mesh.vertices) {
                culledNormalPresent |= ve::dot(vertex.normal, normals[face]) > 0.9999f;
            }
            expectTrue("each neighbor face removes its normal", !culledNormalPresent);
        }
    }

    {
        constexpr std::array<ve::Vec3, 6> normals{{
            {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
            {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}}};
        constexpr std::array<std::size_t, 6> boundaryIndices{{7U, 7U, 5U, 5U, 5U, 5U}};
        for (std::size_t face = 0; face < normals.size(); ++face) {
            ve::GreedyMeshingVolume volume{};
            volume.dimensions = {2, 3, 4};
            std::vector<std::uint32_t> cells(24U, 0U);
            const std::array<std::uint32_t, 3> coordinate{
                face < 2U ? (face == 0U ? 0U : 1U) : 1U,
                face < 4U ? (face == 2U ? 0U : face == 3U ? 2U : 1U) : 2U,
                face < 4U ? 2U : (face == 4U ? 0U : 3U)};
            const std::size_t cellIndex = static_cast<std::size_t>(coordinate[1]) +
                                          static_cast<std::size_t>(coordinate[0]) * 3U +
                                          static_cast<std::size_t>(coordinate[2]) * 6U;
            cells[cellIndex] = 1U;
            std::vector<std::uint32_t> neighbor(face < 2U ? 12U : face < 4U ? 8U : 6U, 0U);
            neighbor[boundaryIndices[face]] = 2U;
            volume.cells = cells;
            volume.neighbors[face] = neighbor;
            const ve::GreedyMeshResult result = ve::generateGreedyMesh(volume);
            expectEqual("asymmetric boundary plane culls matching cell", result.visibleFaceCount, 5U);
            bool culledNormalPresent = false;
            for (const ve::Vertex& vertex : result.mesh.vertices) {
                culledNormalPresent |= ve::dot(vertex.normal, normals[face]) > 0.9999f;
            }
            expectTrue("asymmetric boundary plane removes its normal", !culledNormalPresent);
        }
    }

    {
        ve::GreedyMeshingVolume volume{};
        volume.dimensions = {2, 3, 4};
        volume.origin = {10.0f, -2.0f, 3.5f};
        volume.cellSize = {2.0f, 0.5f, 3.0f};
        const std::vector<std::uint32_t> cells(24U, 1U);
        volume.cells = cells;
        const ve::GreedyMeshResult result = ve::generateGreedyMesh(volume);
        expectVec3Nearly("scaled origin bounds center", result.mesh.bounds.center, {12.0f, -1.25f, 9.5f});
        expectNearly("scaled origin bounds radius", result.mesh.bounds.radius,
                     std::sqrt(2.0f * 2.0f + 0.75f * 0.75f + 6.0f * 6.0f));
        expectQuadBounds("scaled positive Z extent", result, {0.0f, 0.0f, 1.0f},
                         {10.0f, -2.0f, 15.5f}, {14.0f, -0.5f, 15.5f});
    }

    {
        ve::GreedyMeshingVolume volume{};
        volume.dimensions = {2, 4, 2};
        std::vector<std::uint32_t> cells(16U, 0U);
        cells[0U] = 1U;
        cells[3U] = 2U;
        cells[1U + 1U * 4U + 1U * 8U] = 3U;
        volume.cells = cells;
        const ve::GreedyMeshResult scanned = ve::generateGreedyMesh(volume);
        const std::array<std::uint32_t, 4> minY{{0U, 4U, 4U, 1U}};
        const std::array<std::uint32_t, 4> maxY{{3U, 0U, 0U, 1U}};
        volume.minY = minY;
        volume.maxY = maxY;
        const ve::GreedyMeshResult cached = ve::generateGreedyMesh(volume);
        expectEquivalent("cached heightmap output", cached, scanned);
        expectRuntimeError("heightmap spans must be paired", [&] {
            volume.maxY = std::span<const std::uint32_t>{};
            (void)ve::generateGreedyMesh(volume);
        });
        volume.maxY = maxY;
        const std::array<std::uint32_t, 4> invalidMaxY{{4U, 1U, 0U, 0U}};
        volume.maxY = invalidMaxY;
        expectRuntimeError("heightmap upper bound is validated", [&] {
            (void)ve::generateGreedyMesh(volume);
        });
    }

    return gFailureCount == 0 ? 0 : 1;
}
