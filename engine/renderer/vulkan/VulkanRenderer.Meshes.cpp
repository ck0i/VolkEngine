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
    return meshForBatch(sceneMeshBatchIndex(mesh));
}

const VulkanRenderer::Impl::GpuMesh& VulkanRenderer::Impl::meshForBatch(const std::size_t meshIndex) const {
    if (meshIndex >= sceneMeshes_.size()) {
        throw std::runtime_error("Scene mesh batch index out of range");
    }
    return sceneMeshes_[meshIndex];
}

VulkanRenderer::Impl::MeshUpload VulkanRenderer::Impl::stageMeshUpload(std::array<MeshData, kSceneMeshBatchCount>& meshes) {
    MeshUpload upload{};
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
            ScopedVmaMap stagingMap{allocator_, upload.staging.allocation, "vmaMapMemory mesh staging"};
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

void VulkanRenderer::Impl::createMeshes() {
    MeshUpload meshUpload{};
    try {
        std::array<MeshData, kSceneMeshBatchCount> meshes{};
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::Cube)] = createCubeMesh();
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh)] = createUvSphereMesh(32, 64);
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::SphereMedium)] = createUvSphereMesh(16, 32);
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::SphereLow)] = createUvSphereMesh(8, 16);
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::GroundPlane)] = createPlaneMesh(12.0f, 12.0f);
        meshes[sceneMeshBatchIndex(SceneMeshBatchId::ImportedModel)] = loadObjMesh(config_.assetDirectory / "models" / "imported_showcase.obj");
        sceneRenderer_.setImportedModelBounds(meshes[sceneMeshBatchIndex(SceneMeshBatchId::ImportedModel)].bounds);
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

    sceneVertexBuffer_ = takeBuffer(meshUpload.vertices);
    sceneIndexBuffer_ = takeBuffer(meshUpload.indices);
    sceneMeshes_ = meshUpload.meshes;
    for (std::size_t meshIndex = 0; meshIndex < sceneMeshes_.size(); ++meshIndex) {
        sceneMeshTriangleCounts_[meshIndex] = sceneMeshes_[meshIndex].indexCount / 3U;
    }

    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(sceneVertexBuffer_.buffer), "Scene Geometry Vertex Buffer");
    vmaSetAllocationName(allocator_, sceneVertexBuffer_.allocation, "Scene Geometry Vertex Allocation");
    sceneVertexBuffer_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, "Scene Geometry Vertex Buffer", sceneVertexBuffer_.size);
    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(sceneIndexBuffer_.buffer), "Scene Geometry Index Buffer");
    vmaSetAllocationName(allocator_, sceneIndexBuffer_.allocation, "Scene Geometry Index Allocation");
    sceneIndexBuffer_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, "Scene Geometry Index Buffer", sceneIndexBuffer_.size);
}

} // namespace ve
