#pragma once

#include "assets/RuntimeAssets.hpp"
#include "core/Math.hpp"
#include "core/World.hpp"
#include "renderer/Geometry.hpp"
#include "renderer/Lighting.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ve {



struct alignas(16) RenderMaterial {
    Vec4 albedoRoughness;
    Vec4 emissiveMetallic;
    Vec4 flags;
    std::array<TextureAssetHandle, 3> textures{};
};

struct SceneRenderItem {
    Vec3 boundsCenter{};
    float boundsRadius = 0.0f;
    MeshAssetHandle mesh = builtin_assets::kCube;
    Mat4 model{};
    RenderMaterial material{};
};

static_assert(sizeof(SceneRenderItem) == 176,
              "SceneRenderItem layout changed; keep cull fields packed before "
              "model/material");
static_assert(offsetof(SceneRenderItem, boundsCenter) == 0, "SceneRenderItem boundsCenter should stay first for culling");
static_assert(offsetof(SceneRenderItem, boundsRadius) == 12, "SceneRenderItem boundsRadius should stay with boundsCenter");
static_assert(offsetof(SceneRenderItem, mesh) == 16, "SceneRenderItem mesh should stay in the first cache line");
static_assert(offsetof(SceneRenderItem, model) == 32, "SceneRenderItem model offset mismatch");
static_assert(offsetof(SceneRenderItem, material) == 96, "SceneRenderItem material offset mismatch");

struct SceneGridRange {
    std::size_t firstItem = 0;
    std::uint32_t rows = 0;
    std::uint32_t columns = 0;
    bool valid = false;
};

struct SceneGridTile {
    std::uint32_t rowBegin = 0;
    std::uint32_t rowEnd = 0;
    std::uint32_t columnBegin = 0;
    std::uint32_t columnEnd = 0;
    Vec3 boundsCenter{};
    float boundsRadius = 0.0f;
    float maxItemBoundsRadius = 0.0f;
    std::size_t itemCount = 0;
  std::array<unsigned, kRenderMaterialClassCount> materialClassCounts{};
    MeshAssetHandle commonMesh = builtin_assets::kCube;
    bool homogeneousMesh = false;
};

class SceneRenderList {
public:
    void clear() noexcept;

    void reserve(const std::size_t capacity) {
        items_.reserve(capacity);
    }

    void push(const SceneRenderItem& item);

    [[nodiscard]] std::size_t size() const { return items_.size(); }
    [[nodiscard]] std::size_t capacity() const { return items_.capacity(); }
    [[nodiscard]] bool empty() const { return items_.empty(); }

    [[nodiscard]] const SceneRenderItem& operator[](const std::size_t index) const {
        return items_[index];
    }
    [[nodiscard]] SceneRenderItem& operator[](std::size_t index);

    [[nodiscard]] const SceneRenderItem* begin() const { return items_.data(); }
    [[nodiscard]] const SceneRenderItem* end() const { return items_.data() + items_.size(); }

    void setMaterialGridRange(std::size_t firstItem, std::uint32_t rows, std::uint32_t columns) noexcept;

    void rebuildMaterialGridTiles(std::uint32_t tileRows, std::uint32_t tileColumns);

    [[nodiscard]] const SceneGridRange& materialGridRange() const noexcept {
        return materialGridRange_;
    }

    [[nodiscard]] const std::vector<SceneGridTile>& materialGridTiles() const noexcept {
        return materialGridTiles_;
    }

    [[nodiscard]] bool materialGridTilesCoverRange() const noexcept {
        return materialGridTilesCoverRange_;
    }

    [[nodiscard]] std::uint64_t materialGridTileRevision() const noexcept {
        return materialGridTileRevision_;
    }

    void setLocalLights(std::vector<RenderLocalLight> lights);
    [[nodiscard]] std::span<const RenderLocalLight> localLights() const noexcept {
        return localLights_;
    }
    void setDirectionalLight(const RenderDirectionalLight& light);
    [[nodiscard]] const RenderDirectionalLight& directionalLight() const noexcept {
        return directionalLight_;
    }
    void setEnvironment(const RenderEnvironment& environment);
    [[nodiscard]] const RenderEnvironment& environment() const noexcept {
        return environment_;
    }
    void setReflectionProbes(std::vector<RenderReflectionProbe> probes);
    [[nodiscard]] std::span<const RenderReflectionProbe>
    reflectionProbes() const noexcept {
        return reflectionProbes_;
    }

private:
    void invalidateMaterialGridTiles() noexcept;
    [[nodiscard]] bool indexInMaterialGridRange(std::size_t index) const noexcept;

    std::vector<SceneRenderItem> items_;
    SceneGridRange materialGridRange_{};
    bool materialGridTilesCoverRange_ = false;
    std::uint64_t materialGridTileRevision_ = 0;
    std::vector<SceneGridTile> materialGridTiles_{};
    std::vector<RenderLocalLight> localLights_;
    RenderDirectionalLight directionalLight_{};
    std::vector<RenderReflectionProbe> reflectionProbes_;
    RenderEnvironment environment_{};
};

struct SceneEntityId {
    std::uint64_t high = 0U;
    std::uint64_t low = 0U;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return high != 0U || low != 0U;
    }

    constexpr auto operator<=>(const SceneEntityId&) const noexcept = default;
};

struct WorldSceneIdentity {
    SceneEntityId id{};
    std::string name{};
};

[[nodiscard]] bool validWorldSceneName(std::string_view name) noexcept;
void setWorldSceneIdentity(World& world, World::Entity entity, SceneEntityId id, std::string_view name = {});
[[nodiscard]] bool clearWorldSceneIdentity(World& world, World::Entity entity);
[[nodiscard]] World::Entity findWorldSceneEntity(const World& world, SceneEntityId id);
[[nodiscard]] SceneEntityId generateWorldSceneEntityId(const World& world);

struct WorldSceneTransform {
    TransformTRS current{};
    std::uint64_t discontinuityRevision = 0;

    void teleport(const TransformTRS& value) noexcept {
        current = value;
        ++discontinuityRevision;
    }
};

struct WorldSceneParent {
    World::Entity parent{};
};

void setWorldSceneParent(World& world, World::Entity child, World::Entity parent);
[[nodiscard]] bool clearWorldSceneParent(World& world, World::Entity child);

struct WorldSceneRenderable {
    MeshAssetHandle mesh = builtin_assets::kCube;
    RenderMaterial material{};
    MeshBounds localBounds{};
    bool visible = true;
};

class WorldSceneExtractor final {
public:
    void prepareSimulationStep(const World& world);
    void captureSimulationStep(const World& world);
    void resetSimulationState(const World& world);
    void invalidateSimulationState() noexcept;
    [[nodiscard]] const SceneRenderList& build(const World& world, double interpolationAlpha);

private:
    struct History {
        World::Entity entity{};
        TransformTRS previous{};
        TransformTRS current{};
        std::uint64_t discontinuityRevision = 0;
        std::uint64_t lastCaptureEpoch = 0;
        bool initialized = false;
    };

    struct PendingItem {
        World::Entity entity;
        SceneRenderItem item;
    };

    enum class ResolutionState : std::uint8_t {
        Unresolved,
        Visiting,
        Resolved,
        Invalid
    };

    struct ResolutionEntry {
        World::Entity entity{};
        Mat4 world{};
        std::uint64_t epoch = 0;
        ResolutionState state = ResolutionState::Unresolved;
    };

    void ensureWorld(const World& world);
    [[nodiscard]] History& historyFor(const World::Entity entity);
    void initializeHistory(History& history, const World::Entity entity, const WorldSceneTransform& transform) noexcept;
    void resetResolutionEntries();
    [[nodiscard]] ResolutionEntry& resolutionFor(const World::Entity entity);
    void invalidateResolutionPath() noexcept;
    [[nodiscard]] bool resolveWorldTransform(const World& world, World::Entity entity, float alpha);
    std::uint64_t historyWorldToken_ = 0U;
    std::uint64_t captureEpoch_ = 0U;
    std::uint64_t resolutionEpoch_ = 0U;
    std::vector<History> histories_;
    std::vector<PendingItem> pendingItems_;
    std::vector<ResolutionEntry> resolutionEntries_;
    std::vector<World::Entity> resolutionPath_;
    SceneRenderList renderList_;
};

class DemoSceneRenderer {
public:
    static constexpr std::uint64_t kFixedItemCount = 6;

    [[nodiscard]] static std::size_t requiredItemCount(std::uint32_t materialGridRows, std::uint32_t materialGridColumns);
    static void validateMaterialGridDimensions(std::uint32_t materialGridRows, std::uint32_t materialGridColumns);

    void setAuthoredSceneItems(std::vector<SceneRenderItem> items);

    [[nodiscard]] const SceneRenderList& build(double elapsedSeconds,
                                               std::uint32_t materialGridRows = 4,
                                               std::uint32_t materialGridColumns = 5,
                                               std::uint32_t materialGridTileRows = kDefaultMaterialGridTileRows,
                                               std::uint32_t materialGridTileColumns = kDefaultMaterialGridTileColumns);

private:
    static constexpr std::size_t kAnimatedItemCount = 5;
    static constexpr std::uint32_t kDefaultMaterialGridTileRows = 16;
    static constexpr std::uint32_t kDefaultMaterialGridTileColumns = 16;

    void invalidateStaticSceneLayout() noexcept;
    void ensureStaticSceneLayout(std::uint32_t materialGridRows,
                                 std::uint32_t materialGridColumns,
                                 std::uint32_t materialGridTileRows,
                                 std::uint32_t materialGridTileColumns,
                                 std::size_t requiredItems);
    void writeAnimatedItems(double elapsedSeconds);

    std::vector<SceneRenderItem> authoredSceneItems_;
    SceneRenderList renderList_;
    std::uint32_t cachedMaterialGridRows_ = 0;
    std::uint32_t cachedMaterialGridColumns_ = 0;
    std::uint32_t cachedMaterialGridTileRows_ = 0;
    std::uint32_t cachedMaterialGridTileColumns_ = 0;
};

} // namespace ve
