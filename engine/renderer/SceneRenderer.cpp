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

[[nodiscard]] bool validMeshBounds(const MeshBounds& bounds) noexcept {
    return bounds.valid && bounds.radius >= 0.0f && std::isfinite(bounds.radius) && finiteVec3(bounds.center);
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

[[nodiscard]] bool finiteMat4(const Mat4& matrix) noexcept {
    return std::all_of(matrix.m.begin(), matrix.m.end(), [](const float value) {
        return std::isfinite(value);
    });
}

[[nodiscard]] World::Entity nextHierarchyParent(const World& world, const World::Entity entity) noexcept {
    const WorldSceneParent* link = world.tryGet<WorldSceneParent>(entity);
    return link != nullptr && world.alive(link->parent) ? link->parent : World::Entity{};
}

[[nodiscard]] bool utf8Continuation(const unsigned char byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

[[nodiscard]] bool validUtf8(std::string_view value) noexcept {
    for (std::size_t index = 0U; index < value.size();) {
        const unsigned char first = static_cast<unsigned char>(value[index]);
        if (first == 0U) {
            return false;
        }
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU) {
            if (index + 1U >= value.size() || !utf8Continuation(static_cast<unsigned char>(value[index + 1U]))) {
                return false;
            }
            index += 2U;
            continue;
        }
        if (first == 0xE0U) {
            if (index + 2U >= value.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0xA0U || second > 0xBFU ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 2U]))) {
                return false;
            }
            index += 3U;
            continue;
        }
        if ((first >= 0xE1U && first <= 0xECU) || (first >= 0xEEU && first <= 0xEFU)) {
            if (index + 2U >= value.size() ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 1U])) ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 2U]))) {
                return false;
            }
            index += 3U;
            continue;
        }
        if (first == 0xEDU) {
            if (index + 2U >= value.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x80U || second > 0x9FU ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 2U]))) {
                return false;
            }
            index += 3U;
            continue;
        }
        if (first == 0xF0U) {
            if (index + 3U >= value.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x90U || second > 0xBFU ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 2U])) ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 3U]))) {
                return false;
            }
            index += 4U;
            continue;
        }
        if (first >= 0xF1U && first <= 0xF3U) {
            if (index + 3U >= value.size() ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 1U])) ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 2U])) ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 3U]))) {
                return false;
            }
            index += 4U;
            continue;
        }
        if (first == 0xF4U) {
            if (index + 3U >= value.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x80U || second > 0x8FU ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 2U])) ||
                !utf8Continuation(static_cast<unsigned char>(value[index + 3U]))) {
                return false;
            }
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

void validateWorldSceneIdentity(const World& world, const World::Entity entity, const SceneEntityId id) {
    if (!world.alive(entity)) {
        throw std::invalid_argument("Scene identity entity must be live");
    }
    if (!id.valid()) {
        throw std::invalid_argument("Scene identity must be nonzero");
    }

    bool duplicate = false;
    world.each<WorldSceneIdentity>([&](const World::Entity candidate, const WorldSceneIdentity& identity) {
        if (candidate != entity && identity.id == id) {
            duplicate = true;
        }
    });
    if (duplicate) {
        throw std::invalid_argument("Scene identity is already assigned");
    }
}

} // namespace

bool validWorldSceneName(const std::string_view name) noexcept {
    return validUtf8(name);
}

void setWorldSceneIdentity(World& world,
                           const World::Entity entity,
                           const SceneEntityId id,
                           const std::string_view name) {
    if (!validWorldSceneName(name)) {
        throw std::invalid_argument("Scene identity name must be strict UTF-8 without NUL");
    }
    validateWorldSceneIdentity(world, entity, id);

    WorldSceneIdentity replacement{id, std::string{name}};
    if (WorldSceneIdentity* existing = world.tryGet<WorldSceneIdentity>(entity); existing != nullptr) {
        existing->id = replacement.id;
        existing->name.swap(replacement.name);
        return;
    }
    world.emplace<WorldSceneIdentity>(entity, std::move(replacement));
}

bool clearWorldSceneIdentity(World& world, const World::Entity entity) {
    return world.remove<WorldSceneIdentity>(entity);
}

World::Entity findWorldSceneEntity(const World& world, const SceneEntityId id) {
    if (!id.valid()) {
        throw std::invalid_argument("Scene identity must be nonzero");
    }

    World::Entity match{};
    world.each<WorldSceneIdentity>([&](const World::Entity entity, const WorldSceneIdentity& identity) {
        if (identity.id != id) {
            return;
        }
        if (match.valid()) {
            throw std::logic_error("Scene identity is assigned to multiple entities");
        }
        match = entity;
    });
    return match;
}

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

void setWorldSceneParent(World& world, const World::Entity child, const World::Entity parent) {
    if (!world.alive(child) || !world.alive(parent)) {
        throw std::invalid_argument("Scene hierarchy endpoints must be live entities");
    }
    if (child == parent) {
        throw std::invalid_argument("Scene entity cannot parent itself");
    }

    World::Entity ancestor = parent;
    World::Entity slow = parent;
    World::Entity fast = parent;
    while (world.alive(ancestor)) {
        if (ancestor == child) {
            throw std::invalid_argument("Scene parent would create a hierarchy cycle");
        }
        ancestor = nextHierarchyParent(world, ancestor);
        slow = nextHierarchyParent(world, slow);
        fast = nextHierarchyParent(world, nextHierarchyParent(world, fast));
        if (slow.valid() && slow == fast) {
            throw std::invalid_argument("Scene parent chain already contains a hierarchy cycle");
        }
    }

    if (WorldSceneParent* existing = world.tryGet<WorldSceneParent>(child); existing != nullptr) {
        existing->parent = parent;
    } else {
        world.emplace<WorldSceneParent>(child, WorldSceneParent{parent});
    }
}

bool clearWorldSceneParent(World& world, const World::Entity child) {
    return world.remove<WorldSceneParent>(child);
}

void WorldSceneExtractor::ensureWorld(const World& world) {
    if (historyWorldToken_ != world.instanceToken()) {
        histories_.clear();
        captureEpoch_ = 0U;
        historyWorldToken_ = world.instanceToken();
    }
}

WorldSceneExtractor::History& WorldSceneExtractor::historyFor(const World::Entity entity) {
    const std::size_t requiredSize = static_cast<std::size_t>(entity.index) + 1U;
    if (histories_.size() < requiredSize) {
        histories_.resize(requiredSize);
    }
    return histories_[entity.index];
}

void WorldSceneExtractor::initializeHistory(History& history,
                                            const World::Entity entity,
                                            const WorldSceneTransform& transform) noexcept {
    history.entity = entity;
    history.previous = transform.current;
    history.current = transform.current;
    history.discontinuityRevision = transform.discontinuityRevision;
    history.initialized = true;
}

void WorldSceneExtractor::resetResolutionEntries() {
    resolutionPath_.clear();
    ++resolutionEpoch_;
    if (resolutionEpoch_ == 0U) {
        resolutionEntries_.clear();
        resolutionEpoch_ = 1U;
    }
}

WorldSceneExtractor::ResolutionEntry& WorldSceneExtractor::resolutionFor(const World::Entity entity) {
    const std::size_t requiredSize = static_cast<std::size_t>(entity.index) + 1U;
    if (resolutionEntries_.size() < requiredSize) {
        resolutionEntries_.resize(requiredSize);
    }
    ResolutionEntry& entry = resolutionEntries_[entity.index];
    if (entry.epoch != resolutionEpoch_ || entry.entity != entity) {
        entry = ResolutionEntry{};
        entry.entity = entity;
        entry.epoch = resolutionEpoch_;
    }
    return entry;
}

void WorldSceneExtractor::invalidateResolutionPath() noexcept {
    for (const World::Entity entity : resolutionPath_) {
        resolutionEntries_[entity.index].state = ResolutionState::Invalid;
    }
}

bool WorldSceneExtractor::resolveWorldTransform(const World& world, World::Entity entity, const float alpha) {
    resolutionPath_.clear();
    Mat4 parentWorld = Mat4::identity();

    for (;;) {
        ResolutionEntry& entry = resolutionFor(entity);
        if (entry.state == ResolutionState::Resolved) {
            parentWorld = entry.world;
            break;
        }
        if (entry.state == ResolutionState::Invalid) {
            invalidateResolutionPath();
            return false;
        }
        if (entry.state == ResolutionState::Visiting) {
            invalidateResolutionPath();
            return false;
        }

        entry.state = ResolutionState::Visiting;
        resolutionPath_.push_back(entity);

        const WorldSceneTransform* transform = world.tryGet<WorldSceneTransform>(entity);
        if (transform != nullptr && !finite(transform->current)) {
            invalidateResolutionPath();
            return false;
        }

        const WorldSceneParent* parent = world.tryGet<WorldSceneParent>(entity);
        if (parent == nullptr || !world.alive(parent->parent)) {
            break;
        }
        entity = parent->parent;
    }

    for (auto path = resolutionPath_.rbegin(); path != resolutionPath_.rend(); ++path) {
        const World::Entity pathEntity = *path;
        const WorldSceneTransform* transform = world.tryGet<WorldSceneTransform>(pathEntity);
        Mat4 local = Mat4::identity();
        if (transform != nullptr) {
            History& history = historyFor(pathEntity);
            if (!history.initialized || history.entity != pathEntity ||
                history.discontinuityRevision != transform->discontinuityRevision ||
                !finite(history.previous) || !finite(history.current)) {
                initializeHistory(history, pathEntity, *transform);
            }
            local = compose(interpolate(history.previous, history.current, alpha));
        }
        if (!finiteMat4(local)) {
            invalidateResolutionPath();
            return false;
        }

        ResolutionEntry& entry = resolutionFor(pathEntity);
        entry.world = parentWorld * local;
        if (!finiteMat4(entry.world)) {
            invalidateResolutionPath();
            return false;
        }
        entry.state = ResolutionState::Resolved;
        parentWorld = entry.world;
    }
    return true;
}

void WorldSceneExtractor::resetSimulationState(const World& world) {
    ensureWorld(world);
    histories_.reserve(world.entityCapacity());
    world.each<WorldSceneTransform>([&](const World::Entity entity, const WorldSceneTransform& transform) {
        History& history = historyFor(entity);
        initializeHistory(history, entity, transform);
        history.lastCaptureEpoch = captureEpoch_;
    });
}

void WorldSceneExtractor::invalidateSimulationState() noexcept {
    histories_.clear();
    historyWorldToken_ = 0U;
    captureEpoch_ = 0U;
    resolutionEpoch_ = 0U;
    resolutionEntries_.clear();
    resolutionPath_.clear();
}

void WorldSceneExtractor::prepareSimulationStep(const World& world) {
    ensureWorld(world);
    histories_.reserve(world.entityCapacity());
    world.each<WorldSceneTransform>([&](const World::Entity entity, const WorldSceneTransform& transform) {
        History& history = historyFor(entity);
        if (!history.initialized || history.entity != entity ||
            history.discontinuityRevision != transform.discontinuityRevision ||
            !finite(history.current) || !finite(transform.current)) {
            initializeHistory(history, entity, transform);
        }
    });
}

void WorldSceneExtractor::captureSimulationStep(const World& world) {
    ensureWorld(world);
    histories_.reserve(world.entityCapacity());
    ++captureEpoch_;
    if (captureEpoch_ == 0U) {
        histories_.clear();
        captureEpoch_ = 1U;
    }
    const std::uint64_t previousCaptureEpoch = captureEpoch_ - 1U;
    world.each<WorldSceneTransform>([&](const World::Entity entity, const WorldSceneTransform& transform) {
        History& history = historyFor(entity);
        if (!history.initialized || history.entity != entity ||
            history.discontinuityRevision != transform.discontinuityRevision ||
            !finite(history.current) || !finite(transform.current) ||
            history.lastCaptureEpoch != previousCaptureEpoch) {
            initializeHistory(history, entity, transform);
        } else {
            history.previous = history.current;
            history.current = transform.current;
        }
        history.lastCaptureEpoch = captureEpoch_;
    });
}

const SceneRenderList& WorldSceneExtractor::build(const World& world, const double interpolationAlpha) {
    ensureWorld(world);
    histories_.reserve(world.entityCapacity());
    const double clampedAlpha = std::isfinite(interpolationAlpha)
        ? std::clamp(interpolationAlpha, 0.0, 1.0)
        : 0.0;
    const float alpha = static_cast<float>(clampedAlpha);
    pendingItems_.clear();
    resetResolutionEntries();
    world.each<WorldSceneTransform, WorldSceneRenderable>(
        [&](const World::Entity entity, const WorldSceneTransform& transform, const WorldSceneRenderable& renderable) {
            const MeshBounds& bounds = renderable.localBounds;
            if (!renderable.visible || !validMeshBounds(bounds) || !finite(transform.current)) {
                return;
            }

            if (!resolveWorldTransform(world, entity, alpha)) {
                return;
            }
            const Mat4& model = resolutionFor(entity).world;
            const Vec3 worldCenter = transformPoint(model, bounds.center);
            const float worldRadius = bounds.radius * conservativeRadiusScale(model);
            if (!finiteVec3(worldCenter) || !std::isfinite(worldRadius)) {
                return;
            }

            pendingItems_.push_back(PendingItem{entity,
                                                SceneRenderItem{worldCenter,
                                                                worldRadius,
                                                                renderable.mesh,
                                                                model,
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
    if (!validMeshBounds(bounds)) {
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
