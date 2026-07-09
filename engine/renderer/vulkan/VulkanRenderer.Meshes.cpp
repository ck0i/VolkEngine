#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

void VulkanRenderer::Impl::destroyMeshUpload(MeshUpload& upload) {
    destroyBuffer(upload.indexStaging);
    destroyBuffer(upload.vertexStaging);
    destroyBuffer(upload.indices);
    destroyBuffer(upload.vertices);
    upload.cube = {};
    upload.sphere = {};
    upload.plane = {};
}

void VulkanRenderer::Impl::recordMeshUpload(VkCommandBuffer commandBuffer, const MeshUpload& upload) const {
    VkBufferCopy vertexCopy{};
    vertexCopy.size = upload.vertexSize;
    vkCmdCopyBuffer(commandBuffer, upload.vertexStaging.buffer, upload.vertices.buffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{};
    indexCopy.size = upload.indexSize;
    vkCmdCopyBuffer(commandBuffer, upload.indexStaging.buffer, upload.indices.buffer, 1, &indexCopy);

    if (transferQueue_ == graphicsQueue_) {
        std::array<VkBufferMemoryBarrier2, 2> barriers{};
        barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].buffer = upload.vertices.buffer;
        barriers[0].offset = 0;
        barriers[0].size = upload.vertexSize;

        barriers[1] = barriers[0];
        barriers[1].buffer = upload.indices.buffer;
        barriers[1].size = upload.indexSize;
        barriers[1].dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;

        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
        dependency.pBufferMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }
}

const VulkanRenderer::Impl::GpuMesh& VulkanRenderer::Impl::meshFor(const SceneMeshId mesh) const {
    switch (mesh) {
    case SceneMeshId::Cube:
        return cube_;
    case SceneMeshId::Sphere:
        return sphere_;
    case SceneMeshId::GroundPlane:
        return plane_;
    }
    throw std::runtime_error("Unknown scene mesh id");
}

const VulkanRenderer::Impl::GpuMesh& VulkanRenderer::Impl::meshForBatch(const std::size_t meshIndex) const {
    if (meshIndex >= kSceneMeshBatchOrder.size()) {
        throw std::runtime_error("Scene mesh batch index out of range");
    }
    switch (kSceneMeshBatchOrder[meshIndex]) {
    case SceneMeshBatchId::Cube:
        return cube_;
    case SceneMeshBatchId::SphereHigh:
        return sphere_;
    case SceneMeshBatchId::SphereMedium:
        return sphereMedium_;
    case SceneMeshBatchId::SphereLow:
        return sphereLow_;
    case SceneMeshBatchId::GroundPlane:
        return plane_;
    }
    throw std::runtime_error("Unknown scene mesh batch id");
}

VulkanRenderer::Impl::MeshUpload VulkanRenderer::Impl::stageMeshUpload(const MeshData& cubeMesh, const MeshData& sphereMesh, const MeshData& sphereMediumMesh, const MeshData& sphereLowMesh, const MeshData& planeMesh) {
    MeshUpload upload{};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(cubeMesh.vertices.size() + sphereMesh.vertices.size() + sphereMediumMesh.vertices.size() + sphereLowMesh.vertices.size() + planeMesh.vertices.size());
    indices.reserve(cubeMesh.indices.size() + sphereMesh.indices.size() + sphereMediumMesh.indices.size() + sphereLowMesh.indices.size() + planeMesh.indices.size());

    const auto appendMesh = [&](const MeshData& mesh) -> GpuMesh {
        if (mesh.vertices.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::runtime_error("Scene mesh vertex offset exceeds VkDrawIndexed vertexOffset range");
        }
        if (mesh.indices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::runtime_error("Scene mesh index count exceeds uint32 range");
        }
        if (indices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - mesh.indices.size()) {
            throw std::runtime_error("Scene geometry index buffer exceeds uint32 range");
        }
        const std::size_t firstVertex = vertices.size();
        if (firstVertex > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::runtime_error("Scene mesh vertex offset exceeds VkDrawIndexed vertexOffset range");
        }
        const std::size_t firstIndex = indices.size();
        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
        return GpuMesh{
            static_cast<std::uint32_t>(mesh.indices.size()),
            static_cast<std::uint32_t>(firstIndex),
            static_cast<std::int32_t>(firstVertex),
        };
    };

    upload.cube = appendMesh(cubeMesh);
    upload.sphere = appendMesh(sphereMesh);
    upload.sphereMedium = appendMesh(sphereMediumMesh);
    upload.sphereLow = appendMesh(sphereLowMesh);
    upload.plane = appendMesh(planeMesh);
    upload.vertexSize = static_cast<VkDeviceSize>(vertices.size() * sizeof(Vertex));
    upload.indexSize = static_cast<VkDeviceSize>(indices.size() * sizeof(std::uint32_t));

    try {
        upload.vertexStaging = createBuffer(upload.vertexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        {
            ScopedVmaMap vertexMap{allocator_, upload.vertexStaging.allocation, "vmaMapMemory vertex staging"};
            std::memcpy(vertexMap.get(), vertices.data(), static_cast<std::size_t>(upload.vertexSize));
        }

        upload.indexStaging = createBuffer(upload.indexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        {
            ScopedVmaMap indexMap{allocator_, upload.indexStaging.allocation, "vmaMapMemory index staging"};
            std::memcpy(indexMap.get(), indices.data(), static_cast<std::size_t>(upload.indexSize));
        }

        upload.vertices = createBuffer(upload.vertexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        upload.indices = createBuffer(upload.indexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        return upload;
    } catch (...) {
        destroyMeshUpload(upload);
        throw;
    }
}

void VulkanRenderer::Impl::createMeshes() {
    std::vector<Buffer> stagingBuffers;
    stagingBuffers.reserve(2);
    MeshUpload meshUpload{};
    try {
        meshUpload = stageMeshUpload(createCubeMesh(), createUvSphereMesh(32, 64), createUvSphereMesh(16, 32), createUvSphereMesh(8, 16), createPlaneMesh(12.0f, 12.0f));

        VkCommandBuffer uploadCommands = beginUploadCommands();
        recordMeshUpload(uploadCommands, meshUpload);
        stagingBuffers.push_back(meshUpload.indexStaging);
        meshUpload.indexStaging = {};
        stagingBuffers.push_back(meshUpload.vertexStaging);
        meshUpload.vertexStaging = {};
        submitTransferUpload(uploadCommands, std::move(stagingBuffers));
    } catch (...) {
        destroyMeshUpload(meshUpload);
        throw;
    }

    sceneVertexBuffer_ = meshUpload.vertices;
    sceneIndexBuffer_ = meshUpload.indices;
    cube_ = meshUpload.cube;
    sphere_ = meshUpload.sphere;
    sphereMedium_ = meshUpload.sphereMedium;
    sphereLow_ = meshUpload.sphereLow;
    plane_ = meshUpload.plane;
    sceneMeshTriangleCounts_[sceneMeshBatchIndex(SceneMeshBatchId::Cube)] = cube_.indexCount / 3U;
    sceneMeshTriangleCounts_[sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh)] = sphere_.indexCount / 3U;
    sceneMeshTriangleCounts_[sceneMeshBatchIndex(SceneMeshBatchId::SphereMedium)] = sphereMedium_.indexCount / 3U;
    sceneMeshTriangleCounts_[sceneMeshBatchIndex(SceneMeshBatchId::SphereLow)] = sphereLow_.indexCount / 3U;
    sceneMeshTriangleCounts_[sceneMeshBatchIndex(SceneMeshBatchId::GroundPlane)] = plane_.indexCount / 3U;
    meshUpload.vertices = {};
    meshUpload.indices = {};
    meshUpload.cube = {};
    meshUpload.sphere = {};
    meshUpload.plane = {};

    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(sceneVertexBuffer_.buffer), "Scene Geometry Vertex Buffer");
    vmaSetAllocationName(allocator_, sceneVertexBuffer_.allocation, "Scene Geometry Vertex Allocation");
    sceneVertexBuffer_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, "Scene Geometry Vertex Buffer", sceneVertexBuffer_.size);
    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(sceneIndexBuffer_.buffer), "Scene Geometry Index Buffer");
    vmaSetAllocationName(allocator_, sceneIndexBuffer_.allocation, "Scene Geometry Index Allocation");
    sceneIndexBuffer_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, "Scene Geometry Index Buffer", sceneIndexBuffer_.size);
}

} // namespace ve
