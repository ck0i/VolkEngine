#pragma once

#include "core/Math.hpp"
#include "renderer/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ve {

enum class SceneMeshId : std::uint8_t {
    Cube,
    Sphere,
    GroundPlane,
    ImportedModel
};

struct alignas(16) RenderMaterial {
    Vec4 albedoRoughness;
    Vec4 emissiveMetallic;
    Vec4 flags;
};

struct SceneRenderItem {
    Vec3 boundsCenter{};
    float boundsRadius = 0.0f;
    SceneMeshId mesh = SceneMeshId::Cube;
    Mat4 model{};
    RenderMaterial material{};
};

static_assert(sizeof(SceneRenderItem) == 144, "SceneRenderItem layout changed; keep cull fields packed before model/material");
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
    SceneMeshId commonMesh = SceneMeshId::Cube;
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

private:
    void invalidateMaterialGridTiles() noexcept;
    [[nodiscard]] bool indexInMaterialGridRange(std::size_t index) const noexcept;

    std::vector<SceneRenderItem> items_;
    SceneGridRange materialGridRange_{};
    bool materialGridTilesCoverRange_ = false;
    std::uint64_t materialGridTileRevision_ = 0;
    std::vector<SceneGridTile> materialGridTiles_{};
};

class DemoSceneRenderer {
public:
    static constexpr std::uint64_t kFixedItemCount = 7;

    [[nodiscard]] static std::size_t requiredItemCount(std::uint32_t materialGridRows, std::uint32_t materialGridColumns);
    static void validateMaterialGridDimensions(std::uint32_t materialGridRows, std::uint32_t materialGridColumns);

    void setImportedModelBounds(const MeshBounds& bounds) noexcept;

    [[nodiscard]] const SceneRenderList& build(double elapsedSeconds,
                                               std::uint32_t materialGridRows = 4,
                                               std::uint32_t materialGridColumns = 5,
                                               std::uint32_t materialGridTileRows = kDefaultMaterialGridTileRows,
                                               std::uint32_t materialGridTileColumns = kDefaultMaterialGridTileColumns);

private:
    static constexpr std::size_t kAnimatedItemCount = 5;
    static constexpr std::uint32_t kDefaultMaterialGridTileRows = 16;
    static constexpr std::uint32_t kDefaultMaterialGridTileColumns = 16;
    static constexpr Vec3 kImportedModelTranslation{0.0f, 1.0f, -3.35f};
    static constexpr float kImportedModelRotationY = -0.35f;
    static constexpr float kImportedModelScale = 0.78f;
    static constexpr MeshBounds kFallbackImportedModelBounds{{}, 1.0f, true};

    [[nodiscard]] static Mat4 importedModelMatrix();
    [[nodiscard]] static Vec3 transformPoint(const Mat4& matrix, Vec3 point) noexcept;
    [[nodiscard]] SceneRenderItem importedModelItem() const;

    void invalidateStaticSceneLayout() noexcept;
    void ensureStaticSceneLayout(std::uint32_t materialGridRows,
                                 std::uint32_t materialGridColumns,
                                 std::uint32_t materialGridTileRows,
                                 std::uint32_t materialGridTileColumns,
                                 std::size_t requiredItems);
    void writeAnimatedItems(double elapsedSeconds);

    MeshBounds importedModelLocalBounds_ = kFallbackImportedModelBounds;
    SceneRenderList renderList_;
    std::uint32_t cachedMaterialGridRows_ = 0;
    std::uint32_t cachedMaterialGridColumns_ = 0;
    std::uint32_t cachedMaterialGridTileRows_ = 0;
    std::uint32_t cachedMaterialGridTileColumns_ = 0;
};

} // namespace ve
