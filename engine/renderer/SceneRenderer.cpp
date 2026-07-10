#include "renderer/SceneRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ve {
namespace {

[[nodiscard]] bool finiteVec3(const Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool finiteMatrix(const Mat4& matrix) noexcept {
    for (const float value : matrix.m) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool affineMatrix(const Mat4& matrix) noexcept {
    return matrix.m[3] == 0.0F && matrix.m[7] == 0.0F && matrix.m[11] == 0.0F && matrix.m[15] == 1.0F;
}

[[nodiscard]] Vec3 transformPoint(const Mat4& matrix, const Vec3 point) noexcept {
    return {matrix.m[0] * point.x + matrix.m[4] * point.y + matrix.m[8] * point.z + matrix.m[12],
            matrix.m[1] * point.x + matrix.m[5] * point.y + matrix.m[9] * point.z + matrix.m[13],
            matrix.m[2] * point.x + matrix.m[6] * point.y + matrix.m[10] * point.z + matrix.m[14]};
}

[[nodiscard]] float conservativeRadiusScale(const Mat4& matrix) noexcept {
    float sum = 0.0f;
    for (const std::size_t index : {0U, 1U, 2U, 4U, 5U, 6U, 8U, 9U, 10U}) {
        sum += matrix.m[index] * matrix.m[index];
    }
    return std::sqrt(sum);
}

} // namespace

void SceneRenderList::clear() noexcept {
    items_.clear();
    materialGridRange_ = {};
    invalidateMaterialGridTiles();
}

void SceneRenderList::push(const SceneRenderItem& item) {
    const std::size_t itemIndex = items_.size();
    items_.push_back(item);
    if (indexInMaterialGridRange(itemIndex)) {
        invalidateMaterialGridTiles();
    }
}

SceneRenderItem& SceneRenderList::operator[](const std::size_t index) {
    if (indexInMaterialGridRange(index)) {
        invalidateMaterialGridTiles();
    }
    return items_[index];
}

void SceneRenderList::setMaterialGridRange(const std::size_t firstItem, const std::uint32_t rows, const std::uint32_t columns) noexcept {
    materialGridRange_ = SceneGridRange{firstItem, rows, columns, rows > 0U && columns > 0U};
    invalidateMaterialGridTiles();
}

void SceneRenderList::rebuildMaterialGridTiles(const std::uint32_t tileRows, const std::uint32_t tileColumns) {
    invalidateMaterialGridTiles();
    if (!materialGridRange_.valid || tileRows == 0U || tileColumns == 0U || materialGridRange_.firstItem > items_.size()) {
        return;
    }
    const std::size_t gridItemCount = static_cast<std::size_t>(materialGridRange_.rows) * static_cast<std::size_t>(materialGridRange_.columns);
    if (gridItemCount > items_.size() - materialGridRange_.firstItem) {
        return;
    }

    const std::uint32_t tileRowCount = ((materialGridRange_.rows - 1U) / tileRows) + 1U;
    const std::uint32_t tileColumnCount = ((materialGridRange_.columns - 1U) / tileColumns) + 1U;
    materialGridTiles_.reserve(static_cast<std::size_t>(tileRowCount) * static_cast<std::size_t>(tileColumnCount));
    for (std::uint32_t rowBegin = 0; rowBegin < materialGridRange_.rows;) {
        const std::uint32_t rowCount = std::min(tileRows, materialGridRange_.rows - rowBegin);
        const std::uint32_t rowEnd = rowBegin + rowCount;
        for (std::uint32_t columnBegin = 0; columnBegin < materialGridRange_.columns;) {
            const std::uint32_t columnCount = std::min(tileColumns, materialGridRange_.columns - columnBegin);
            const std::uint32_t columnEnd = columnBegin + columnCount;
            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float maxY = std::numeric_limits<float>::lowest();
            float maxZ = std::numeric_limits<float>::lowest();
            const std::size_t firstTileItem = materialGridRange_.firstItem +
                                              (static_cast<std::size_t>(rowBegin) * materialGridRange_.columns) +
                                              columnBegin;
            const SceneMeshId commonMesh = items_[firstTileItem].mesh;
            bool homogeneousMesh = true;
            float maxItemBoundsRadius = 0.0f;
            for (std::uint32_t row = rowBegin; row < rowEnd; ++row) {
                const std::size_t rowBase = materialGridRange_.firstItem + (static_cast<std::size_t>(row) * materialGridRange_.columns);
                for (std::uint32_t column = columnBegin; column < columnEnd; ++column) {
                    const SceneRenderItem& item = items_[rowBase + column];
                    minX = std::min(minX, item.boundsCenter.x - item.boundsRadius);
                    minY = std::min(minY, item.boundsCenter.y - item.boundsRadius);
                    minZ = std::min(minZ, item.boundsCenter.z - item.boundsRadius);
                    maxX = std::max(maxX, item.boundsCenter.x + item.boundsRadius);
                    maxY = std::max(maxY, item.boundsCenter.y + item.boundsRadius);
                    maxZ = std::max(maxZ, item.boundsCenter.z + item.boundsRadius);
                    homogeneousMesh = homogeneousMesh && item.mesh == commonMesh;
                    maxItemBoundsRadius = std::max(maxItemBoundsRadius, item.boundsRadius);
                }
            }
            const Vec3 tileCenter{(minX + maxX) * 0.5f, (minY + maxY) * 0.5f, (minZ + maxZ) * 0.5f};
            const Vec3 tileExtents{(maxX - minX) * 0.5f, (maxY - minY) * 0.5f, (maxZ - minZ) * 0.5f};
            materialGridTiles_.push_back(SceneGridTile{rowBegin,
                                                        rowEnd,
                                                        columnBegin,
                                                        columnEnd,
                                                        tileCenter,
                                                        length(tileExtents),
                                                        maxItemBoundsRadius,
                                                        static_cast<std::size_t>(rowEnd - rowBegin) * static_cast<std::size_t>(columnEnd - columnBegin),
                                                        commonMesh,
                                                        homogeneousMesh});
            columnBegin = columnEnd;
        }
        rowBegin = rowEnd;
    }
    materialGridTilesCoverRange_ = true;
}

void SceneRenderList::invalidateMaterialGridTiles() noexcept {
    materialGridTiles_.clear();
    materialGridTilesCoverRange_ = false;
    ++materialGridTileRevision_;
}

bool SceneRenderList::indexInMaterialGridRange(const std::size_t index) const noexcept {
    if (!materialGridRange_.valid || index < materialGridRange_.firstItem) {
        return false;
    }
    const std::size_t rowCount = materialGridRange_.rows;
    const std::size_t columnCount = materialGridRange_.columns;
    if (rowCount != 0U && columnCount > std::numeric_limits<std::size_t>::max() / rowCount) {
        return false;
    }
    const std::size_t gridItemCount = rowCount * columnCount;
    return index - materialGridRange_.firstItem < gridItemCount;
}

const SceneRenderList& WorldSceneExtractor::build(const World& world) {
    pendingItems_.clear();
    world.each<WorldSceneTransform, WorldSceneRenderable>(
        [&](const World::Entity entity, const WorldSceneTransform& transform, const WorldSceneRenderable& renderable) {
            const MeshBounds& bounds = renderable.localBounds;
            if (!renderable.visible || !bounds.valid || bounds.radius < 0.0f ||
                !finiteVec3(bounds.center) || !std::isfinite(bounds.radius) || !finiteMatrix(transform.model) ||
                !affineMatrix(transform.model)) {
                return;
            }

            const Vec3 worldCenter = transformPoint(transform.model, bounds.center);
            const float worldRadius = bounds.radius * conservativeRadiusScale(transform.model);
            if (!finiteVec3(worldCenter) || !std::isfinite(worldRadius)) {
                return;
            }

            pendingItems_.push_back(PendingItem{entity,
                                                SceneRenderItem{worldCenter,
                                                                worldRadius,
                                                                renderable.mesh,
                                                                transform.model,
                                                                renderable.material}});
        });

    std::sort(pendingItems_.begin(), pendingItems_.end(), [](const PendingItem& lhs, const PendingItem& rhs) {
        if (lhs.entity.index != rhs.entity.index) {
            return lhs.entity.index < rhs.entity.index;
        }
        return lhs.entity.generation < rhs.entity.generation;
    });

    renderList_.clear();
    renderList_.reserve(pendingItems_.size());
    for (const PendingItem& pending : pendingItems_) {
        renderList_.push(pending.item);
    }
    return renderList_;
}

std::size_t DemoSceneRenderer::requiredItemCount(const std::uint32_t materialGridRows, const std::uint32_t materialGridColumns) {
    const std::uint64_t materialGridItems = static_cast<std::uint64_t>(materialGridRows) * static_cast<std::uint64_t>(materialGridColumns);
    const std::uint64_t totalItems = materialGridItems + kFixedItemCount;
    if (totalItems > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("Material grid exceeds renderer instance-count range");
    }
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (totalItems > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            throw std::runtime_error("Material grid exceeds host addressable scene-list range");
        }
    }
    return static_cast<std::size_t>(totalItems);
}

void DemoSceneRenderer::validateMaterialGridDimensions(const std::uint32_t materialGridRows, const std::uint32_t materialGridColumns) {
    if (materialGridRows == 0U || materialGridColumns == 0U) {
        throw std::runtime_error("Material grid dimensions must be positive");
    }
    (void)requiredItemCount(materialGridRows, materialGridColumns);
}

void DemoSceneRenderer::setImportedModelBounds(const MeshBounds& bounds) noexcept {
    if (!bounds.valid) {
        return;
    }
    importedModelLocalBounds_ = bounds;
    invalidateStaticSceneLayout();
}

const SceneRenderList& DemoSceneRenderer::build(const double elapsedSeconds,
                                                const std::uint32_t materialGridRows,
                                                const std::uint32_t materialGridColumns,
                                                const std::uint32_t materialGridTileRows,
                                                const std::uint32_t materialGridTileColumns) {
    validateMaterialGridDimensions(materialGridRows, materialGridColumns);
    const std::size_t requiredItems = requiredItemCount(materialGridRows, materialGridColumns);
    ensureStaticSceneLayout(materialGridRows, materialGridColumns, materialGridTileRows, materialGridTileColumns, requiredItems);
    writeAnimatedItems(elapsedSeconds);
    return renderList_;
}

Mat4 DemoSceneRenderer::importedModelMatrix() {
    return translate(kImportedModelTranslation) * rotateY(kImportedModelRotationY) *
           scale({kImportedModelScale, kImportedModelScale, kImportedModelScale});
}

Vec3 DemoSceneRenderer::transformPoint(const Mat4& matrix, const Vec3 point) noexcept {
    return Vec3{matrix.m[0] * point.x + matrix.m[4] * point.y + matrix.m[8] * point.z + matrix.m[12],
                matrix.m[1] * point.x + matrix.m[5] * point.y + matrix.m[9] * point.z + matrix.m[13],
                matrix.m[2] * point.x + matrix.m[6] * point.y + matrix.m[10] * point.z + matrix.m[14]};
}

SceneRenderItem DemoSceneRenderer::importedModelItem() const {
    const Mat4 model = importedModelMatrix();
    const MeshBounds& bounds = importedModelLocalBounds_.valid ? importedModelLocalBounds_ : kFallbackImportedModelBounds;
    return SceneRenderItem{transformPoint(model, bounds.center),
                           bounds.radius * kImportedModelScale,
                           SceneMeshId::ImportedModel,
                           model,
                           {{0.72f, 0.66f, 0.54f, 0.42f}, {0.0f, 0.0f, 0.0f, 0.65f}, {0.0f, 0.0f, 0.0f, 0.0f}}};
}

void DemoSceneRenderer::invalidateStaticSceneLayout() noexcept {
    renderList_.clear();
    cachedMaterialGridRows_ = 0;
    cachedMaterialGridColumns_ = 0;
    cachedMaterialGridTileRows_ = 0;
    cachedMaterialGridTileColumns_ = 0;
}

void DemoSceneRenderer::ensureStaticSceneLayout(const std::uint32_t materialGridRows,
                                                const std::uint32_t materialGridColumns,
                                                const std::uint32_t materialGridTileRows,
                                                const std::uint32_t materialGridTileColumns,
                                                const std::size_t requiredItems) {
    if (cachedMaterialGridRows_ == materialGridRows &&
        cachedMaterialGridColumns_ == materialGridColumns &&
        cachedMaterialGridTileRows_ == materialGridTileRows &&
        cachedMaterialGridTileColumns_ == materialGridTileColumns &&
        renderList_.size() == requiredItems) {
        return;
    }

    renderList_.clear();
    renderList_.reserve(requiredItems);
    for (std::size_t itemIndex = 0; itemIndex < kAnimatedItemCount; ++itemIndex) {
        renderList_.push(SceneRenderItem{});
    }

    const float gridHalfWidth = (static_cast<float>(materialGridColumns) - 1.0f) * 0.5f;
    for (std::uint32_t row = 0; row < materialGridRows; ++row) {
        for (std::uint32_t column = 0; column < materialGridColumns; ++column) {
            const float x = (static_cast<float>(column) - gridHalfWidth) * 2.0f;
            const float z = -4.4f - static_cast<float>(row) * 1.25f;
            const float roughness = materialGridRows > 1U ? 0.18f + (static_cast<float>(row) / static_cast<float>(materialGridRows - 1U)) * 0.60f : 0.48f;
            const float metallic = materialGridColumns > 1U ? static_cast<float>(column) / static_cast<float>(materialGridColumns - 1U) : 0.0f;
            const Vec3 center{x, 0.28f, z};
            renderList_.push(SceneRenderItem{center,
                                             0.32f,
                                             SceneMeshId::Sphere,
                                             translate(center) * scale({0.28f, 0.28f, 0.28f}),
                                             {{0.38f + 0.11f * metallic * 4.0f, 0.42f + 0.08f * roughness * 3.0f, 0.82f - 0.08f * metallic * 4.0f, roughness},
                                              {0.0f, 0.0f, 0.0f, metallic},
                                              {0.0f, 0.0f, 0.0f, 0.0f}}});
        }
    }
    renderList_.setMaterialGridRange(kAnimatedItemCount, materialGridRows, materialGridColumns);
    renderList_.rebuildMaterialGridTiles(materialGridTileRows, materialGridTileColumns);
    renderList_.push(SceneRenderItem{{0.0f, 0.0f, 0.0f},
                                     17.0f,
                                     SceneMeshId::GroundPlane,
                                     Mat4::identity(),
                                     {{0.34f, 0.36f, 0.38f, 0.82f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 0.55f}}});
    renderList_.push(importedModelItem());
    cachedMaterialGridRows_ = materialGridRows;
    cachedMaterialGridColumns_ = materialGridColumns;
    cachedMaterialGridTileRows_ = materialGridTileRows;
    cachedMaterialGridTileColumns_ = materialGridTileColumns;
}

void DemoSceneRenderer::writeAnimatedItems(const double elapsedSeconds) {
    const double rotation = elapsedSeconds * 0.55;
    renderList_[0] = SceneRenderItem{{-1.55f, 0.9f, 0.0f},
                                     1.30f,
                                     SceneMeshId::Cube,
                                     translate({-1.55f, 0.9f, 0.0f}) * rotateY(static_cast<float>(rotation)) * scale({0.75f, 0.75f, 0.75f}),
                                     {{0.75f, 0.18f, 0.08f, 0.58f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}}};
    renderList_[1] = SceneRenderItem{{1.35f, 1.05f, 0.0f},
                                     0.95f,
                                     SceneMeshId::Sphere,
                                     translate({1.35f, 1.05f, 0.0f}) * rotateY(static_cast<float>(-rotation * 0.6)) * scale({0.9f, 0.9f, 0.9f}),
                                     {{0.82f, 0.78f, 0.66f, 0.32f}, {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}}};
    renderList_[2] = SceneRenderItem{{-2.75f, 0.5f, -2.1f},
                                     0.50f,
                                     SceneMeshId::Sphere,
                                     translate({-2.75f, 0.5f, -2.1f}) * scale({0.45f, 0.45f, 0.45f}),
                                     {{0.95f, 0.54f, 0.28f, 0.18f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}}};
    renderList_[3] = SceneRenderItem{{0.0f, 0.5f, -2.1f},
                                     0.50f,
                                     SceneMeshId::Sphere,
                                     translate({0.0f, 0.5f, -2.1f}) * scale({0.45f, 0.45f, 0.45f}),
                                     {{0.62f, 0.68f, 0.78f, 0.52f}, {0.0f, 0.0f, 0.0f, 0.45f}, {0.0f, 0.0f, 0.0f, 0.0f}}};
    renderList_[4] = SceneRenderItem{{2.75f, 0.5f, -2.1f},
                                     0.50f,
                                     SceneMeshId::Sphere,
                                     translate({2.75f, 0.5f, -2.1f}) * scale({0.45f, 0.45f, 0.45f}),
                                     {{0.88f, 0.82f, 0.68f, 0.08f}, {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}}};
}

} // namespace ve
