#include "renderer/SceneRenderer.hpp"
#include "renderer/Renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
namespace {

int gFailureCount = 0;

template <typename T, typename U>
void expectEqual(std::string_view context, const T& actual, const U& expected) {
    if (actual != expected) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}

template <typename T, typename U>
void expectNotEqual(std::string_view context, const T& actual, const U& expectedDifferent) {
    if (actual == expectedDifferent) {
        std::cerr << "[FAILED] " << context << ": expected value to differ from " << expectedDifferent << ", got same value\n";
        ++gFailureCount;
    }
}

template <typename F>
void expectNoThrow(std::string_view context, F&& callable) {
    try {
        callable();
    } catch (const std::exception& e) {
        std::cerr << "[FAILED] " << context << ": unexpected exception " << e.what() << '\n';
        ++gFailureCount;
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": unexpected non-std exception\n";
        ++gFailureCount;
    }
}

template <typename F>
void expectThrowsRuntimeError(std::string_view context, F&& callable) {
    try {
        callable();
        std::cerr << "[FAILED] " << context << ": expected runtime_error but no exception thrown\n";
        ++gFailureCount;
    } catch (const std::runtime_error&) {
        // expected
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": expected runtime_error but threw different exception\n";
        ++gFailureCount;
    }
}

template <typename T, typename U>
void expectGreaterOrEqual(std::string_view context, const T& actual, const U& minimum) {
    if (actual < minimum) {
        std::cerr << "[FAILED] " << context << ": expected at least " << minimum << ", got " << actual << '\n';
        ++gFailureCount;
    }
}
template <typename T, typename U>
void expectNearly(std::string_view context, const T& actual, const U& expected, const float epsilon = 1.0e-5f) {
    const auto delta = std::fabs(static_cast<float>(actual) - static_cast<float>(expected));
    if (delta > epsilon) {
        std::cerr << "[FAILED] " << context << ": expected nearly " << expected << ", got " << actual << '\n';
        ++gFailureCount;
    }
}

void expectVec3Nearly(std::string_view context, const ve::Vec3& actual, const ve::Vec3 expected, const float epsilon = 1.0e-5f) {
    const std::string label = std::string(context);
    expectNearly(label + " x", actual.x, expected.x, epsilon);
    expectNearly(label + " y", actual.y, expected.y, epsilon);
    expectNearly(label + " z", actual.z, expected.z, epsilon);
}

[[nodiscard]] ve::Vec3 transformImportedModelCenter(const ve::MeshBounds& localBounds) {
    constexpr float importedModelYawRadians = -0.35f;
    constexpr float importedModelScale = 0.78f;
    const float yawCos = std::cos(importedModelYawRadians);
    const float yawSin = std::sin(importedModelYawRadians);
    return {importedModelScale * (yawCos * localBounds.center.x + yawSin * localBounds.center.z),
            1.0f + (importedModelScale * localBounds.center.y),
            importedModelScale * (-yawSin * localBounds.center.x + yawCos * localBounds.center.z) - 3.35f};
}


[[nodiscard]] float transformImportedModelRadius(const ve::MeshBounds& localBounds) {
    constexpr float importedModelScale = 0.78f;
    return localBounds.radius * importedModelScale;
}


} // namespace

int main() {
    using ve::DemoSceneRenderer;
    const std::uint64_t widerTriangleCount = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1ULL;
    ve::RenderStats renderStats{};
    renderStats.triangleCount = widerTriangleCount;
    expectEqual("RenderStats::triangleCount holds >uint32_t::max() without narrowing", renderStats.triangleCount, widerTriangleCount);
    renderStats.sceneTriangleCount = widerTriangleCount;
    expectEqual("RenderStats::sceneTriangleCount holds >uint32_t::max() without narrowing", renderStats.sceneTriangleCount, widerTriangleCount);

    expectEqual("requiredItemCount(4, 5)", DemoSceneRenderer::requiredItemCount(4U, 5U), static_cast<std::size_t>(4U * 5U + DemoSceneRenderer::kFixedItemCount));
    expectEqual("requiredItemCount(31, 66)", DemoSceneRenderer::requiredItemCount(31U, 66U), static_cast<std::size_t>(31U * 66U + DemoSceneRenderer::kFixedItemCount));
 
    DemoSceneRenderer renderer;
    const ve::MeshBounds importedModelLocalBounds = {{1.2f, 0.35f, -0.9f}, 1.45f, true};
    const auto importedModelCenter = transformImportedModelCenter(importedModelLocalBounds);
    const auto importedModelRadius = transformImportedModelRadius(importedModelLocalBounds);
    renderer.setImportedModelBounds(importedModelLocalBounds);

    const auto& first = renderer.build(0.125, 4U, 5U);
    const auto firstSize = first.size();
    const auto firstCapacity = first.capacity();
    const auto firstImportedItemIndex = first.size() - 1U;
    const auto expectedFirstSize = (4U * 5U) + DemoSceneRenderer::kFixedItemCount;
    expectEqual("build(4, 5) size", firstSize, expectedFirstSize);
    expectEqual("build(4, 5) imported model appended", static_cast<int>(first[firstImportedItemIndex].mesh), static_cast<int>(ve::SceneMeshId::ImportedModel));
    expectVec3Nearly("build(4, 5) imported model transformed center", first[firstImportedItemIndex].boundsCenter, importedModelCenter);
    expectNearly("build(4, 5) imported model transformed radius", first[firstImportedItemIndex].boundsRadius, importedModelRadius);
    constexpr std::uint32_t largeRows = 65535U;
    constexpr std::uint32_t largeColumns = 65535U;
    constexpr std::uint64_t largeExpected = static_cast<std::uint64_t>(largeRows) * static_cast<std::uint64_t>(largeColumns)
                                           + DemoSceneRenderer::kFixedItemCount;
    expectEqual("requiredItemCount(65535, 65535)",
                DemoSceneRenderer::requiredItemCount(largeRows, largeColumns),
                static_cast<std::size_t>(largeExpected));

    expectNoThrow("validateMaterialGridDimensions(60000, 60000)", [] {
        DemoSceneRenderer::validateMaterialGridDimensions(60000U, 60000U);
    });
    expectThrowsRuntimeError("validateMaterialGridDimensions overflow", [] {
        DemoSceneRenderer::validateMaterialGridDimensions(65535U, 65537U);
    });

    const auto firstGridRange = first.materialGridRange();
    expectEqual("build(4, 5) material grid valid", firstGridRange.valid, true);
    expectEqual("build(4, 5) material grid first item", firstGridRange.firstItem, static_cast<std::size_t>(5));
    expectEqual("build(4, 5) material grid rows", firstGridRange.rows, 4U);
    expectEqual("build(4, 5) material grid columns", firstGridRange.columns, 5U);
    const auto firstGridItemCount = static_cast<std::size_t>(firstGridRange.rows) * static_cast<std::size_t>(firstGridRange.columns);
    expectGreaterOrEqual("build(4, 5) trailing item remains outside grid", firstSize, firstGridRange.firstItem + firstGridItemCount + 1U);
    const auto& firstGridTiles = first.materialGridTiles();
    expectEqual("build(4, 5) materialGridTiles count", firstGridTiles.size(), static_cast<std::size_t>(1));
    if (firstGridTiles.size() == 1U) {
        const auto& firstTile = firstGridTiles.front();
        expectEqual("build(4, 5) materialGridTile row begin", firstTile.rowBegin, 0U);
        expectEqual("build(4, 5) materialGridTile row end", firstTile.rowEnd, 4U);
        expectEqual("build(4, 5) materialGridTile column begin", firstTile.columnBegin, 0U);
        expectEqual("build(4, 5) materialGridTile column end", firstTile.columnEnd, 5U);
        expectEqual("build(4, 5) materialGridTile itemCount", firstTile.itemCount, static_cast<std::size_t>(20));
        expectEqual("build(4, 5) materialGridTile center x finite", std::isfinite(firstTile.boundsCenter.x), true);
        expectEqual("build(4, 5) materialGridTile center y finite", std::isfinite(firstTile.boundsCenter.y), true);
        expectEqual("build(4, 5) materialGridTile center z finite", std::isfinite(firstTile.boundsCenter.z), true);
        expectEqual("build(4, 5) materialGridTile radius positive", firstTile.boundsRadius > 0.0f, true);
        expectEqual("build(4, 5) materialGridTile radius finite", std::isfinite(firstTile.boundsRadius), true);
    }
    {
        DemoSceneRenderer edgeRenderer;
        const auto& defaultTileGrid = edgeRenderer.build(0.25, 17U, 17U);
        const auto& defaultTileGridTiles = defaultTileGrid.materialGridTiles();
        const auto defaultTileGridTileCount = defaultTileGridTiles.size();
        const auto defaultTileGridItemCount = defaultTileGrid.size();
        expectEqual("build(17, 17) default tile size materialGridTiles count", defaultTileGridTileCount, static_cast<std::size_t>(4));
        expectEqual("build(17, 17) default tile size scene size", defaultTileGridItemCount, static_cast<std::size_t>(17U * 17U + DemoSceneRenderer::kFixedItemCount));
        if (defaultTileGridTiles.size() == 4U) {
            const auto& tile00 = defaultTileGridTiles[0];
            expectEqual("build(17, 17) default materialGridTile[0] row begin", tile00.rowBegin, 0U);
            expectEqual("build(17, 17) default materialGridTile[0] row end", tile00.rowEnd, 16U);
            expectEqual("build(17, 17) default materialGridTile[0] column begin", tile00.columnBegin, 0U);
            expectEqual("build(17, 17) default materialGridTile[0] column end", tile00.columnEnd, 16U);
            expectEqual("build(17, 17) default materialGridTile[0] itemCount", tile00.itemCount, static_cast<std::size_t>(256));

            const auto& tile01 = defaultTileGridTiles[1];
            expectEqual("build(17, 17) default materialGridTile[1] row begin", tile01.rowBegin, 0U);
            expectEqual("build(17, 17) default materialGridTile[1] row end", tile01.rowEnd, 16U);
            expectEqual("build(17, 17) default materialGridTile[1] column begin", tile01.columnBegin, 16U);
            expectEqual("build(17, 17) default materialGridTile[1] column end", tile01.columnEnd, 17U);
            expectEqual("build(17, 17) default materialGridTile[1] itemCount", tile01.itemCount, static_cast<std::size_t>(16));

            const auto& tile10 = defaultTileGridTiles[2];
            expectEqual("build(17, 17) default materialGridTile[2] row begin", tile10.rowBegin, 16U);
            expectEqual("build(17, 17) default materialGridTile[2] row end", tile10.rowEnd, 17U);
            expectEqual("build(17, 17) default materialGridTile[2] column begin", tile10.columnBegin, 0U);
            expectEqual("build(17, 17) default materialGridTile[2] column end", tile10.columnEnd, 16U);
            expectEqual("build(17, 17) default materialGridTile[2] itemCount", tile10.itemCount, static_cast<std::size_t>(16));

            const auto& tile11 = defaultTileGridTiles[3];
            expectEqual("build(17, 17) default materialGridTile[3] row begin", tile11.rowBegin, 16U);
            expectEqual("build(17, 17) default materialGridTile[3] row end", tile11.rowEnd, 17U);
            expectEqual("build(17, 17) default materialGridTile[3] column begin", tile11.columnBegin, 16U);
            expectEqual("build(17, 17) default materialGridTile[3] column end", tile11.columnEnd, 17U);
            expectEqual("build(17, 17) default materialGridTile[3] itemCount", tile11.itemCount, static_cast<std::size_t>(1));
        }

        const auto& customTileGrid = edgeRenderer.build(0.5, 17U, 17U, 4U, 4U);
        const auto& customTileGridTiles = customTileGrid.materialGridTiles();
        expectEqual("build(17,17,4,4) materialGridTiles count", customTileGridTiles.size(), static_cast<std::size_t>(25));
        expectEqual("build(17,17,4,4) preserves item count", customTileGrid.size(), defaultTileGridItemCount);
        expectNotEqual("build(17,17,4,4) changes materialGridTiles count from default", customTileGridTiles.size(), defaultTileGridTileCount);
        if (customTileGridTiles.size() == 25U) {
            const auto& tile00 = customTileGridTiles[0];
            expectEqual("build(17,17,4,4) tile[0] row begin", tile00.rowBegin, 0U);
            expectEqual("build(17,17,4,4) tile[0] row end", tile00.rowEnd, 4U);
            expectEqual("build(17,17,4,4) tile[0] column begin", tile00.columnBegin, 0U);
            expectEqual("build(17,17,4,4) tile[0] column end", tile00.columnEnd, 4U);
            expectEqual("build(17,17,4,4) tile[0] itemCount", tile00.itemCount, static_cast<std::size_t>(16));

            const auto& tile24 = customTileGridTiles[24];
            expectEqual("build(17,17,4,4) tile[24] row begin", tile24.rowBegin, 16U);
            expectEqual("build(17,17,4,4) tile[24] row end", tile24.rowEnd, 17U);
            expectEqual("build(17,17,4,4) tile[24] column begin", tile24.columnBegin, 16U);
            expectEqual("build(17,17,4,4) tile[24] column end", tile24.columnEnd, 17U);
            expectEqual("build(17,17,4,4) tile[24] itemCount", tile24.itemCount, static_cast<std::size_t>(1));
        }

        const auto& revertedTileGrid = edgeRenderer.build(0.75, 17U, 17U);
        const auto& revertedTileGridTiles = revertedTileGrid.materialGridTiles();
        expectEqual("build(17,17) after custom returns to 16x16 default tiling", revertedTileGridTiles.size(), static_cast<std::size_t>(4));
        expectEqual("build(17,17) after custom preserves item count", revertedTileGrid.size(), defaultTileGridItemCount);
        if (revertedTileGridTiles.size() == 4U) {
            const auto& tile00 = revertedTileGridTiles[0];
            expectEqual("build(17,17) after custom tile[0] row begin", tile00.rowBegin, 0U);
            expectEqual("build(17,17) after custom tile[0] row end", tile00.rowEnd, 16U);
            expectEqual("build(17,17) after custom tile[0] column begin", tile00.columnBegin, 0U);
            expectEqual("build(17,17) after custom tile[0] column end", tile00.columnEnd, 16U);
            expectEqual("build(17,17) after custom tile[0] itemCount", tile00.itemCount, static_cast<std::size_t>(256));
        }
    }

    const auto firstAnimatedItemM00 = first[0].model.m[0];
    const auto firstStaticItemMesh = static_cast<int>(first[5].mesh);
    const auto firstStaticItemBoundsCenterX = first[5].boundsCenter.x;
    const auto firstStaticItemBoundsCenterY = first[5].boundsCenter.y;
    const auto firstStaticItemBoundsCenterZ = first[5].boundsCenter.z;
    const auto firstStaticItemBoundsRadius = first[5].boundsRadius;

    const auto& second = renderer.build(0.5, 4U, 5U);
    expectEqual("build(4, 5) repeated size stability", second.size(), expectedFirstSize);
    expectGreaterOrEqual("build(4, 5) repeated capacity", second.capacity(), firstCapacity);
    expectNotEqual("build(4, 5) animated item updates with elapsed", second[0].model.m[0], firstAnimatedItemM00);
    expectEqual("build(4, 5) static grid item keeps mesh", static_cast<int>(second[5].mesh), firstStaticItemMesh);
    expectEqual("build(4, 5) static grid item keeps bounds center x", second[5].boundsCenter.x, firstStaticItemBoundsCenterX);
    expectEqual("build(4, 5) static grid item keeps bounds center y", second[5].boundsCenter.y, firstStaticItemBoundsCenterY);
    expectEqual("build(4, 5) static grid item keeps bounds center z", second[5].boundsCenter.z, firstStaticItemBoundsCenterZ);
    expectEqual("build(4, 5) static grid item keeps bounds radius", second[5].boundsRadius, firstStaticItemBoundsRadius);
    const auto& third = renderer.build(0.75, 3U, 4U);
    const auto thirdGridRange = third.materialGridRange();
    expectEqual("build(3, 4) material grid valid", thirdGridRange.valid, true);
    expectEqual("build(3, 4) material grid first item", thirdGridRange.firstItem, static_cast<std::size_t>(5));
    expectEqual("build(3, 4) material grid rows", thirdGridRange.rows, 3U);
    expectEqual("build(3, 4) material grid columns", thirdGridRange.columns, 4U);
    const auto thirdGridItemCount = static_cast<std::size_t>(thirdGridRange.rows) * static_cast<std::size_t>(thirdGridRange.columns);
    expectGreaterOrEqual("build(3, 4) trailing item remains outside grid", third.size(), thirdGridRange.firstItem + thirdGridItemCount + 1U);
    const auto expectedThirdSize = (3U * 4U) + DemoSceneRenderer::kFixedItemCount;
    expectNotEqual("build(3, 4) changes size from prior layout", third.size(), firstSize);
    expectEqual("build(3, 4) size follows rows*columns+kFixedItemCount", third.size(), expectedThirdSize);

    {
        ve::SceneRenderList renderList;
        for (std::size_t itemIndex = 0; itemIndex < 20U; ++itemIndex) {
            renderList.push({});
        }
        expectEqual("SceneRenderList::materialGridTilesCoverRange() false by default", renderList.materialGridTilesCoverRange(), false);
        renderList.setMaterialGridRange(1U, 3U, 5U);
        expectEqual("SceneRenderList::setMaterialGridRange() clears material grid tile coverage", renderList.materialGridTilesCoverRange(), false);
        constexpr float syntheticSphereBoundsRadius = 0.32f;
        for (std::size_t itemIndex = 1U; itemIndex < 16U; ++itemIndex) {
            const auto itemOffset = static_cast<std::uint32_t>(itemIndex - 1U);
            const auto row = itemOffset / 5U;
            const auto column = itemOffset - row * 5U;
            renderList[itemIndex].mesh = ve::SceneMeshId::Sphere;
            renderList[itemIndex].boundsRadius = syntheticSphereBoundsRadius;
            renderList[itemIndex].boundsCenter = {(static_cast<float>(column) - 2.0f) * 2.0f, 0.28f, -4.4f - (static_cast<float>(row) * 1.25f)};
        }
        const auto revisionBeforeFirstRebuild = renderList.materialGridTileRevision();
        renderList.rebuildMaterialGridTiles(16U, 16U);
        const auto revisionAfterRebuild = renderList.materialGridTileRevision();
        expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) covers material grid range", renderList.materialGridTilesCoverRange(), true);
        expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) creates one test tile", renderList.materialGridTiles().size(), static_cast<std::size_t>(1));
        expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) advances materialGridTileRevision by one", revisionAfterRebuild, revisionBeforeFirstRebuild + 1U);
        if (renderList.materialGridTiles().size() == 1U) {
            const auto& homogeneousTile = renderList.materialGridTiles().front();
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile row begin", homogeneousTile.rowBegin, 0U);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile row end", homogeneousTile.rowEnd, 3U);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile column begin", homogeneousTile.columnBegin, 0U);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile column end", homogeneousTile.columnEnd, 5U);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile itemCount", homogeneousTile.itemCount, static_cast<std::size_t>(15));
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile reports same mesh", homogeneousTile.homogeneousMesh, true);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile common mesh", static_cast<int>(homogeneousTile.commonMesh), static_cast<int>(ve::SceneMeshId::Sphere));
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile maxItemBoundsRadius finite", std::isfinite(homogeneousTile.maxItemBoundsRadius), true);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile maxItemBoundsRadius positive", homogeneousTile.maxItemBoundsRadius > 0.0f, true);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile maxItemBoundsRadius bounded by tile radius", homogeneousTile.maxItemBoundsRadius <= homogeneousTile.boundsRadius, true);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) homogeneous tile maxItemBoundsRadius equals per-item radius", homogeneousTile.maxItemBoundsRadius, syntheticSphereBoundsRadius);
        }

        const auto expectedMaterialGridTileCount = renderList.materialGridTiles().size();
        renderList.push({});
        expectEqual("SceneRenderList::push(non-grid-item) keeps materialGridTilesCoverRange() true", renderList.materialGridTilesCoverRange(), true);
        expectEqual("SceneRenderList::push(non-grid-item) keeps materialGridTileRevision", renderList.materialGridTileRevision(), revisionAfterRebuild);
        expectEqual("SceneRenderList::push(non-grid-item) keeps materialGridTiles cache size", renderList.materialGridTiles().size(), expectedMaterialGridTileCount);

        const auto revisionBeforeOutsideMutation = renderList.materialGridTileRevision();
        renderList[0].mesh = ve::SceneMeshId::Cube;
        expectEqual("SceneRenderList::operator[](outside materialGridRange) keeps material grid tile coverage", renderList.materialGridTilesCoverRange(), true);
        expectEqual("SceneRenderList::operator[](outside materialGridRange) keeps materialGridTiles cache size", renderList.materialGridTiles().size(), expectedMaterialGridTileCount);
        expectEqual("SceneRenderList::operator[](outside materialGridRange) keeps materialGridTileRevision", renderList.materialGridTileRevision(), revisionBeforeOutsideMutation);

        const auto revisionBeforeInsideMutation = renderList.materialGridTileRevision();
        renderList[2].mesh = ve::SceneMeshId::Cube;
        const auto revisionAfterInsideMutation = renderList.materialGridTileRevision();
        expectEqual("SceneRenderList::operator[](inside materialGridRange) clears material grid tile coverage", renderList.materialGridTilesCoverRange(), false);
        expectEqual("SceneRenderList::operator[](inside materialGridRange) clears materialGridTiles cache", renderList.materialGridTiles().empty(), true);
        expectEqual("SceneRenderList::operator[](inside materialGridRange) advances materialGridTileRevision by one", revisionAfterInsideMutation, revisionBeforeInsideMutation + 1U);

        const auto revisionBeforeSecondRebuild = renderList.materialGridTileRevision();
        renderList.rebuildMaterialGridTiles(16U, 16U);
        const auto revisionAfterSecondRebuild = renderList.materialGridTileRevision();
        expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) mixed tile recreates one test tile", renderList.materialGridTiles().size(), static_cast<std::size_t>(1));
        expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) reports material grid coverage", renderList.materialGridTilesCoverRange(), true);
        expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) advances materialGridTileRevision by one", revisionAfterSecondRebuild, revisionBeforeSecondRebuild + 1U);
        const auto& mixedRange = renderList.materialGridRange();
        expectEqual("SceneRenderList::materialGridRange() remains valid for mixed rebuild", mixedRange.valid, true);
        expectEqual("SceneRenderList::materialGridRange() preserves firstItem for mixed rebuild", mixedRange.firstItem, static_cast<std::size_t>(1));
        expectEqual("SceneRenderList::materialGridRange() preserves rows for mixed rebuild", mixedRange.rows, 3U);
        expectEqual("SceneRenderList::materialGridRange() preserves columns for mixed rebuild", mixedRange.columns, 5U);
        if (renderList.materialGridTiles().size() == 1U) {
            const auto& mixedTile = renderList.materialGridTiles().front();
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) mixed tile preserves range begin", mixedTile.rowBegin, 0U);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) mixed tile preserves range end", mixedTile.rowEnd, 3U);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) mixed tile preserves column begin", mixedTile.columnBegin, 0U);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) mixed tile preserves column end", mixedTile.columnEnd, 5U);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) mixed tile preserves itemCount", mixedTile.itemCount, static_cast<std::size_t>(15));
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) mixed tile reports non-homogeneous mesh", mixedTile.homogeneousMesh, false);
            expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) mixed tile keeps first mesh as common", static_cast<int>(mixedTile.commonMesh), static_cast<int>(ve::SceneMeshId::Sphere));
        }

        renderList.setMaterialGridRange(2U, 2U, 2U);
        expectEqual("SceneRenderList::setMaterialGridRange(2, 2, 2) clears material grid tile coverage", renderList.materialGridTilesCoverRange(), false);
        expectEqual("SceneRenderList::setMaterialGridRange() clears material grid tiles", renderList.materialGridTiles().empty(), true);

        renderList.setMaterialGridRange(1U, 3U, 5U);
        renderList.rebuildMaterialGridTiles(16U, 16U);
        expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) recreates tile after range reset", renderList.materialGridTiles().size(), static_cast<std::size_t>(1));
        expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) restores full material grid coverage", renderList.materialGridTilesCoverRange(), true);

        renderList.setMaterialGridRange(renderList.size(), 3U, 5U);
        expectEqual("SceneRenderList::setMaterialGridRange() with no remaining grid items clears coverage", renderList.materialGridTilesCoverRange(), false);
        renderList.rebuildMaterialGridTiles(16U, 16U);
        expectEqual("SceneRenderList::rebuildMaterialGridTiles(16, 16) fails to set coverage for out-of-range grid", renderList.materialGridTilesCoverRange(), false);

        renderList.clear();
        const auto& clearedRange = renderList.materialGridRange();
        expectEqual("SceneRenderList::clear() resets material grid validity", clearedRange.valid, false);
        expectEqual("SceneRenderList::clear() resets material grid first item", clearedRange.firstItem, static_cast<std::size_t>(0));
        expectEqual("SceneRenderList::clear() resets material grid rows", clearedRange.rows, 0U);
        expectEqual("SceneRenderList::clear() resets material grid columns", clearedRange.columns, 0U);
        expectEqual("SceneRenderList::clear() clears material grid tiles", renderList.materialGridTiles().empty(), true);
        expectEqual("SceneRenderList::clear() clears material grid tile coverage", renderList.materialGridTilesCoverRange(), false);
    }
    if (gFailureCount == 0) {
        return 0;
    }

    std::cerr << "SceneRenderer tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
