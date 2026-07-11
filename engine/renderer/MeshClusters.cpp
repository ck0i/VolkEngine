#include "renderer/MeshClusters.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ve {
namespace {

MeshBounds boundsForVertices(const MeshData& mesh, const std::vector<std::uint32_t>& vertices) {
    if (vertices.empty()) throw std::runtime_error("Mesh cluster has no vertices");
    Vec3 minimum = mesh.vertices[vertices.front()].position;
    Vec3 maximum = minimum;
    for (const std::uint32_t index : vertices) {
        if (index >= mesh.vertices.size()) throw std::runtime_error("Mesh cluster index is out of range");
        const Vec3 position = mesh.vertices[index].position;
        minimum.x = std::min(minimum.x, position.x);
        minimum.y = std::min(minimum.y, position.y);
        minimum.z = std::min(minimum.z, position.z);
        maximum.x = std::max(maximum.x, position.x);
        maximum.y = std::max(maximum.y, position.y);
        maximum.z = std::max(maximum.z, position.z);
    }
    const Vec3 center = (minimum + maximum) * 0.5f;
    float radius = 0.0f;
    for (const std::uint32_t index : vertices) {
        radius = std::max(radius, length(mesh.vertices[index].position - center));
    }
    if (!finite(center) || !std::isfinite(radius)) {
        throw std::runtime_error("Mesh cluster bounds are non-finite");
    }
    return {center, radius, true};
}

MeshBounds combineBounds(const MeshBounds& left, const MeshBounds& right) {
    if (!left.valid || !right.valid) throw std::runtime_error("Mesh cluster hierarchy bounds are invalid");
    const Vec3 delta = right.center - left.center;
    const float distance = length(delta);
    if (left.radius >= distance + right.radius) return left;
    if (right.radius >= distance + left.radius) return right;
    const float radius = (distance + left.radius + right.radius) * 0.5f;
    const Vec3 center = distance > 0.000001f
        ? left.center + delta * ((radius - left.radius) / distance)
        : left.center;
    return {center, radius, true};
}

bool containsPoint(const MeshBounds& bounds, const Vec3 point) {
    const float epsilon = std::max(0.0001f, bounds.radius * 0.0001f);
    return length(point - bounds.center) <= bounds.radius + epsilon;
}

bool containsBounds(const MeshBounds& parent, const MeshBounds& child) {
    const float epsilon = std::max(0.0001f, parent.radius * 0.0001f);
    return length(child.center - parent.center) + child.radius <=
           parent.radius + epsilon;
}

} // namespace

MeshClusterData buildMeshClusters(const MeshData& mesh) {
    if (mesh.indices.empty()) return {};
    if (mesh.indices.size() % 3U != 0U) {
        throw std::runtime_error("Mesh cluster input is not a triangle list");
    }
    if (mesh.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Mesh cluster index range exceeds uint32");
    }

    MeshClusterData result;
    result.clusters.reserve(
        (mesh.indices.size() / 3U + kMaximumClusterTriangles - 1U) /
        kMaximumClusterTriangles);
    std::vector<std::uint32_t> uniqueVertices;
    uniqueVertices.reserve(kMaximumClusterVertices);
    std::uint32_t clusterFirstIndex = 0U;
    std::uint32_t clusterIndexCount = 0U;

    const auto finishCluster = [&] {
        if (clusterIndexCount == 0U) return;
        result.clusters.push_back({
            clusterFirstIndex, clusterIndexCount,
            static_cast<std::uint32_t>(uniqueVertices.size()),
            boundsForVertices(mesh, uniqueVertices)});
        clusterFirstIndex += clusterIndexCount;
        clusterIndexCount = 0U;
        uniqueVertices.clear();
    };

    for (std::size_t first = 0; first < mesh.indices.size(); first += 3U) {
        std::uint32_t additionalVertices = 0U;
        for (std::size_t corner = 0; corner < 3U; ++corner) {
            const std::uint32_t index = mesh.indices[first + corner];
            if (index >= mesh.vertices.size()) {
                throw std::runtime_error("Mesh cluster input index is out of range");
            }
            if (std::ranges::find(uniqueVertices, index) == uniqueVertices.end()) {
                bool duplicatedInTriangle = false;
                for (std::size_t prior = 0; prior < corner; ++prior) {
                    duplicatedInTriangle |= mesh.indices[first + prior] == index;
                }
                if (!duplicatedInTriangle) ++additionalVertices;
            }
        }
        if (clusterIndexCount != 0U &&
            (clusterIndexCount / 3U >= kMaximumClusterTriangles ||
             uniqueVertices.size() + additionalVertices > kMaximumClusterVertices)) {
            finishCluster();
        }
        for (std::size_t corner = 0; corner < 3U; ++corner) {
            const std::uint32_t index = mesh.indices[first + corner];
            if (std::ranges::find(uniqueVertices, index) == uniqueVertices.end()) {
                uniqueVertices.push_back(index);
            }
        }
        clusterIndexCount += 3U;
    }
    finishCluster();

    result.hierarchy.reserve(result.clusters.size() * 2U - 1U);
    std::vector<std::uint32_t> level;
    level.reserve(result.clusters.size());
    for (std::uint32_t cluster = 0; cluster < result.clusters.size(); ++cluster) {
        level.push_back(static_cast<std::uint32_t>(result.hierarchy.size()));
        result.hierarchy.push_back({result.clusters[cluster].bounds,
                                    kInvalidClusterNode, kInvalidClusterNode, cluster});
    }
    while (level.size() > 1U) {
        std::vector<std::uint32_t> next;
        next.reserve((level.size() + 1U) / 2U);
        for (std::size_t index = 0; index < level.size(); index += 2U) {
            if (index + 1U == level.size()) {
                next.push_back(level[index]);
                continue;
            }
            const std::uint32_t left = level[index];
            const std::uint32_t right = level[index + 1U];
            const std::uint32_t parent = static_cast<std::uint32_t>(result.hierarchy.size());
            result.hierarchy.push_back({
                combineBounds(result.hierarchy[left].bounds, result.hierarchy[right].bounds),
                left, right, kInvalidClusterNode});
            next.push_back(parent);
        }
        level = std::move(next);
    }
    result.root = level.front();
    validateMeshClusters(mesh, result);
    return result;
}

void validateMeshClusters(const MeshData& mesh, const MeshClusterData& clusters) {
    if (mesh.indices.empty()) {
        if (!clusters.clusters.empty() || !clusters.hierarchy.empty() ||
            clusters.root != kInvalidClusterNode) {
            throw std::runtime_error("Empty mesh has non-empty cluster metadata");
        }
        return;
    }
    if (clusters.clusters.empty() || clusters.hierarchy.empty() ||
        clusters.root >= clusters.hierarchy.size() ||
        clusters.hierarchy.size() > clusters.clusters.size() * 2U - 1U) {
        throw std::runtime_error("Mesh cluster hierarchy metadata is invalid");
    }
    std::uint64_t nextIndex = 0U;
    for (const MeshCluster& cluster : clusters.clusters) {
        if (cluster.firstIndex != nextIndex || cluster.indexCount == 0U ||
            cluster.indexCount % 3U != 0U ||
            cluster.indexCount / 3U > kMaximumClusterTriangles ||
            cluster.vertexCount == 0U || cluster.vertexCount > kMaximumClusterVertices ||
            !cluster.bounds.valid || !finite(cluster.bounds.center) ||
            !std::isfinite(cluster.bounds.radius) || cluster.bounds.radius < 0.0f ||
            cluster.indexCount > mesh.indices.size() - nextIndex) {
            throw std::runtime_error("Mesh cluster metadata violates bounds");
        }
        std::vector<std::uint32_t> uniqueVertices;
        uniqueVertices.reserve(cluster.vertexCount);
        for (std::uint64_t offset = 0; offset < cluster.indexCount; ++offset) {
            const std::uint32_t vertex = mesh.indices[
                static_cast<std::size_t>(nextIndex + offset)];
            if (vertex >= mesh.vertices.size() ||
                !containsPoint(cluster.bounds, mesh.vertices[vertex].position)) {
                throw std::runtime_error(
                    "Mesh cluster bounds do not contain referenced geometry");
            }
            if (std::ranges::find(uniqueVertices, vertex) == uniqueVertices.end()) {
                uniqueVertices.push_back(vertex);
            }
        }
        if (uniqueVertices.size() != cluster.vertexCount) {
            throw std::runtime_error("Mesh cluster vertex count is inconsistent");
        }
        nextIndex += cluster.indexCount;
    }
    if (nextIndex != mesh.indices.size()) {
        throw std::runtime_error("Mesh clusters do not cover the mesh index stream");
    }

    std::vector<bool> visitedNodes(clusters.hierarchy.size(), false);
    std::vector<bool> visitedClusters(clusters.clusters.size(), false);
    std::vector<std::uint32_t> pending{clusters.root};
    while (!pending.empty()) {
        const std::uint32_t nodeIndex = pending.back();
        pending.pop_back();
        if (nodeIndex >= clusters.hierarchy.size() || visitedNodes[nodeIndex]) {
            throw std::runtime_error("Mesh cluster hierarchy contains a cycle or invalid child");
        }
        visitedNodes[nodeIndex] = true;
        const MeshClusterNode& node = clusters.hierarchy[nodeIndex];
        if (!node.bounds.valid || !finite(node.bounds.center) ||
            !std::isfinite(node.bounds.radius) || node.bounds.radius < 0.0f) {
            throw std::runtime_error("Mesh cluster hierarchy bounds are invalid");
        }
        const bool leaf = node.cluster != kInvalidClusterNode;
        if (leaf) {
            if (node.left != kInvalidClusterNode || node.right != kInvalidClusterNode ||
                node.cluster >= clusters.clusters.size() || visitedClusters[node.cluster]) {
                throw std::runtime_error("Mesh cluster hierarchy leaf is invalid");
            }
            visitedClusters[node.cluster] = true;
            if (!containsBounds(node.bounds,
                                clusters.clusters[node.cluster].bounds)) {
                throw std::runtime_error(
                    "Mesh cluster hierarchy leaf does not contain its cluster");
            }
        } else {
            if (node.left == kInvalidClusterNode || node.right == kInvalidClusterNode ||
                node.left >= clusters.hierarchy.size() ||
                node.right >= clusters.hierarchy.size() ||
                !containsBounds(node.bounds, clusters.hierarchy[node.left].bounds) ||
                !containsBounds(node.bounds, clusters.hierarchy[node.right].bounds)) {
                throw std::runtime_error(
                    "Mesh cluster hierarchy internal node is incomplete or non-containing");
            }
            pending.push_back(node.left);
            pending.push_back(node.right);
        }
    }
    if (!std::ranges::all_of(visitedClusters, [](const bool visited) { return visited; })) {
        throw std::runtime_error("Mesh cluster hierarchy does not reference every cluster");
    }
}

} // namespace ve
