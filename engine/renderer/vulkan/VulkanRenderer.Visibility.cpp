#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {
namespace {

[[nodiscard]] bool sameVec4(const Vec4& left, const Vec4& right) noexcept {
    return left.x == right.x && left.y == right.y &&
           left.z == right.z && left.w == right.w;
}

[[nodiscard]] bool sameRenderItem(const SceneRenderItem& left,
                                  const SceneRenderItem& right) noexcept {
    return left.boundsCenter.x == right.boundsCenter.x &&
           left.boundsCenter.y == right.boundsCenter.y &&
           left.boundsCenter.z == right.boundsCenter.z &&
           left.boundsRadius == right.boundsRadius &&
           left.mesh == right.mesh && left.model.m == right.model.m &&
           sameVec4(left.material.albedoRoughness,
                    right.material.albedoRoughness) &&
           sameVec4(left.material.emissiveMetallic,
                    right.material.emissiveMetallic) &&
           sameVec4(left.material.flags, right.material.flags) &&
           left.material.textures == right.material.textures;
}
[[nodiscard]] bool sameShadowCasterLayout(
    const SceneRenderItem& left, const SceneRenderItem& right) noexcept {
    if (left.boundsCenter.x != right.boundsCenter.x ||
        left.boundsCenter.y != right.boundsCenter.y ||
        left.boundsCenter.z != right.boundsCenter.z ||
        left.boundsRadius != right.boundsRadius ||
        left.mesh != right.mesh) {
        return false;
    }
    constexpr float foliageClass =
        static_cast<float>(RenderMaterialClass::Foliage);
    if ((left.material.flags.y == foliageClass) !=
        (right.material.flags.y == foliageClass)) {
        return false;
    }
    const auto leftFeatures =
        static_cast<std::uint32_t>(left.material.flags.x);
    const auto rightFeatures =
        static_cast<std::uint32_t>(right.material.flags.x);
    if (((leftFeatures | rightFeatures) & MaterialFeatureAlphaMask) == 0U) {
        return true;
    }
    return left.material.flags.x == right.material.flags.x &&
           left.material.textures == right.material.textures;
}


} // namespace


VulkanRenderer::Impl::SceneVisibilityPlan
VulkanRenderer::Impl::prepareGpuVisibility(
    FrameResources& frame, const std::size_t frameIndex, const Camera& camera,
    const Mat4& projection, const Mat4& viewProjection,
    const SceneRenderList& renderItems) {
    if (renderItems.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("GPU visibility candidate count exceeds uint32 range");
    }
    if (resourceOwner_.sceneClusters.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("GPU cluster count exceeds uint32 range");
    }
    const bool clusterSubmission = config_.gpuClusterCommands;
    const std::size_t commandCount =
        clusterSubmission ? resourceOwner_.sceneClusters.size()
                          : resourceOwner_.sceneMeshes.size();
    auto& clusterCapacities = gpuClusterCapacityScratch_;
    clusterCapacities.resize(commandCount);
    auto& meshPotentialCounts = gpuMeshPotentialCountScratch_;
    meshPotentialCounts.resize(resourceOwner_.sceneMeshes.size());
    const std::array<std::size_t, 4> sphereMeshes{
        sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh),
        sceneMeshBatchIndex(SceneMeshBatchId::SphereMedium),
        sceneMeshBatchIndex(SceneMeshBatchId::SphereLow),
        sceneMeshBatchIndex(SceneMeshBatchId::SphereUltraLow)};
    const SceneGridRange& gridRange = renderItems.materialGridRange();
    const bool gridRangeFits =
        gridRange.valid && gridRange.firstItem <= renderItems.size() &&
        gridRange.rows != 0U &&
        static_cast<std::size_t>(gridRange.columns) <=
            (renderItems.size() - gridRange.firstItem) / gridRange.rows;
    const std::size_t gridEnd =
        gridRangeFits
            ? gridRange.firstItem +
                  static_cast<std::size_t>(gridRange.rows) * gridRange.columns
            : 0U;
    const bool cacheGridAggregate =
        gridRangeFits && renderItems.materialGridTilesCoverRange();
    auto& cachedGridMeshCounts =
        frame.cachedGpuMaterialGridMeshPotentialCounts;
    const bool reuseCachedGridMeshCounts =
        cacheGridAggregate &&
        cachedGridMeshCounts.size() == meshPotentialCounts.size() &&
        frame.cachedGpuMaterialGridMeshContentRevision ==
            renderItems.materialGridContentRevision();
    if (reuseCachedGridMeshCounts) {
        meshPotentialCounts = cachedGridMeshCounts;
    } else {
        std::ranges::fill(meshPotentialCounts, 0U);
        if (cacheGridAggregate) {
            cachedGridMeshCounts.assign(meshPotentialCounts.size(), 0U);
        }
    }

    for (std::size_t index = 0U; index < renderItems.size(); ++index) {
        if (reuseCachedGridMeshCounts && index == gridRange.firstItem) {
            index = gridEnd - 1U;
            continue;
        }
        const SceneRenderItem& item = renderItems[index];
        const bool gridItem =
            cacheGridAggregate && index >= gridRange.firstItem &&
            index < gridEnd;
        if (item.mesh == builtin_assets::kSphere) {
            for (const std::size_t meshIndex : sphereMeshes) {
                ++meshPotentialCounts[meshIndex];
                if (gridItem) ++cachedGridMeshCounts[meshIndex];
            }
        } else {
            const std::size_t meshIndex = meshBatchIndex(item.mesh);
            ++meshPotentialCounts[meshIndex];
            if (gridItem) ++cachedGridMeshCounts[meshIndex];
        }
    }
    if (cacheGridAggregate && !reuseCachedGridMeshCounts) {
        frame.cachedGpuMaterialGridMeshContentRevision =
            renderItems.materialGridContentRevision();
    }
    std::uint64_t clusterInstanceCapacity64 = 0;
    for (std::size_t commandIndex = 0; commandIndex < commandCount;
         ++commandIndex) {
        const std::size_t meshIndex =
            clusterSubmission
                ? resourceOwner_.sceneClusters[commandIndex].meshIndex
                : commandIndex;
        const std::uint32_t capacity = meshPotentialCounts[meshIndex];
        clusterCapacities[commandIndex] = capacity;
        clusterInstanceCapacity64 += capacity;
    }
    if (clusterInstanceCapacity64 >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "GPU visible cluster instance capacity exceeds uint32 range");
    }
    const std::size_t clusterInstanceCapacity =
        static_cast<std::size_t>(clusterInstanceCapacity64);
    ensureGpuVisibilityCapacity(frame, frameIndex, renderItems.size(),
                                clusterInstanceCapacity);
    const bool updateAllRenderItems =
        !frame.gpuRenderItemCacheValid ||
        frame.cachedGpuRenderItems.size() != renderItems.size();
    frame.cachedGpuRenderItems.resize(renderItems.size());
    bool renderItemsChanged = updateAllRenderItems;
    bool shadowCasterLayoutChanged = updateAllRenderItems;
    frame.hasAlphaMaskedRenderItems = false;
    const bool reuseCachedGridItems =
        !updateAllRenderItems && gridRangeFits &&
        renderItems.materialGridTilesCoverRange() &&
        frame.cachedGpuMaterialGridContentRevision ==
            renderItems.materialGridContentRevision();


    auto* candidates =
        static_cast<GpuCullCandidate*>(frame.cullCandidates.mapped);
    auto* sceneInstances =
        static_cast<InstanceData*>(frame.instanceData.mapped);
    SceneVisibilityPlan plan{};
    plan.meshInstanceCounts.assign(resourceOwner_.sceneMeshes.size(), 0U);
    plan.visibleItemCount = static_cast<std::uint32_t>(renderItems.size());
    plan.cameraPosition = camera.position();
    plan.cameraForward = camera.forward();
    std::array<unsigned, kRenderMaterialClassCount> gridClassCounts{};
    std::uint64_t gridTriangleCount = 0;
    bool gridHasAlphaMaskedItems = false;
    for (std::size_t index = 0; index < renderItems.size(); ++index) {
        if (reuseCachedGridItems && index == gridRange.firstItem) {
            frame.hasAlphaMaskedRenderItems |=
                frame.cachedGpuMaterialGridHasAlphaMaskedItems;
            for (std::size_t materialClass = 0U;
                 materialClass < kRenderMaterialClassCount; ++materialClass) {
                plan.materialClassCounts[materialClass] +=
                    frame.cachedGpuMaterialGridClassCounts[materialClass];
            }
            plan.sceneTriangleCount +=
                frame.cachedGpuMaterialGridTriangleCount;
            index = gridEnd - 1U;
            continue;
        }
        const SceneRenderItem& item = renderItems[index];
        const bool alphaMasked =
            (static_cast<std::uint32_t>(item.material.flags.x) &
             MaterialFeatureAlphaMask) != 0U;
        frame.hasAlphaMaskedRenderItems |= alphaMasked;
        const std::size_t materialClass = static_cast<std::size_t>(
            std::clamp(std::lround(item.material.flags.y), 0L,
                       static_cast<long>(kRenderMaterialClassCount - 1U)));
        ++plan.materialClassCounts[materialClass];
        if (updateAllRenderItems ||
            !sameRenderItem(frame.cachedGpuRenderItems[index], item)) {
            shadowCasterLayoutChanged =
                shadowCasterLayoutChanged ||
                !sameShadowCasterLayout(
                    frame.cachedGpuRenderItems[index], item);
            GpuCullCandidate& candidate = candidates[index];
            sceneInstances[index] = instanceDataFor(item);
            candidate.model = item.model;
            candidate.bounds = {item.boundsCenter.x, item.boundsCenter.y,
                                item.boundsCenter.z, item.boundsRadius};
            candidate.metadata = {
                item.mesh == builtin_assets::kSphere
                    ? 0U
                    : static_cast<std::uint32_t>(meshBatchIndex(item.mesh)),
                item.mesh == builtin_assets::kSphere ? 1U : 0U,
                static_cast<std::uint32_t>(materialClass), 0U};
            frame.cachedGpuRenderItems[index] = item;
            renderItemsChanged = true;
        }
        const std::size_t triangleMesh =
            item.mesh == builtin_assets::kSphere
                ? sphereMeshes[0]
                : meshBatchIndex(item.mesh);
        const std::uint64_t triangleCount =
            resourceOwner_.sceneMeshTriangleCounts[triangleMesh];
        plan.sceneTriangleCount += triangleCount;
        if (cacheGridAggregate && index >= gridRange.firstItem &&
            index < gridEnd) {
            gridHasAlphaMaskedItems |= alphaMasked;
            ++gridClassCounts[materialClass];
            gridTriangleCount += triangleCount;
        }
    }
    if (cacheGridAggregate && !reuseCachedGridItems) {
        frame.cachedGpuMaterialGridClassCounts = gridClassCounts;
        frame.cachedGpuMaterialGridTriangleCount = gridTriangleCount;
        frame.cachedGpuMaterialGridHasAlphaMaskedItems =
            gridHasAlphaMaskedItems;
    }
    frame.gpuRenderItemCacheValid = true;
    frame.cachedGpuMaterialGridContentRevision =
        renderItems.materialGridContentRevision();
    frame.gpuRenderItemsChangedThisFrame = renderItemsChanged;
    frame.shadowCasterLayoutChangedThisFrame =
        shadowCasterLayoutChanged;

    auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(
        frame.indirectCommands.mapped);
    std::uint32_t firstInstance = 0;
    for (std::size_t commandIndex = 0; commandIndex < commandCount;
         ++commandIndex) {
        if (clusterSubmission) {
            const GpuCluster& cluster =
                resourceOwner_.sceneClusters[commandIndex];
            commands[commandIndex] = {
                cluster.indexCount, 0U, cluster.firstIndex,
                cluster.vertexOffset, firstInstance};
        } else {
            const GpuMesh& mesh = resourceOwner_.sceneMeshes[commandIndex];
            commands[commandIndex] = {
                mesh.indexCount, 0U, mesh.firstIndex, mesh.vertexOffset,
                firstInstance};
        }
        firstInstance += clusterCapacities[commandIndex];
    }
    if (firstInstance != clusterInstanceCapacity64) {
        throw std::logic_error("GPU cluster capacity prefix sum is inconsistent");
    }
    frame.submittedGpuCommandCount =
        static_cast<std::uint32_t>(commandCount);
    frame.submittedClusterCommands = clusterSubmission;

    const Frustum frustum = extractFrustumPlanes(viewProjection);
    GpuCullUniforms uniforms{};
    for (std::size_t planeIndex = 0; planeIndex < frustum.size(); ++planeIndex) {
        const FrustumPlane& plane = frustum[planeIndex];
        uniforms.frustumPlanes[planeIndex] = {
            plane.normal.x, plane.normal.y, plane.normal.z, plane.distance};
    }
    uniforms.cameraPositionProjectionScale = {
        plan.cameraPosition.x, plan.cameraPosition.y, plan.cameraPosition.z,
        std::abs(projection.m[5])};
    uniforms.cameraForward = {
        plan.cameraForward.x, plan.cameraForward.y, plan.cameraForward.z,
        std::abs(projection.m[5]) *
            static_cast<float>(swapchainOwner_.extent.height) * 0.5f};
    uniforms.counts = {
        static_cast<std::uint32_t>(renderItems.size()),
        static_cast<std::uint32_t>(commandCount),
        firstInstance, clusterSubmission ? 1U : 0U};
    uniforms.sphereLodMeshes = {
        static_cast<std::uint32_t>(sphereMeshes[0]),
        static_cast<std::uint32_t>(sphereMeshes[1]),
        static_cast<std::uint32_t>(sphereMeshes[2]),
        static_cast<std::uint32_t>(sphereMeshes[3])};
    uniforms.viewProjection = viewProjection;
    uniforms.depthPyramid = {
        static_cast<float>(resourceOwner_.depthPyramid.extent.width),
        static_cast<float>(resourceOwner_.depthPyramid.extent.height),
        static_cast<float>(resourceOwner_.depthPyramid.mipLevels),
        resourceOwner_.depthPyramidValid &&
                config_.depthPyramidOcclusion &&
                !config_.gpuVisibilityValidation
            ? 1.0f
            : 0.0f};
    if (config_.gpuVisibilityValidation) {
        frame.expectedCullingUnitCounts.assign(commandCount, 0U);
        frame.expectedVisibleItemCount = 0U;
        frame.expectedMaterialClassCounts.fill(0U);
        frame.expectedSphereLodCounts.fill(0U);
        for (const SceneRenderItem& item : renderItems) {
            if (classifySphereAgainstFrustum(
                    frustum, item.boundsCenter, item.boundsRadius) ==
                FrustumSphereClassification::Outside) {
                continue;
            }
            ++frame.expectedVisibleItemCount;
            const std::size_t materialClass = static_cast<std::size_t>(
                std::clamp(
                    std::lround(item.material.flags.y), 0L,
                    static_cast<long>(kRenderMaterialClassCount - 1U)));
            ++frame.expectedMaterialClassCounts[materialClass];
            std::size_t meshIndex = 0;
            if (item.mesh == builtin_assets::kSphere) {
                const float viewDepth =
                    std::max(dot(item.boundsCenter - plan.cameraPosition,
                                 plan.cameraForward) -
                                 item.boundsRadius,
                             0.001f);
                const float projectedRadiusPixels =
                    item.boundsRadius * uniforms.cameraForward.w / viewDepth;
                const std::size_t lod =
                    projectedRadiusPixels >= 12.6f
                        ? 0U
                        : projectedRadiusPixels >= 4.32f
                              ? 1U
                              : (projectedRadiusPixels >= 2.0f ? 2U : 3U);
                meshIndex = sphereMeshes[lod];
                ++frame.expectedSphereLodCounts[
                    std::min(lod, std::size_t{2})];
            } else {
                meshIndex = meshBatchIndex(item.mesh);
            }
            if (!clusterSubmission) {
                ++frame.expectedCullingUnitCounts[meshIndex];
                continue;
            }
            const GpuMeshClusterRange range =
                resourceOwner_.sceneMeshClusterRanges[meshIndex];
            const Vec3 scaleX{item.model.m[0], item.model.m[1],
                              item.model.m[2]};
            const Vec3 scaleY{item.model.m[4], item.model.m[5],
                              item.model.m[6]};
            const Vec3 scaleZ{item.model.m[8], item.model.m[9],
                              item.model.m[10]};
            const float radiusScale =
                std::max({length(scaleX), length(scaleY), length(scaleZ)});
            for (std::uint32_t offset = 0; offset < range.clusterCount;
                 ++offset) {
                const std::uint32_t clusterIndex =
                    range.firstCluster + offset;
                const GpuCluster& cluster =
                    resourceOwner_.sceneClusters[clusterIndex];
                const Vec3 localCenter{cluster.bounds.x, cluster.bounds.y,
                                       cluster.bounds.z};
                const Vec3 worldCenter{
                    item.model.m[0] * localCenter.x +
                        item.model.m[4] * localCenter.y +
                        item.model.m[8] * localCenter.z + item.model.m[12],
                    item.model.m[1] * localCenter.x +
                        item.model.m[5] * localCenter.y +
                        item.model.m[9] * localCenter.z + item.model.m[13],
                    item.model.m[2] * localCenter.x +
                        item.model.m[6] * localCenter.y +
                        item.model.m[10] * localCenter.z + item.model.m[14]};
                if (classifySphereAgainstFrustum(
                        frustum, worldCenter,
                        cluster.bounds.w * radiusScale) !=
                    FrustumSphereClassification::Outside) {
                    ++frame.expectedCullingUnitCounts[clusterIndex];
                }
            }
        }
        frame.gpuVisibilityValidationPending = true;
    }
    *static_cast<GpuCullCounters*>(frame.cullCounters.mapped) = {};
    std::memcpy(frame.cullUniforms.mapped, &uniforms, sizeof(uniforms));
    const VulkanBufferSyncState hostWrite{
        VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT};
    if (renderItemsChanged) {
        frame.cullCandidates.syncState = hostWrite;
        frame.instanceData.syncState = hostWrite;
    }
    frame.cullUniforms.syncState = hostWrite;
    frame.cullCounters.syncState = hostWrite;
    frame.indirectCommands.syncState = hostWrite;
    return plan;
}

void VulkanRenderer::Impl::recordGpuCull(
    const VkCommandBuffer commandBuffer,
    const std::uint32_t candidateCount) const {
    if (candidateCount == 0U) {
        return;
    }
    const VkPipeline cullPipeline =
        !config_.gpuClusterCommands &&
                pipelineOwner_.cullSubgroup != VK_NULL_HANDLE
            ? pipelineOwner_.cullSubgroup
            : pipelineOwner_.cull;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      cullPipeline);
    const VkDescriptorSet descriptorSet =
        resourceOwner_.cullDescriptorSets[frameOwner_.currentFrame];
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineOwner_.cullLayout, 0, 1, &descriptorSet, 0,
                            nullptr);
    vkCmdDispatch(commandBuffer, (candidateCount + 63U) / 64U, 1U, 1U);
}

void VulkanRenderer::Impl::validateGpuVisibility(
    FrameResources& frame) const {
    const auto* commands = static_cast<const VkDrawIndexedIndirectCommand*>(
        frame.indirectCommands.mapped);
    if (frame.submittedOnce && indirectSceneDrawsEnabled_) {
        const auto* counters =
            static_cast<const GpuCullCounters*>(frame.cullCounters.mapped);
        frame.completedVisibleItemCount = counters->visibleItemCount;
        frame.completedVisibleCullingUnitCount =
            counters->visibleCullingUnitCount;
        frame.completedTestedCullingUnitCount =
            counters->testedCullingUnitCount;
        frame.completedOccludedCullingUnitCount =
            counters->occludedCullingUnitCount;
        frame.completedSphereLodCounts = counters->sphereLodCounts;
    std::ranges::copy(counters->visibleMaterialClassCounts,
                      frame.completedMaterialClassCounts.begin());
    frame.completedSceneTriangleCount = 0U;
        for (std::size_t commandIndex = 0;
             commandIndex < frame.submittedGpuCommandCount;
             ++commandIndex) {
            const std::uint32_t indexCount =
                frame.submittedClusterCommands
                    ? resourceOwner_.sceneClusters[commandIndex].indexCount
                    : resourceOwner_.sceneMeshes[commandIndex].indexCount;
            frame.completedSceneTriangleCount +=
                static_cast<std::uint64_t>(indexCount / 3U) *
                commands[commandIndex].instanceCount;
        }
        frame.completedGpuCullCountersValid = true;
    }
    if (!frame.gpuVisibilityValidationPending) {
        return;
    }
    if (frame.expectedCullingUnitCounts.size() !=
        frame.submittedGpuCommandCount) {
        throw std::logic_error(
            "GPU visibility reference command count is inconsistent");
    }
    std::uint32_t expectedVisibleCullingUnits = 0U;
    for (std::size_t commandIndex = 0;
         commandIndex < frame.expectedCullingUnitCounts.size();
         ++commandIndex) {
        const std::uint32_t actual = commands[commandIndex].instanceCount;
        const std::uint32_t expected =
            frame.expectedCullingUnitCounts[commandIndex];
        const std::uint32_t limit =
            commandIndex + 1U < frame.expectedCullingUnitCounts.size()
                ? commands[commandIndex + 1U].firstInstance
                : static_cast<std::uint32_t>(
                      frame.clusterInstanceCapacity);
        expectedVisibleCullingUnits += expected;
        if (commands[commandIndex].firstInstance > limit ||
            actual > limit - commands[commandIndex].firstInstance) {
            throw std::runtime_error(
                "GPU visibility command exceeded its output partition");
        }
        if (actual != expected) {
            throw std::runtime_error(
                "GPU visibility disagrees with CPU reference at command " +
                std::to_string(commandIndex) + ": expected " +
                std::to_string(expected) + ", observed " +
                std::to_string(actual));
        }
    }
    if (frame.completedVisibleCullingUnitCount !=
            expectedVisibleCullingUnits ||
        frame.completedVisibleItemCount != frame.expectedVisibleItemCount ||
        frame.completedSphereLodCounts[0] !=
            frame.expectedSphereLodCounts[0] ||
        frame.completedSphereLodCounts[1] !=
            frame.expectedSphereLodCounts[1] ||
        frame.completedSphereLodCounts[2] !=
            frame.expectedSphereLodCounts[2] ||
        frame.completedMaterialClassCounts !=
            frame.expectedMaterialClassCounts) {
        throw std::runtime_error(
            "GPU visibility summary disagrees with CPU reference");
    }
    frame.gpuVisibilityValidationPending = false;
}

VulkanRenderer::Impl::SceneVisibilityPlan
VulkanRenderer::Impl::planSceneVisibility(
    const Camera& camera, const Mat4& projection,
    const Mat4& viewProjection, const SceneRenderList& renderItems) {
    SceneVisibilityPlan plan{};
    plan.meshInstanceCounts.resize(resourceOwner_.sceneMeshes.size());
    if (gridVisibilityCache_.meshInstanceCounts.size() !=
        resourceOwner_.sceneMeshes.size()) {
        gridVisibilityCache_.valid = false;
        gridVisibilityCache_.meshInstanceCounts.resize(
            resourceOwner_.sceneMeshes.size());
        gridVisibilityCache_.instanceDataByMesh.resize(
            resourceOwner_.sceneMeshes.size());
    }
    const Frustum frustum = extractFrustumPlanes(viewProjection);
    auto& visibleSceneWork = visibleSceneWorkScratch_;
    visibleSceneWork.clear();
    const auto& meshTriangleCounts = resourceOwner_.sceneMeshTriangleCounts;

    const Vec3 cameraPosition = camera.position();
    const Vec3 cameraForward = camera.forward();
    plan.cameraPosition = cameraPosition;
    plan.cameraForward = cameraForward;
    const float projectionScaleY =
        std::abs(projection.m[5]);
    const float projectionPixelScale =
        projectionScaleY *
        static_cast<float>(swapchainOwner_.extent.height) * 0.5f;
    if (renderItems.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("Scene visibility exceeds renderer instance-count range");
    }
  plan.gridRange = renderItems.materialGridRange();
    const std::vector<SceneGridTile>& gridTiles = renderItems.materialGridTiles();
    plan.gridItemCount = static_cast<std::size_t>(plan.gridRange.rows) * static_cast<std::size_t>(plan.gridRange.columns);
    const bool gridTilesCoverRange = renderItems.materialGridTilesCoverRange();
    plan.useGridTiles = plan.gridRange.valid &&
                        gridTilesCoverRange &&
                        plan.gridRange.firstItem <= renderItems.size() &&
                        plan.gridItemCount <= (renderItems.size() - plan.gridRange.firstItem);
    std::size_t visibilityWorkCapacity = renderItems.size();
    if (plan.useGridTiles) {
        const std::size_t nonGridItemCount = renderItems.size() - plan.gridItemCount;
        if (gridTiles.size() > std::numeric_limits<std::size_t>::max() - nonGridItemCount) {
            throw std::runtime_error("Scene visibility work list exceeds host size range");
        }
        visibilityWorkCapacity = nonGridItemCount + gridTiles.size();
    }
    visibleSceneWork.reserve(visibilityWorkCapacity);
    const std::size_t sphereHighBatchIndex = sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh);
    const std::size_t sphereMediumBatchIndex = sceneMeshBatchIndex(SceneMeshBatchId::SphereMedium);
    const std::size_t sphereLowBatchIndex = sceneMeshBatchIndex(SceneMeshBatchId::SphereLow);
    const std::size_t sphereUltraLowBatchIndex =
        sceneMeshBatchIndex(SceneMeshBatchId::SphereUltraLow);
    const auto sphereLodBatchIndex = [&](const Vec3 boundsCenter, const float projectedBoundsRadius, const float conservativeDepthRadius) {
        const float viewDepth = std::max(dot(boundsCenter - cameraPosition, cameraForward) - conservativeDepthRadius, 0.001f);
        const float projectedRadiusPixels =
            (projectedBoundsRadius * projectionPixelScale) / viewDepth;
        if (projectedRadiusPixels >= 12.6f) {
            return sphereHighBatchIndex;
        }
        if (projectedRadiusPixels >= 4.32f) {
            return sphereMediumBatchIndex;
        }
        return projectedRadiusPixels >= 2.0f
                   ? sphereLowBatchIndex
                   : sphereUltraLowBatchIndex;
    };
    const auto meshBatchIndexFor = [&](const MeshAssetHandle mesh, const Vec3 boundsCenter, const float projectedBoundsRadius, const float conservativeDepthRadius) {
        if (mesh == builtin_assets::kSphere) {
            return sphereLodBatchIndex(boundsCenter, projectedBoundsRadius, conservativeDepthRadius);
        }
        return meshBatchIndex(mesh);
    };
    const auto materialClassFor = [](const SceneRenderItem &item) {
    return static_cast<std::size_t>(
        std::clamp(std::lround(item.material.flags.y), 0L,
                   static_cast<long>(kRenderMaterialClassCount - 1U)));
  };
  const auto acceptVisibleItem = [&](const std::size_t itemIndex, const SceneRenderItem& item) {
        const std::size_t meshIndex = meshBatchIndexFor(item.mesh, item.boundsCenter, item.boundsRadius, 0.0f);
        visibleSceneWork.push_back(VisibleSceneWork{
            VisibleSceneWork::Kind::Item, static_cast<std::uint32_t>(meshIndex),
            static_cast<std::uint32_t>(itemIndex)});
        ++plan.visibleItemCount;
        ++plan.meshInstanceCounts[meshIndex];
        plan.sceneTriangleCount += meshTriangleCounts[meshIndex];
    ++plan.materialClassCounts[materialClassFor(item)];
  };
    const auto acceptHomogeneousTile = [&](const SceneGridTile& tile, const std::size_t tileIndex) -> bool {
        if (!tile.homogeneousMesh ||
            tileIndex > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
            tile.itemCount > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - plan.visibleItemCount)) {
            return false;
        }
    // Sphere tiles are logically homogeneous but camera-dependent LOD is per
    // item. Fall back to the tile's item loop while retaining its accepted
    // frustum result.
    if (tile.commonMesh == builtin_assets::kSphere) {
            return false;
        }
        const std::size_t meshIndex = meshBatchIndexFor(tile.commonMesh, tile.boundsCenter, tile.maxItemBoundsRadius, tile.boundsRadius);
        if (tile.itemCount > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - plan.meshInstanceCounts[meshIndex])) {
            return false;
        }
        visibleSceneWork.push_back(VisibleSceneWork{
            VisibleSceneWork::Kind::HomogeneousGridTile,
            static_cast<std::uint32_t>(meshIndex),
            static_cast<std::uint32_t>(tileIndex)});
        plan.visibleItemCount += static_cast<std::uint32_t>(tile.itemCount);
        plan.meshInstanceCounts[meshIndex] += static_cast<std::uint32_t>(tile.itemCount);
        plan.sceneTriangleCount += static_cast<std::uint64_t>(meshTriangleCounts[meshIndex]) * static_cast<std::uint64_t>(tile.itemCount);
    for (std::size_t materialClass = 0U;
         materialClass < kRenderMaterialClassCount; ++materialClass) {
      plan.materialClassCounts[materialClass] +=
          tile.materialClassCounts[materialClass];
    }
    return true;
    };
    const auto cullItem = [&](const std::size_t itemIndex) {
        const SceneRenderItem& item = renderItems[itemIndex];
        if (classifySphereAgainstFrustum(frustum, item.boundsCenter, item.boundsRadius) == FrustumSphereClassification::Outside) {
            ++plan.culledItemCount;
            return;
        }
        acceptVisibleItem(itemIndex, item);
    };
    const auto cullRange = [&](const std::size_t begin, const std::size_t end) {
        for (std::size_t itemIndex = begin; itemIndex < end; ++itemIndex) {
            cullItem(itemIndex);
        }
    };

    if (plan.useGridTiles) {
        cullRange(0, plan.gridRange.firstItem);
        plan.gridWorkBegin = visibleSceneWork.size();
        const bool reuseCachedGridVisibility = gridVisibilityCache_.valid &&
                                               gridVisibilityCache_.tileRevision == renderItems.materialGridTileRevision() &&
                                               sameGridRange(gridVisibilityCache_.range, plan.gridRange) &&
                                               gridVisibilityCache_.tileCount == gridTiles.size() &&
                                               sameMatrix(gridVisibilityCache_.viewProjection, viewProjection);
        if (reuseCachedGridVisibility) {
            plan.gridWorkEnd = plan.gridWorkBegin;
            plan.visibleItemCount += gridVisibilityCache_.visibleItemCount;
            plan.sceneTriangleCount += gridVisibilityCache_.sceneTriangleCount;
            plan.culledItemCount += gridVisibilityCache_.culledItemCount;
            plan.gridTileCount += gridVisibilityCache_.gridTileCount;
            plan.gridTilesAccepted += gridVisibilityCache_.gridTilesAccepted;
            plan.gridTilesCulled += gridVisibilityCache_.gridTilesCulled;
            plan.gridTilesIntersected += gridVisibilityCache_.gridTilesIntersected;
            for (std::size_t meshIndex = 0; meshIndex < resourceOwner_.sceneMeshes.size(); ++meshIndex) {
                plan.meshInstanceCounts[meshIndex] += gridVisibilityCache_.meshInstanceCounts[meshIndex];
            }
      for (std::size_t materialClass = 0U;
           materialClass < kRenderMaterialClassCount; ++materialClass) {
        plan.materialClassCounts[materialClass] +=
            gridVisibilityCache_.materialClassCounts[materialClass];
      }
      plan.gridVisibilityCacheHit = true;
            plan.gridVisibilityWorkItems = gridVisibilityCache_.workItemCount;
        } else {
            plan.gridWorkBegin = visibleSceneWork.size();
            const std::uint32_t gridVisibleBegin = plan.visibleItemCount;
            const std::uint64_t gridTriangleBegin = plan.sceneTriangleCount;
            const std::uint32_t gridCulledBegin = plan.culledItemCount;
            const std::uint32_t gridTileBegin = plan.gridTileCount;
            const std::uint32_t gridAcceptedBegin = plan.gridTilesAccepted;
            const std::uint32_t gridCulledTileBegin = plan.gridTilesCulled;
            const std::uint32_t gridIntersectedBegin = plan.gridTilesIntersected;
            const auto meshCountBegin = plan.meshInstanceCounts;
      const auto materialCountBegin = plan.materialClassCounts;
            for (std::size_t tileIndex = 0; tileIndex < gridTiles.size(); ++tileIndex) {
                const SceneGridTile& tile = gridTiles[tileIndex];
                const auto visitTileItems = [&](const auto& visitor) {
                    for (std::uint32_t row = tile.rowBegin; row < tile.rowEnd; ++row) {
                        const std::size_t rowBase = plan.gridRange.firstItem + (static_cast<std::size_t>(row) * plan.gridRange.columns);
                        for (std::uint32_t column = tile.columnBegin; column < tile.columnEnd; ++column) {
                            visitor(rowBase + column);
                        }
                    }
                };
                ++plan.gridTileCount;
                const FrustumSphereClassification tileVisibility = classifySphereAgainstFrustum(frustum, tile.boundsCenter, tile.boundsRadius);
                if (tileVisibility == FrustumSphereClassification::Outside) {
                    plan.culledItemCount += static_cast<std::uint32_t>(tile.itemCount);
                    ++plan.gridTilesCulled;
                    continue;
                }
                if (tileVisibility == FrustumSphereClassification::Inside) {
                    ++plan.gridTilesAccepted;
                    if (!acceptHomogeneousTile(tile, tileIndex)) {
                        visitTileItems([&](const std::size_t itemIndex) {
                            acceptVisibleItem(itemIndex, renderItems[itemIndex]);
                        });
                    }
                    continue;
                }
                ++plan.gridTilesIntersected;
                visitTileItems(cullItem);
            }

            plan.gridWorkEnd = visibleSceneWork.size();
            gridVisibilityCache_.valid = false;
            gridVisibilityCache_.tileRevision = renderItems.materialGridTileRevision();
            gridVisibilityCache_.range = plan.gridRange;
            gridVisibilityCache_.tileCount = gridTiles.size();
            gridVisibilityCache_.viewProjection = viewProjection;
            gridVisibilityCache_.workItemCount = static_cast<std::uint32_t>(plan.gridWorkEnd - plan.gridWorkBegin);
            for (std::size_t meshIndex = 0; meshIndex < resourceOwner_.sceneMeshes.size(); ++meshIndex) {
                gridVisibilityCache_.meshInstanceCounts[meshIndex] = plan.meshInstanceCounts[meshIndex] - meshCountBegin[meshIndex];
                gridVisibilityCache_.instanceDataByMesh[meshIndex].clear();
                gridVisibilityCache_.instanceDataByMesh[meshIndex].reserve(gridVisibilityCache_.meshInstanceCounts[meshIndex]);
            }
            gridVisibilityCache_.visibleItemCount = plan.visibleItemCount - gridVisibleBegin;
            gridVisibilityCache_.sceneTriangleCount = plan.sceneTriangleCount - gridTriangleBegin;
            gridVisibilityCache_.culledItemCount = plan.culledItemCount - gridCulledBegin;
            gridVisibilityCache_.gridTileCount = plan.gridTileCount - gridTileBegin;
            gridVisibilityCache_.gridTilesAccepted = plan.gridTilesAccepted - gridAcceptedBegin;
            gridVisibilityCache_.gridTilesCulled = plan.gridTilesCulled - gridCulledTileBegin;
            gridVisibilityCache_.gridTilesIntersected = plan.gridTilesIntersected - gridIntersectedBegin;
      for (std::size_t materialClass = 0U;
           materialClass < kRenderMaterialClassCount; ++materialClass) {
        gridVisibilityCache_.materialClassCounts[materialClass] =
            plan.materialClassCounts[materialClass] -
            materialCountBegin[materialClass];
      }
      plan.gridVisibilityWorkItems = gridVisibilityCache_.workItemCount;
        }
        cullRange(plan.gridRange.firstItem + plan.gridItemCount, renderItems.size());
    } else {
        gridVisibilityCache_.valid = false;
        cullRange(0, renderItems.size());
    }

    return plan;
}

} // namespace ve
