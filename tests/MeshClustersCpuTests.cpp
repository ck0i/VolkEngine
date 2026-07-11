#include "renderer/MeshClusters.hpp"

#include <cassert>
#include <stdexcept>

namespace {

template <typename F>
bool throwsRuntimeError(F&& function) {
    try {
        function();
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

} // namespace

int main() {
    ve::MeshData mesh = ve::createUvSphereMesh(16U, 32U);
    ve::optimizeTriangleIndexOrderForVertexCache(mesh.indices, mesh.vertices.size());
    ve::optimizeVertexFetchOrder(mesh);
    const ve::MeshClusterData clusters = ve::buildMeshClusters(mesh);
    ve::validateMeshClusters(mesh, clusters);

    assert(clusters.clusters.size() > 1U);
    assert(clusters.root < clusters.hierarchy.size());
    std::uint32_t nextIndex = 0U;
    for (const ve::MeshCluster& cluster : clusters.clusters) {
        assert(cluster.firstIndex == nextIndex);
        assert(cluster.indexCount / 3U <= ve::kMaximumClusterTriangles);
        assert(cluster.vertexCount <= ve::kMaximumClusterVertices);
        nextIndex += cluster.indexCount;
    }
    assert(nextIndex == mesh.indices.size());

    const ve::MeshClusterData repeated = ve::buildMeshClusters(mesh);
    assert(repeated.root == clusters.root);
    assert(repeated.clusters.size() == clusters.clusters.size());
    assert(repeated.hierarchy.size() == clusters.hierarchy.size());
    for (std::size_t index = 0; index < clusters.clusters.size(); ++index) {
        assert(repeated.clusters[index].firstIndex == clusters.clusters[index].firstIndex);
        assert(repeated.clusters[index].indexCount == clusters.clusters[index].indexCount);
        assert(repeated.clusters[index].vertexCount == clusters.clusters[index].vertexCount);
        assert(repeated.clusters[index].bounds.center.x ==
               clusters.clusters[index].bounds.center.x);
        assert(repeated.clusters[index].bounds.center.y ==
               clusters.clusters[index].bounds.center.y);
        assert(repeated.clusters[index].bounds.center.z ==
               clusters.clusters[index].bounds.center.z);
        assert(repeated.clusters[index].bounds.radius == clusters.clusters[index].bounds.radius);
    }

    ve::MeshClusterData corruptCluster = clusters;
    corruptCluster.clusters.front().bounds.radius = 0.0f;
    assert(throwsRuntimeError([&] { ve::validateMeshClusters(mesh, corruptCluster); }));

    ve::MeshClusterData corruptHierarchy = clusters;
    corruptHierarchy.hierarchy[corruptHierarchy.root].bounds.radius = 0.0f;
    assert(throwsRuntimeError([&] { ve::validateMeshClusters(mesh, corruptHierarchy); }));

    ve::MeshData invalid = mesh;
    invalid.indices.pop_back();
    assert(throwsRuntimeError([&] { static_cast<void>(ve::buildMeshClusters(invalid)); }));
    return 0;
}
