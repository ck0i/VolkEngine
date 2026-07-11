#include "renderer/vulkan/VulkanRendererImpl.hpp"

#include <cmath>
#include <cstring>

namespace ve {
namespace {

[[nodiscard]] std::int16_t packSnorm16(const float value) {
    return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

[[nodiscard]] GpuVertex packVertex(const Vertex& vertex) {
    return GpuVertex{{vertex.position.x, vertex.position.y, vertex.position.z},
                     {vertex.uv.x, vertex.uv.y},
                     {packSnorm16(vertex.normal.x), packSnorm16(vertex.normal.y), packSnorm16(vertex.normal.z), 0},
                     {packSnorm16(vertex.tangent.x), packSnorm16(vertex.tangent.y), packSnorm16(vertex.tangent.z), packSnorm16(vertex.tangent.w)}};
}

} // namespace

void VulkanRenderer::Impl::destroyMeshUpload(MeshUpload& upload) {
    destroyBuffer(upload.staging);
    destroyBuffer(upload.indices);
    destroyBuffer(upload.vertices);
    upload.meshes = {};
}

void VulkanRenderer::Impl::recordMeshUpload(VkCommandBuffer commandBuffer, const MeshUpload& upload) const {
    VkBufferCopy vertexCopy{};
    vertexCopy.size = upload.vertexSize;
    vkCmdCopyBuffer(commandBuffer, upload.staging.buffer, upload.vertices.buffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{};
    indexCopy.srcOffset = upload.indexStagingOffset;
    indexCopy.size = upload.indexSize;
    vkCmdCopyBuffer(commandBuffer, upload.staging.buffer, upload.indices.buffer, 1, &indexCopy);

    if (deviceOwner_.transferQueue == deviceOwner_.graphicsQueue) {
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

const VulkanRenderer::Impl::GpuMesh& VulkanRenderer::Impl::meshFor(const MeshAssetHandle mesh) const {
    return meshForBatch(meshBatchIndex(mesh));
}

std::size_t VulkanRenderer::Impl::meshBatchIndex(const MeshAssetHandle mesh) const {
    if (mesh == builtin_assets::kCube || mesh == builtin_assets::kSphere ||
        mesh == builtin_assets::kGroundPlane) {
        return baseSceneMeshBatchIndex(mesh);
    }
    constexpr std::uint32_t kFirstAuthoredMeshIndex = builtin_assets::kReferenceMesh.index;
    if (mesh.generation != 1U || mesh.index < kFirstAuthoredMeshIndex) {
        throw std::runtime_error("Unknown or stale scene mesh handle");
    }
    const std::size_t batch = kBaseSceneMeshBatchOrder.size() +
        static_cast<std::size_t>(mesh.index - kFirstAuthoredMeshIndex);
    if (batch >= resourceOwner_.sceneMeshes.size()) {
        throw std::runtime_error("Unknown or stale authored mesh handle");
    }
    return batch;
}

const VulkanRenderer::Impl::GpuMesh& VulkanRenderer::Impl::meshForBatch(const std::size_t meshIndex) const {
    if (meshIndex >= resourceOwner_.sceneMeshes.size()) {
        throw std::runtime_error("Scene mesh batch index out of range");
    }
    return resourceOwner_.sceneMeshes[meshIndex];
}

MeshBounds VulkanRenderer::Impl::meshBounds(const MeshAssetHandle mesh) const {
    const std::size_t meshIndex = meshBatchIndex(mesh);
    if (meshIndex >= resourceOwner_.sceneMeshBounds.size()) {
        throw std::runtime_error("Scene mesh bounds index out of range");
    }
    return resourceOwner_.sceneMeshBounds[meshIndex];
}

VulkanRenderer::Impl::MeshUpload VulkanRenderer::Impl::stageMeshUpload(
    std::vector<MeshData>& meshes) {
    MeshUpload upload{};
    upload.meshes.resize(meshes.size());
    for (const MeshData& mesh : meshes) {
        if (mesh.vertices.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::runtime_error("Scene mesh vertex offset exceeds VkDrawIndexed vertexOffset range");
        }
        if (mesh.indices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::runtime_error("Scene mesh index count exceeds uint32 range");
        }
    }
    for (MeshData& mesh : meshes) {
        optimizeTriangleIndexOrderForVertexCache(mesh.indices, mesh.vertices.size());
        optimizeVertexFetchOrder(mesh);
    }
    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    for (const MeshData& mesh : meshes) {
        if (mesh.vertices.size() > std::numeric_limits<std::size_t>::max() - vertexCount ||
            mesh.indices.size() > std::numeric_limits<std::size_t>::max() - indexCount) {
            throw std::runtime_error("Scene geometry exceeds host size range");
        }
        vertexCount += mesh.vertices.size();
        indexCount += mesh.indices.size();
    }
    if (vertexCount > static_cast<std::size_t>(std::numeric_limits<VkDeviceSize>::max() / sizeof(GpuVertex)) ||
        indexCount > static_cast<std::size_t>(std::numeric_limits<VkDeviceSize>::max() / sizeof(std::uint32_t))) {
        throw std::runtime_error("Scene geometry upload exceeds VkDeviceSize range");
    }
    upload.vertexSize = static_cast<VkDeviceSize>(vertexCount * sizeof(GpuVertex));
    upload.indexSize = static_cast<VkDeviceSize>(indexCount * sizeof(std::uint32_t));
    if (upload.indexSize > std::numeric_limits<VkDeviceSize>::max() - upload.vertexSize) {
        throw std::runtime_error("Scene geometry staging size exceeds VkDeviceSize range");
    }
    upload.indexStagingOffset = upload.vertexSize;
    const VkDeviceSize stagingSize = upload.vertexSize + upload.indexSize;

    try {
        upload.staging = createBuffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        {
            ScopedVmaMap stagingMap{deviceOwner_.allocator, upload.staging.allocation, "vmaMapMemory mesh staging"};
            auto* stagingBytes = static_cast<std::uint8_t*>(stagingMap.get());
            auto* vertexDst = reinterpret_cast<GpuVertex*>(stagingBytes);
            auto* indexDst = reinterpret_cast<std::uint32_t*>(stagingBytes + static_cast<std::size_t>(upload.indexStagingOffset));
            std::size_t vertexCursor = 0;
            std::size_t indexCursor = 0;
            const auto appendMesh = [&](MeshData& mesh) -> GpuMesh {
                if (indexCursor > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - mesh.indices.size()) {
                    throw std::runtime_error("Scene geometry index buffer exceeds uint32 range");
                }
                const std::size_t firstVertex = vertexCursor;
                if (firstVertex > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                    throw std::runtime_error("Scene mesh vertex offset exceeds VkDrawIndexed vertexOffset range");
                }
                const std::size_t firstIndex = indexCursor;
                const std::size_t meshVertexCount = mesh.vertices.size();
                const std::size_t meshIndexCount = mesh.indices.size();
                GpuVertex* meshVertexDst = vertexDst + vertexCursor;
                for (std::size_t vertexIndex = 0; vertexIndex < meshVertexCount; ++vertexIndex) {
                    meshVertexDst[vertexIndex] = packVertex(mesh.vertices[vertexIndex]);
                }
                vertexCursor += meshVertexCount;
                if (meshIndexCount > 0U) {
                    std::memcpy(indexDst + indexCursor, mesh.indices.data(), meshIndexCount * sizeof(std::uint32_t));
                }
                indexCursor += meshIndexCount;
                return GpuMesh{static_cast<std::uint32_t>(meshIndexCount),
                               static_cast<std::uint32_t>(firstIndex),
                               static_cast<std::int32_t>(firstVertex)};
            };

            for (std::size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
                upload.meshes[meshIndex] = appendMesh(meshes[meshIndex]);
            }
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

const ReferenceAssetBundle& VulkanRenderer::Impl::referenceAssets() {
    if (resourceOwner_.referenceAssets == nullptr) {
        throw std::logic_error("Renderer has no reference asset bundle");
    }
    return *resourceOwner_.referenceAssets;
}

void VulkanRenderer::Impl::createMeshes() {
    MeshUpload meshUpload{};
    try {
        const ReferenceAssetBundle& authored = referenceAssets();
        if (authored.scene.meshes.empty()) {
            throw std::runtime_error("Authored reference scene contains no mesh primitives");
        }
        std::vector<MeshData> meshes(kBaseSceneMeshBatchOrder.size() +
                                     authored.scene.meshes.size());
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::Cube)] = createCubeMesh();
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh)] = createUvSphereMesh(32, 64);
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::SphereMedium)] = createUvSphereMesh(16, 32);
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::SphereLow)] = createUvSphereMesh(8, 16);
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::GroundPlane)] = createPlaneMesh(12.0f, 12.0f);
        stats_.assetCookMs = authored.metrics.cookMilliseconds;
        stats_.assetRecordCount = static_cast<unsigned>(authored.database.records().size());
        stats_.assetCacheHits = authored.metrics.cacheHits;
        stats_.assetCacheMisses = authored.metrics.cacheMisses;
        stats_.assetRebuiltCount = authored.metrics.rebuiltAssets;
        logger()->info("Reference assets: {} records, {} hits, {} misses, {:.3f} ms",
                       authored.database.records().size(), authored.metrics.cacheHits,
                       authored.metrics.cacheMisses, authored.metrics.cookMilliseconds);
        for (std::size_t index = 0; index < authored.scene.meshes.size(); ++index) {
            meshes[kBaseSceneMeshBatchOrder.size() + index] =
                authored.scene.meshes[index].mesh;
        }
        resourceOwner_.sceneMeshBounds.resize(meshes.size());
        for (std::size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
            resourceOwner_.sceneMeshBounds[meshIndex] = meshes[meshIndex].bounds;
        }
        meshUpload = stageMeshUpload(meshes);
        for (MeshData& mesh : meshes) {
            mesh = {};
        }

        VkCommandBuffer uploadCommands = beginUploadCommands();
        recordMeshUpload(uploadCommands, meshUpload);
        submitTransferUpload(uploadCommands, takeBuffer(meshUpload.staging));
    } catch (...) {
        destroyMeshUpload(meshUpload);
        throw;
    }

    resourceOwner_.sceneVertexBuffer = takeBuffer(meshUpload.vertices);
    resourceOwner_.sceneIndexBuffer = takeBuffer(meshUpload.indices);
    resourceOwner_.sceneMeshes = meshUpload.meshes;
    resourceOwner_.sceneMeshTriangleCounts.resize(resourceOwner_.sceneMeshes.size());
    for (std::size_t meshIndex = 0; meshIndex < resourceOwner_.sceneMeshes.size(); ++meshIndex) {
        resourceOwner_.sceneMeshTriangleCounts[meshIndex] = resourceOwner_.sceneMeshes[meshIndex].indexCount / 3U;
    }

    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(resourceOwner_.sceneVertexBuffer.buffer), "Scene Geometry Vertex Buffer");
    vmaSetAllocationName(deviceOwner_.allocator, resourceOwner_.sceneVertexBuffer.allocation, "Scene Geometry Vertex Allocation");
    resourceOwner_.sceneVertexBuffer.resourceId = resourceOwner_.registry.registerResource(GpuResourceKind::Buffer, "Scene Geometry Vertex Buffer", resourceOwner_.sceneVertexBuffer.size);
    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(resourceOwner_.sceneIndexBuffer.buffer), "Scene Geometry Index Buffer");
    vmaSetAllocationName(deviceOwner_.allocator, resourceOwner_.sceneIndexBuffer.allocation, "Scene Geometry Index Allocation");
    resourceOwner_.sceneIndexBuffer.resourceId = resourceOwner_.registry.registerResource(GpuResourceKind::Buffer, "Scene Geometry Index Buffer", resourceOwner_.sceneIndexBuffer.size);
}

} // namespace ve
