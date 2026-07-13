#include "renderer/Lighting.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ve {
namespace {

[[nodiscard]] bool finiteVec4(const Vec4 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] Vec4 transformPoint(const Mat4& matrix, const Vec3 point) noexcept {
    return {
        matrix.m[0] * point.x + matrix.m[4] * point.y +
            matrix.m[8] * point.z + matrix.m[12],
        matrix.m[1] * point.x + matrix.m[5] * point.y +
            matrix.m[9] * point.z + matrix.m[13],
        matrix.m[2] * point.x + matrix.m[6] * point.y +
            matrix.m[10] * point.z + matrix.m[14],
        matrix.m[3] * point.x + matrix.m[7] * point.y +
            matrix.m[11] * point.z + matrix.m[15],
    };
}

struct TileBounds {
    std::uint32_t minColumn = 0;
    std::uint32_t maxColumn = 0;
    std::uint32_t minRow = 0;
    std::uint32_t maxRow = 0;
    bool visible = false;
};

[[nodiscard]] TileBounds projectLightBounds(
    const RenderLocalLight& light, const Mat4& viewProjection,
    const std::uint32_t width, const std::uint32_t height,
    const std::uint32_t columns, const std::uint32_t rows) noexcept {
    const Vec3 position{light.positionRange.x, light.positionRange.y,
                        light.positionRange.z};
    const float range = light.positionRange.w;
    const Vec4 clip = transformPoint(viewProjection, position);
    if (clip.w + range <= 0.0F) return {};

    if (clip.w <= range) {
        return {0U, columns - 1U, 0U, rows - 1U, true};
    }

    const float inverseW = 1.0F / clip.w;
    const float centerX = (clip.x * inverseW * 0.5F + 0.5F) *
                          static_cast<float>(width);
    const float centerY = (clip.y * inverseW * 0.5F + 0.5F) *
                          static_cast<float>(height);
    const float conservativeDepth = std::max(clip.w - range, 0.0001F);
    const float radiusX = std::abs(viewProjection.m[0]) * range /
                          conservativeDepth * 0.5F *
                          static_cast<float>(width);
    const float radiusY = std::abs(viewProjection.m[5]) * range /
                          conservativeDepth * 0.5F *
                          static_cast<float>(height);
    if (centerX + radiusX < 0.0F ||
        centerX - radiusX >= static_cast<float>(width) ||
        centerY + radiusY < 0.0F ||
        centerY - radiusY >= static_cast<float>(height)) {
        return {};
    }

    const auto toTile = [](const float pixel, const std::uint32_t maximum) {
        const float clamped = std::clamp(
            pixel, 0.0F,
            static_cast<float>(maximum * kLightTileSize - 1U));
        return static_cast<std::uint32_t>(clamped) / kLightTileSize;
    };
    return {
        toTile(centerX - radiusX, columns),
        toTile(centerX + radiusX, columns),
        toTile(centerY - radiusY, rows),
        toTile(centerY + radiusY, rows),
        true,
    };
}

} // namespace

std::uint32_t packUnitFloat(const float value) noexcept {
    return static_cast<std::uint32_t>(
        std::lround(std::clamp(value, 0.0F, 1.0F) * 65535.0F));
}

float unpackUnitFloat(const std::uint32_t value) noexcept {
    return static_cast<float>(std::min(value, 65535U)) / 65535.0F;
}

void validateDirectionalLight(const RenderDirectionalLight& light) {
    const Vec3 direction{light.directionIntensity.x,
                         light.directionIntensity.y,
                         light.directionIntensity.z};
    const float directionLength = length(direction);
    if (!finiteVec4(light.directionIntensity) || !finiteVec4(light.color) ||
        directionLength < 0.999F || directionLength > 1.001F ||
        light.directionIntensity.w < 0.0F || light.color.x < 0.0F ||
        light.color.y < 0.0F || light.color.z < 0.0F ||
        light.parameters[0] > 1U) {
        throw std::invalid_argument("Directional light contains invalid data");
    }
}

void validateEnvironment(const RenderEnvironment& environment) {
    if (!finiteVec4(environment.skyColorIntensity) ||
        !finiteVec4(environment.groundColorIntensity) ||
        !finiteVec4(environment.parameters) ||
        environment.skyColorIntensity.x < 0.0F ||
        environment.skyColorIntensity.y < 0.0F ||
        environment.skyColorIntensity.z < 0.0F ||
        environment.skyColorIntensity.w < 0.0F ||
        environment.groundColorIntensity.x < 0.0F ||
        environment.groundColorIntensity.y < 0.0F ||
        environment.groundColorIntensity.z < 0.0F ||
        environment.groundColorIntensity.w < 0.0F ||
        environment.parameters.x <= 0.0F) {
        throw std::invalid_argument("Environment contains invalid data");
    }
}
void validateReflectionProbes(
    const std::span<const RenderReflectionProbe> probes) {
    if (probes.size() > kMaximumReflectionProbes) {
        throw std::invalid_argument(
            "Reflection probe count exceeds renderer limit");
    }
    for (const RenderReflectionProbe& probe : probes) {
        const float radius = probe.positionRadius.w;
        const float radiusSquared = radius * radius;
        const float inverseRadiusSquared =
            radiusSquared > 0.0F && std::isfinite(radiusSquared)
            ? 1.0F / radiusSquared
            : 0.0F;
        const bool representableRadius =
            radius > 0.0F &&
            inverseRadiusSquared >= std::numeric_limits<float>::min() &&
            std::isfinite(inverseRadiusSquared);
        if (!finiteVec4(probe.positionRadius) ||
            !finiteVec4(probe.tintIntensity) ||
            !representableRadius ||
            probe.tintIntensity.x < 0.0F ||
            probe.tintIntensity.y < 0.0F ||
            probe.tintIntensity.z < 0.0F ||
            probe.tintIntensity.w < 0.0F) {
            throw std::invalid_argument(
                "Reflection probe contains invalid data");
        }
    }
}


void validateLocalLights(const std::span<const RenderLocalLight> lights) {
    if (lights.size() > kMaximumLocalLights) {
        throw std::invalid_argument("Local light count exceeds renderer limit");
    }
    for (const RenderLocalLight& light : lights) {
        if (!finiteVec4(light.positionRange) ||
            !finiteVec4(light.colorIntensity) ||
            !finiteVec4(light.directionOuterCone) ||
            light.positionRange.w <= 0.0F || light.colorIntensity.x < 0.0F ||
            light.colorIntensity.y < 0.0F || light.colorIntensity.z < 0.0F ||
            light.colorIntensity.w < 0.0F || light.parameters[0] > 1U ||
            light.parameters[1] > 1U) {
            throw std::invalid_argument("Local light contains invalid data");
        }
        if (light.parameters[0] ==
            static_cast<std::uint32_t>(LocalLightType::Spot)) {
            const Vec3 direction{light.directionOuterCone.x,
                                 light.directionOuterCone.y,
                                 light.directionOuterCone.z};
            const float directionLength = length(direction);
            const float innerCosine = unpackUnitFloat(light.parameters[2]);
            if (directionLength < 0.999F || directionLength > 1.001F ||
                light.directionOuterCone.w < 0.0F ||
                light.directionOuterCone.w > 1.0F ||
                innerCosine < light.directionOuterCone.w) {
                throw std::invalid_argument("Spot light cone is invalid");
            }
        }
    }
}

TiledLightLists buildTiledLightLists(
    const std::span<const RenderLocalLight> lights,
    const Mat4& viewProjection, const std::uint32_t width,
    const std::uint32_t height) {
    validateLocalLights(lights);
    if (width == 0U || height == 0U) {
        throw std::invalid_argument("Light-list extent must be nonzero");
    }
    TiledLightLists result;
    result.columns = (width + kLightTileSize - 1U) / kLightTileSize;
    result.rows = (height + kLightTileSize - 1U) / kLightTileSize;
    const std::size_t tileCount =
        static_cast<std::size_t>(result.columns) * result.rows;
    result.headers.resize(tileCount);
    result.indices.assign(tileCount * kMaximumLightsPerTile,
                          std::numeric_limits<std::uint32_t>::max());
    for (std::size_t tile = 0; tile < tileCount; ++tile) {
        result.headers[tile].offset =
            static_cast<std::uint32_t>(tile * kMaximumLightsPerTile);
    }

    for (std::uint32_t lightIndex = 0;
         lightIndex < static_cast<std::uint32_t>(lights.size());
         ++lightIndex) {
        const TileBounds bounds = projectLightBounds(
            lights[lightIndex], viewProjection, width, height,
            result.columns, result.rows);
        if (!bounds.visible) continue;
        for (std::uint32_t row = bounds.minRow; row <= bounds.maxRow; ++row) {
            for (std::uint32_t column = bounds.minColumn;
                 column <= bounds.maxColumn; ++column) {
                const std::size_t tile =
                    static_cast<std::size_t>(row) * result.columns + column;
                LightTileHeader& header = result.headers[tile];
                if (header.count < kMaximumLightsPerTile) {
                    result.indices[header.offset + header.count] = lightIndex;
                    ++header.count;
                } else {
                    ++result.overflowCount;
                }
            }
        }
    }
    return result;
}

ShadowAtlasAssignment assignShadowAtlasSlots(
    const RenderDirectionalLight& directional,
    const std::span<const RenderLocalLight> lights) noexcept {
    ShadowAtlasAssignment result;
    result.localLightSlots.fill(-1);
    result.directionalCascadeCount = directional.parameters[0] != 0U
                                         ? kDirectionalShadowCascadeCount
                                         : 0U;
    std::uint32_t nextSlot = result.directionalCascadeCount;
    const std::size_t boundedCount =
        std::min<std::size_t>(lights.size(), kMaximumLocalLights);
    for (std::size_t index = 0; index < boundedCount; ++index) {
        const RenderLocalLight& light = lights[index];
        if (light.parameters[1] == 0U) continue;
        const bool supported = light.parameters[0] ==
            static_cast<std::uint32_t>(LocalLightType::Spot);
        if (!supported || nextSlot >= kShadowAtlasSlotCount) {
            ++result.overflowCount;
            continue;
        }
        result.localLightSlots[index] = static_cast<std::int32_t>(nextSlot++);
        ++result.localShadowCount;
    }
    return result;
}

} // namespace ve
