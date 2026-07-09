#include "renderer/GreedyMesher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace ve {
namespace {

enum class Face : std::uint8_t {
    NegativeX,
    PositiveX,
    NegativeY,
    PositiveY,
    NegativeZ,
    PositiveZ,
};

struct FaceSpec {
    Face face;
    std::uint8_t primaryAxis;
    std::uint8_t secondaryAxis;
    Vec3 normal;
    Vec3 tangent;
    float handedness;
};

constexpr std::array<FaceSpec, 6> kFaces{{
    {Face::NegativeX, 1, 2, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, -1.0f},
    {Face::PositiveX, 1, 2, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, -1.0f},
    {Face::NegativeZ, 1, 0, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, -1.0f},
    {Face::PositiveZ, 1, 0, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, -1.0f},
    {Face::NegativeY, 0, 2, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, -1.0f},
    {Face::PositiveY, 0, 2, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, -1.0f},
}};

[[nodiscard]] std::size_t checkedCellCount(const std::array<std::uint32_t, 3>& dimensions) {
    const std::size_t width = dimensions[0];
    const std::size_t height = dimensions[1];
    const std::size_t depth = dimensions[2];
    if (width == 0U || height == 0U || depth == 0U) {
        throw std::runtime_error("Greedy meshing dimensions must be non-zero");
    }
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        throw std::runtime_error("Greedy meshing dimensions overflow the host size range");
    }
    const std::size_t area = width * height;
    if (area > std::numeric_limits<std::size_t>::max() / depth) {
        throw std::runtime_error("Greedy meshing dimensions overflow the host size range");
    }
    return area * depth;
}

[[nodiscard]] std::size_t linearIndex(const std::array<std::uint32_t, 3>& dimensions,
                                      const std::array<std::uint32_t, 3>& coordinate) noexcept {
    return static_cast<std::size_t>(coordinate[1]) +
           static_cast<std::size_t>(coordinate[0]) * dimensions[1] +
           static_cast<std::size_t>(coordinate[2]) * dimensions[0] * dimensions[1];
}

[[nodiscard]] Vec3 cellPosition(const Vec3 origin, const Vec3 cellSize, const std::array<std::uint32_t, 3>& coordinate) noexcept {
    return origin + Vec3{static_cast<float>(coordinate[0]) * cellSize.x,
                         static_cast<float>(coordinate[1]) * cellSize.y,
                         static_cast<float>(coordinate[2]) * cellSize.z};
}

[[nodiscard]] bool finiteVec3(const Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

GreedyMeshResult generateGreedyMesh(const GreedyMeshingVolume& volume) {
    const std::size_t cellCount = checkedCellCount(volume.dimensions);
    if (volume.cells.size() != cellCount) {
        throw std::runtime_error("Greedy meshing cell data size does not match dimensions");
    }
    const std::size_t width = volume.dimensions[0];
    const std::size_t height = volume.dimensions[1];
    const std::size_t depth = volume.dimensions[2];
    if (!finiteVec3(volume.origin) || !finiteVec3(volume.cellSize) ||
        volume.cellSize.x <= 0.0f || volume.cellSize.y <= 0.0f || volume.cellSize.z <= 0.0f) {
        throw std::runtime_error("Greedy meshing origin and cell size must be finite; cell size must be positive");
    }
    const std::size_t columnCount = width * depth;
    const bool hasHeightMap = !volume.minY.empty() || !volume.maxY.empty();
    if (hasHeightMap && (volume.minY.size() != columnCount || volume.maxY.size() != columnCount)) {
        throw std::runtime_error("Greedy meshing heightmap size does not match X/Z columns");
    }
    if (hasHeightMap) {
        for (std::size_t column = 0; column < columnCount; ++column) {
            const std::uint32_t minimum = volume.minY[column];
            const std::uint32_t maximum = volume.maxY[column];
            const bool emptyColumn = minimum == height;
            if (minimum > height || (emptyColumn ? maximum != 0U : minimum > maximum || maximum >= height)) {
                throw std::runtime_error("Greedy meshing heightmap bounds are invalid");
            }
        }
    }
    const std::array<std::size_t, 6> neighborCellCounts{
        height * depth, height * depth, width * depth, width * depth, width * height, width * height};
    for (std::size_t faceIndex = 0; faceIndex < volume.neighbors.size(); ++faceIndex) {
        const std::span<const std::uint32_t> neighbor = volume.neighbors[faceIndex];
        if (!neighbor.empty() && neighbor.size() != neighborCellCounts[faceIndex]) {
            throw std::runtime_error("Greedy meshing boundary data size does not match its plane dimensions");
        }
    }

    GreedyMeshResult result{};
    const std::size_t reserveCells = std::min(cellCount, static_cast<std::size_t>(1024U));
    result.mesh.vertices.reserve(reserveCells * 4U);
    result.mesh.indices.reserve(reserveCells * 6U);
    result.materialRanges.reserve(std::min(cellCount, static_cast<std::size_t>(64U)));
    const auto cellAt = [&](const std::array<std::uint32_t, 3>& coordinate) {
        return volume.cells[linearIndex(volume.dimensions, coordinate)];
    };
    const auto faceVisible = [&](const std::array<std::uint32_t, 3>& coordinate, const Face face) {
        const std::size_t faceIndex = static_cast<std::size_t>(face);
        const std::uint8_t axis = static_cast<std::uint8_t>(faceIndex / 2U);
        const bool positive = (faceIndex & 1U) != 0U;
        const std::uint32_t axisLimit = volume.dimensions[axis];
        const bool inside = positive ? coordinate[axis] + 1U < axisLimit : coordinate[axis] > 0U;
        if (inside) {
            std::array<std::uint32_t, 3> adjacent = coordinate;
            adjacent[axis] = positive ? adjacent[axis] + 1U : adjacent[axis] - 1U;
            return cellAt(adjacent) == 0U;
        }
        if (volume.emitBoundaryFaces) {
            return true;
        }
        const std::span<const std::uint32_t> neighbor = volume.neighbors[faceIndex];
        if (neighbor.empty()) {
            return volume.meshExterior;
        }
        std::size_t boundaryIndex = 0U;
        if (axis == 0U) {
            boundaryIndex = static_cast<std::size_t>(coordinate[1]) +
                            static_cast<std::size_t>(coordinate[2]) * height;
        } else if (axis == 1U) {
            boundaryIndex = static_cast<std::size_t>(coordinate[0]) +
                            static_cast<std::size_t>(coordinate[2]) * width;
        } else {
            boundaryIndex = static_cast<std::size_t>(coordinate[1]) +
                            static_cast<std::size_t>(coordinate[0]) * height;
        }
        return neighbor[boundaryIndex] == 0U;
    };

    std::array<std::vector<std::uint32_t>, 6> visited;
    for (auto& faceVisited : visited) {
        faceVisited.assign(cellCount, 0U);
    }
    constexpr std::uint32_t comparison = 1U;

    const auto appendMaterialRange = [&](const std::uint32_t material, const std::size_t firstIndex) {
        const auto first = static_cast<std::uint32_t>(firstIndex);
        constexpr std::size_t maxIndexCount = std::numeric_limits<std::uint32_t>::max();
        if (result.materialRanges.empty() || result.materialRanges.back().material != material ||
            static_cast<std::size_t>(result.materialRanges.back().firstIndex) + result.materialRanges.back().indexCount != firstIndex) {
            result.materialRanges.push_back({material, first, 6U});
        } else {
            if (static_cast<std::size_t>(result.materialRanges.back().indexCount) + 6U > maxIndexCount) {
                throw std::runtime_error("Greedy meshing material range exceeds uint32 range");
            }
            result.materialRanges.back().indexCount += 6U;
        }
    };

    const auto emitQuad = [&](const FaceSpec& spec,
                              const std::array<std::uint32_t, 3>& coordinate,
                              const std::uint32_t lengthA,
                              const std::uint32_t lengthB,
                              const std::uint32_t material) {
        const std::uint32_t x = coordinate[0];
        const std::uint32_t y = coordinate[1];
        const std::uint32_t z = coordinate[2];
        std::array<Vec3, 4> positions{};
        switch (spec.face) {
        case Face::NegativeX:
            positions = {cellPosition(volume.origin, volume.cellSize, {x, y, z}),
                         cellPosition(volume.origin, volume.cellSize, {x, y, z + lengthB}),
                         cellPosition(volume.origin, volume.cellSize, {x, y + lengthA, z}),
                         cellPosition(volume.origin, volume.cellSize, {x, y + lengthA, z + lengthB})};
            break;
        case Face::PositiveX:
            positions = {cellPosition(volume.origin, volume.cellSize, {x + 1U, y, z}),
                         cellPosition(volume.origin, volume.cellSize, {x + 1U, y + lengthA, z}),
                         cellPosition(volume.origin, volume.cellSize, {x + 1U, y, z + lengthB}),
                         cellPosition(volume.origin, volume.cellSize, {x + 1U, y + lengthA, z + lengthB})};
            break;
        case Face::NegativeY:
            positions = {cellPosition(volume.origin, volume.cellSize, {x, y, z}),
                         cellPosition(volume.origin, volume.cellSize, {x + lengthA, y, z}),
                         cellPosition(volume.origin, volume.cellSize, {x, y, z + lengthB}),
                         cellPosition(volume.origin, volume.cellSize, {x + lengthA, y, z + lengthB})};
            break;
        case Face::PositiveY:
            positions = {cellPosition(volume.origin, volume.cellSize, {x, y + 1U, z}),
                         cellPosition(volume.origin, volume.cellSize, {x, y + 1U, z + lengthB}),
                         cellPosition(volume.origin, volume.cellSize, {x + lengthA, y + 1U, z}),
                         cellPosition(volume.origin, volume.cellSize, {x + lengthA, y + 1U, z + lengthB})};
            break;
        case Face::NegativeZ:
            positions = {cellPosition(volume.origin, volume.cellSize, {x, y, z}),
                         cellPosition(volume.origin, volume.cellSize, {x, y + lengthA, z}),
                         cellPosition(volume.origin, volume.cellSize, {x + lengthB, y, z}),
                         cellPosition(volume.origin, volume.cellSize, {x + lengthB, y + lengthA, z})};
            break;
        case Face::PositiveZ:
            positions = {cellPosition(volume.origin, volume.cellSize, {x, y, z + 1U}),
                         cellPosition(volume.origin, volume.cellSize, {x + lengthB, y, z + 1U}),
                         cellPosition(volume.origin, volume.cellSize, {x, y + lengthA, z + 1U}),
                         cellPosition(volume.origin, volume.cellSize, {x + lengthB, y + lengthA, z + 1U})};
            break;
        }
        if (result.mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max() - 4U ||
            result.mesh.indices.size() > std::numeric_limits<std::uint32_t>::max() - 6U) {
            throw std::runtime_error("Greedy meshing output exceeds uint32 index range");
        }
        const std::uint32_t firstVertex = static_cast<std::uint32_t>(result.mesh.vertices.size());
        result.mesh.vertices.push_back({positions[0], spec.normal, {0.0f, 0.0f}, {spec.tangent.x, spec.tangent.y, spec.tangent.z, spec.handedness}});
        result.mesh.vertices.push_back({positions[1], spec.normal, {0.0f, 1.0f}, {spec.tangent.x, spec.tangent.y, spec.tangent.z, spec.handedness}});
        result.mesh.vertices.push_back({positions[2], spec.normal, {1.0f, 0.0f}, {spec.tangent.x, spec.tangent.y, spec.tangent.z, spec.handedness}});
        result.mesh.vertices.push_back({positions[3], spec.normal, {1.0f, 1.0f}, {spec.tangent.x, spec.tangent.y, spec.tangent.z, spec.handedness}});
        const std::size_t firstIndex = result.mesh.indices.size();
        result.mesh.indices.insert(result.mesh.indices.end(), {firstVertex, firstVertex + 1U, firstVertex + 2U,
                                                               firstVertex + 2U, firstVertex + 1U, firstVertex + 3U});
        appendMaterialRange(material, firstIndex);
        ++result.mergedQuadCount;
    };

    std::vector<std::uint32_t> generatedMinY;
    std::vector<std::uint32_t> generatedMaxY;
    std::span<const std::uint32_t> minY = volume.minY;
    std::span<const std::uint32_t> maxY = volume.maxY;
    if (!hasHeightMap) {
        generatedMinY.assign(columnCount, static_cast<std::uint32_t>(height));
        generatedMaxY.assign(columnCount, 0U);
        for (std::uint32_t z = 0; z < volume.dimensions[2]; ++z) {
            for (std::uint32_t x = 0; x < volume.dimensions[0]; ++x) {
                const std::size_t column = static_cast<std::size_t>(x) + static_cast<std::size_t>(z) * width;
                for (std::uint32_t y = 0; y < volume.dimensions[1]; ++y) {
                    if (cellAt({x, y, z}) != 0U) {
                        generatedMinY[column] = std::min(generatedMinY[column], y);
                        generatedMaxY[column] = y;
                    }
                }
            }
        }
        minY = generatedMinY;
        maxY = generatedMaxY;
    }

    for (std::uint32_t z = 0; z < volume.dimensions[2]; ++z) {
        for (std::uint32_t x = 0; x < volume.dimensions[0]; ++x) {
            const std::size_t column = static_cast<std::size_t>(x) + static_cast<std::size_t>(z) * width;
            for (std::uint32_t y = minY[column]; y <= maxY[column]; ++y) {
                const std::array<std::uint32_t, 3> coordinate{x, y, z};
                const std::uint32_t material = cellAt(coordinate);
                if (material == 0U) {
                    continue;
                }
                for (const FaceSpec& spec : kFaces) {
                    const std::size_t faceIndex = static_cast<std::size_t>(spec.face);
                    if (!faceVisible(coordinate, spec.face)) {
                        continue;
                    }
                    ++result.visibleFaceCount;
                    const std::size_t start = linearIndex(volume.dimensions, coordinate);
                    if (visited[faceIndex][start] == comparison) {
                        continue;
                    }
                    const std::uint8_t primaryAxis = spec.primaryAxis;
                    const std::uint8_t secondaryAxis = spec.secondaryAxis;
                    std::uint32_t lengthA = 1U;
                    while (coordinate[primaryAxis] + lengthA < volume.dimensions[primaryAxis]) {
                        auto candidate = coordinate;
                        candidate[primaryAxis] += lengthA;
                        const std::size_t candidateIndex = linearIndex(volume.dimensions, candidate);
                        if (cellAt(candidate) != material || visited[faceIndex][candidateIndex] == comparison ||
                            !faceVisible(candidate, spec.face)) {
                            break;
                        }
                        ++lengthA;
                    }
                    for (std::uint32_t offset = 0; offset < lengthA; ++offset) {
                        auto marked = coordinate;
                        marked[primaryAxis] += offset;
                        visited[faceIndex][linearIndex(volume.dimensions, marked)] = comparison;
                    }

                    std::uint32_t lengthB = 1U;
                    while (coordinate[secondaryAxis] + lengthB < volume.dimensions[secondaryAxis]) {
                        bool identicalRow = true;
                        for (std::uint32_t offset = 0; offset < lengthA; ++offset) {
                            auto candidate = coordinate;
                            candidate[secondaryAxis] += lengthB;
                            candidate[primaryAxis] += offset;
                            const std::size_t candidateIndex = linearIndex(volume.dimensions, candidate);
                            if (cellAt(candidate) != material || visited[faceIndex][candidateIndex] == comparison ||
                                !faceVisible(candidate, spec.face)) {
                                identicalRow = false;
                                break;
                            }
                        }
                        if (!identicalRow) {
                            break;
                        }
                        for (std::uint32_t offset = 0; offset < lengthA; ++offset) {
                            auto marked = coordinate;
                            marked[secondaryAxis] += lengthB;
                            marked[primaryAxis] += offset;
                            visited[faceIndex][linearIndex(volume.dimensions, marked)] = comparison;
                        }
                        ++lengthB;
                    }
                    emitQuad(spec, coordinate, lengthA, lengthB, material);
                }
            }
        }
    }

    if (!result.mesh.vertices.empty()) {
        Vec3 minimum = result.mesh.vertices.front().position;
        Vec3 maximum = minimum;
        for (const Vertex& vertex : result.mesh.vertices) {
            minimum.x = std::min(minimum.x, vertex.position.x);
            minimum.y = std::min(minimum.y, vertex.position.y);
            minimum.z = std::min(minimum.z, vertex.position.z);
            maximum.x = std::max(maximum.x, vertex.position.x);
            maximum.y = std::max(maximum.y, vertex.position.y);
            maximum.z = std::max(maximum.z, vertex.position.z);
        }
        result.mesh.bounds.center = (minimum + maximum) * 0.5f;
        for (const Vertex& vertex : result.mesh.vertices) {
            result.mesh.bounds.radius = std::max(result.mesh.bounds.radius, length(vertex.position - result.mesh.bounds.center));
        }
        result.mesh.bounds.valid = true;
    }
    return result;
}

} // namespace ve
