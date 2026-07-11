#include "renderer/Lighting.hpp"

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

template <typename F>
bool throwsInvalidArgument(F&& function) {
    try {
        function();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

ve::RenderLocalLight pointLight(const ve::Vec3 position, const float range) {
    ve::RenderLocalLight light;
    light.positionRange = {position.x, position.y, position.z, range};
    light.colorIntensity = {1.0F, 0.8F, 0.6F, 4.0F};
    light.directionOuterCone = {0.0F, -1.0F, 0.0F, 0.0F};
    light.parameters = {
        static_cast<std::uint32_t>(ve::LocalLightType::Point), 0U,
        ve::packUnitFloat(1.0F), 0U};
    return light;
}

ve::RenderLocalLight shadowedSpotLight() {
    ve::RenderLocalLight light = pointLight({0.0F, 2.0F, 0.0F}, 8.0F);
    light.directionOuterCone = {0.0F, -1.0F, 0.0F, 0.7F};
    light.parameters = {
        static_cast<std::uint32_t>(ve::LocalLightType::Spot), 1U,
        ve::packUnitFloat(0.85F), 0U};
    return light;
}

} // namespace

int main() {
    static_assert(static_cast<std::uint32_t>(
                      ve::RenderMaterialClass::Standard) == 0U);
    static_assert(static_cast<std::uint32_t>(
                      ve::RenderMaterialClass::Masked) == 1U);
    static_assert(static_cast<std::uint32_t>(
                      ve::RenderMaterialClass::Emissive) == 7U);

    assert(ve::packUnitFloat(-1.0F) == 0U);
    assert(ve::packUnitFloat(2.0F) == 65535U);
    assert(std::abs(ve::unpackUnitFloat(ve::packUnitFloat(0.37F)) -
                    0.37F) < 0.00002F);

    const ve::RenderLocalLight centered =
        pointLight({-0.5F, 0.0F, 0.0F}, 0.05F);
    const ve::TiledLightLists first = ve::buildTiledLightLists(
        std::span<const ve::RenderLocalLight>{&centered, 1U},
        ve::Mat4::identity(), 32U, 32U);
    const ve::TiledLightLists second = ve::buildTiledLightLists(
        std::span<const ve::RenderLocalLight>{&centered, 1U},
        ve::Mat4::identity(), 32U, 32U);
    assert(first.columns == 2U && first.rows == 2U);
    assert(first.overflowCount == 0U);
    assert(first.headers.size() == second.headers.size());
    assert(first.indices == second.indices);
    for (std::size_t index = 0U; index < first.headers.size(); ++index) {
        assert(first.headers[index].offset == second.headers[index].offset);
        assert(first.headers[index].count == second.headers[index].count);
    }
    assert(first.headers[0].count == 1U);
    assert(first.headers[1].count == 0U);

    std::vector<ve::RenderLocalLight> crowded(
        ve::kMaximumLightsPerTile + 1U,
        pointLight({0.0F, 0.0F, 0.0F}, 2.0F));
    const ve::TiledLightLists overflow = ve::buildTiledLightLists(
        crowded, ve::Mat4::identity(), 32U, 32U);
    assert(overflow.overflowCount == 4U);
    for (const ve::LightTileHeader header : overflow.headers) {
        assert(header.count == ve::kMaximumLightsPerTile);
    }

    ve::RenderDirectionalLight directional;
    std::vector<ve::RenderLocalLight> shadowedSpots(20U,
                                                    shadowedSpotLight());
    const ve::ShadowAtlasAssignment assignment =
        ve::assignShadowAtlasSlots(directional, shadowedSpots);
    assert(assignment.directionalCascadeCount ==
           ve::kDirectionalShadowCascadeCount);
    assert(assignment.localShadowCount ==
           ve::kShadowAtlasSlotCount -
               ve::kDirectionalShadowCascadeCount);
    assert(assignment.overflowCount == 7U);
    for (std::uint32_t index = 0U;
         index < assignment.localShadowCount; ++index) {
        assert(assignment.localLightSlots[index] ==
               static_cast<std::int32_t>(
                   ve::kDirectionalShadowCascadeCount + index));
    }

    std::vector<ve::RenderReflectionProbe> probes{
        {{0.0F, 1.0F, -2.0F, 5.0F}, {1.0F, 0.8F, 0.6F, 0.5F}}};
    ve::validateReflectionProbes(probes);
    probes[0].positionRadius.w = 0.0F;
    assert(throwsInvalidArgument(
        [&] { ve::validateReflectionProbes(probes); }));
    probes.assign(ve::kMaximumReflectionProbes + 1U, {});
    assert(throwsInvalidArgument(
        [&] { ve::validateReflectionProbes(probes); }));

    ve::RenderLocalLight invalid = centered;
    invalid.positionRange.w =
        std::numeric_limits<float>::quiet_NaN();
    assert(throwsInvalidArgument([&] {
        ve::validateLocalLights(
            std::span<const ve::RenderLocalLight>{&invalid, 1U});
    }));

    return 0;
}
