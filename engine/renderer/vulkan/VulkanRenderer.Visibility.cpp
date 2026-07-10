#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

VulkanRenderer::Impl::SceneVisibilityPlan VulkanRenderer::Impl::planSceneVisibility(const Camera& camera, const Mat4& projection, const Mat4& viewProjection, const SceneRenderList& renderItems) {
    static_assert(kSceneMeshBatchOrder.size() == kSceneMeshBatchCount);
    SceneVisibilityPlan plan{};
    const Frustum frustum = extractFrustumPlanes(viewProjection);
    auto& visibleSceneWork = visibleSceneWorkScratch_;
    visibleSceneWork.clear();
    const auto& meshTriangleCounts = sceneMeshTriangleCounts_;

    const Vec3 cameraPosition = camera.position();
    const Vec3 cameraForward = camera.forward();
    plan.cameraPosition = cameraPosition;
    plan.cameraForward = cameraForward;
    const float projectionScaleY = projection.m[5] < 0.0f ? -projection.m[5] : projection.m[5];
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
    const auto sphereLodBatchIndex = [&](const Vec3 boundsCenter, const float projectedBoundsRadius, const float conservativeDepthRadius) {
        const float viewDepth = std::max(dot(boundsCenter - cameraPosition, cameraForward) - conservativeDepthRadius, 0.001f);
        const float projectedRadiusNdc = (projectedBoundsRadius * projectionScaleY) / viewDepth;
        if (projectedRadiusNdc >= 0.035f) {
            return sphereHighBatchIndex;
        }
        if (projectedRadiusNdc >= 0.012f) {
            return sphereMediumBatchIndex;
        }
        return sphereLowBatchIndex;
    };
    const auto meshBatchIndexFor = [&](const SceneMeshId mesh, const Vec3 boundsCenter, const float projectedBoundsRadius, const float conservativeDepthRadius) {
        if (mesh == SceneMeshId::Sphere) {
            return sphereLodBatchIndex(boundsCenter, projectedBoundsRadius, conservativeDepthRadius);
        }
        return sceneMeshBatchIndex(mesh);
    };
    const auto acceptVisibleItem = [&](const std::size_t itemIndex, const SceneRenderItem& item) {
        const std::size_t meshIndex = meshBatchIndexFor(item.mesh, item.boundsCenter, item.boundsRadius, 0.0f);
        visibleSceneWork.push_back(VisibleSceneWork{VisibleSceneWork::Kind::Item,
                                                    static_cast<std::uint8_t>(meshIndex),
                                                    static_cast<std::uint32_t>(itemIndex)});
        ++plan.visibleItemCount;
        ++plan.meshInstanceCounts[meshIndex];
        plan.sceneTriangleCount += meshTriangleCounts[meshIndex];
    };
    const auto acceptHomogeneousTile = [&](const SceneGridTile& tile, const std::size_t tileIndex) -> bool {
        if (!tile.homogeneousMesh ||
            tileIndex > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
            tile.itemCount > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - plan.visibleItemCount)) {
            return false;
        }
        // Sphere tiles are logically homogeneous but camera-dependent LOD is per item.
        // Fall back to the tile's item loop while retaining its accepted frustum result.
        if (tile.commonMesh == SceneMeshId::Sphere) {
            return false;
        }
        const std::size_t meshIndex = meshBatchIndexFor(tile.commonMesh, tile.boundsCenter, tile.maxItemBoundsRadius, tile.boundsRadius);
        if (tile.itemCount > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - plan.meshInstanceCounts[meshIndex])) {
            return false;
        }
        visibleSceneWork.push_back(VisibleSceneWork{VisibleSceneWork::Kind::HomogeneousGridTile,
                                                    static_cast<std::uint8_t>(meshIndex),
                                                    static_cast<std::uint32_t>(tileIndex)});
        plan.visibleItemCount += static_cast<std::uint32_t>(tile.itemCount);
        plan.meshInstanceCounts[meshIndex] += static_cast<std::uint32_t>(tile.itemCount);
        plan.sceneTriangleCount += static_cast<std::uint64_t>(meshTriangleCounts[meshIndex]) * static_cast<std::uint64_t>(tile.itemCount);
        return true;
    };
    const auto cullItem = [&](const std::size_t itemIndex) {
        const SceneRenderItem& item = renderItems[itemIndex];
        if (classifySphereAgainstFrustum(frustum, item.boundsCenter, item.boundsRadius) == FrustumSphereClassification::Outside) {
            ++plan.culledDrawCalls;
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
            plan.culledDrawCalls += gridVisibilityCache_.culledDrawCalls;
            plan.gridTileCount += gridVisibilityCache_.gridTileCount;
            plan.gridTilesAccepted += gridVisibilityCache_.gridTilesAccepted;
            plan.gridTilesCulled += gridVisibilityCache_.gridTilesCulled;
            plan.gridTilesIntersected += gridVisibilityCache_.gridTilesIntersected;
            for (std::size_t meshIndex = 0; meshIndex < kSceneMeshBatchOrder.size(); ++meshIndex) {
                plan.meshInstanceCounts[meshIndex] += gridVisibilityCache_.meshInstanceCounts[meshIndex];
            }
            plan.gridVisibilityCacheHit = true;
            plan.gridVisibilityWorkItems = gridVisibilityCache_.workItemCount;
        } else {
            plan.gridWorkBegin = visibleSceneWork.size();
            const std::uint32_t gridVisibleBegin = plan.visibleItemCount;
            const std::uint64_t gridTriangleBegin = plan.sceneTriangleCount;
            const std::uint32_t gridCulledBegin = plan.culledDrawCalls;
            const std::uint32_t gridTileBegin = plan.gridTileCount;
            const std::uint32_t gridAcceptedBegin = plan.gridTilesAccepted;
            const std::uint32_t gridCulledTileBegin = plan.gridTilesCulled;
            const std::uint32_t gridIntersectedBegin = plan.gridTilesIntersected;
            const auto meshCountBegin = plan.meshInstanceCounts;
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
                    plan.culledDrawCalls += static_cast<std::uint32_t>(tile.itemCount);
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
            for (std::size_t meshIndex = 0; meshIndex < kSceneMeshBatchOrder.size(); ++meshIndex) {
                gridVisibilityCache_.meshInstanceCounts[meshIndex] = plan.meshInstanceCounts[meshIndex] - meshCountBegin[meshIndex];
                gridVisibilityCache_.instanceDataByMesh[meshIndex].clear();
                gridVisibilityCache_.instanceDataByMesh[meshIndex].reserve(gridVisibilityCache_.meshInstanceCounts[meshIndex]);
            }
            gridVisibilityCache_.visibleItemCount = plan.visibleItemCount - gridVisibleBegin;
            gridVisibilityCache_.sceneTriangleCount = plan.sceneTriangleCount - gridTriangleBegin;
            gridVisibilityCache_.culledDrawCalls = plan.culledDrawCalls - gridCulledBegin;
            gridVisibilityCache_.gridTileCount = plan.gridTileCount - gridTileBegin;
            gridVisibilityCache_.gridTilesAccepted = plan.gridTilesAccepted - gridAcceptedBegin;
            gridVisibilityCache_.gridTilesCulled = plan.gridTilesCulled - gridCulledTileBegin;
            gridVisibilityCache_.gridTilesIntersected = plan.gridTilesIntersected - gridIntersectedBegin;
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
