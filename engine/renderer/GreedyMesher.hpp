#pragma once

#include "renderer/Geometry.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ve {

// Cells are laid out with Y as the fastest-changing coordinate:
// index = y + x * height + z * width * height. A zero cell is empty;
// non-zero values identify occupied surfaces and their material.
struct GreedyMeshingVolume {
    std::array<std::uint32_t, 3> dimensions{}; // width (X), height (Y), depth (Z)
    std::span<const std::uint32_t> cells{};
    // Boundary planes in -X,+X,-Y,+Y,-Z,+Z order. Their layouts are:
    // YZ (height*depth), XZ (width*depth), and XY (width*height).
    std::array<std::span<const std::uint32_t>, 6> neighbors{};
    // Optional cached Y bounds per X/Z column (x + z*width), matching the
    // reference chunk heightmap optimization. Empty columns use minY=height,
    // maxY=0. Bounds must enclose every non-zero cell in the column; when
    // supplied, both spans must contain width*depth entries.
    std::span<const std::uint32_t> minY{};
    std::span<const std::uint32_t> maxY{};
    Vec3 origin{};
    Vec3 cellSize{1.0f, 1.0f, 1.0f};
    bool meshExterior = true;
    bool emitBoundaryFaces = false;
};

struct GreedyMeshMaterialRange {
    std::uint32_t material = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
};

struct GreedyMeshResult {
    MeshData mesh;
    // Ranges are contiguous draw spans; a material can occur in multiple
    // ranges when the directional sweep encounters it non-contiguously.
    std::vector<GreedyMeshMaterialRange> materialRanges;
    std::uint64_t visibleFaceCount = 0;
    std::uint64_t mergedQuadCount = 0;
};

// Builds indexed axis-aligned quads with the reference run-merging strategy:
// extend each visible face along a primary axis, then merge identical rows
// along a secondary axis. Faces merge only when occupancy/material and
// visibility match, preserving material boundaries and finite-grid seams.
[[nodiscard]] GreedyMeshResult generateGreedyMesh(const GreedyMeshingVolume& volume);

} // namespace ve
