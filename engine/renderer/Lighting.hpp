#pragma once

#include "core/Math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ve {

inline constexpr std::uint32_t kLightTileSize = 16;
inline constexpr std::uint32_t kMaximumLocalLights = 256;
inline constexpr std::uint32_t kMaximumLightsPerTile = 64;
inline constexpr std::uint32_t kShadowAtlasExtent = 2048;
inline constexpr std::uint32_t kShadowAtlasSlotExtent = 512;
inline constexpr std::uint32_t kShadowAtlasSlotCount = 16;
inline constexpr std::uint32_t kDirectionalShadowCascadeCount = 3;
inline constexpr std::uint32_t kMaximumReflectionProbes = 4;

// Values are part of the CPU/GLSL ABI.
enum class LocalLightType : std::uint32_t { Point = 0, Spot = 1 };

enum class RenderMaterialClass : std::uint32_t {
    Standard = 0,
    Masked = 1,
    ClearCoat = 2,
    Foliage = 3,
    Skin = 4,
    Hair = 5,
    Cloth = 6,
    Emissive = 7,
  Landscape = 8,
  Water = 9,
};
inline constexpr std::size_t kRenderMaterialClassCount = 10U;

enum MaterialFeature : std::uint32_t {
    MaterialFeatureNone = 0,
    MaterialFeatureAlphaMask = 1U << 0U,
    MaterialFeatureDoubleSided = 1U << 1U,
  MaterialFeatureGroundGrid = 1U << 2U,
};

struct alignas(16) RenderLocalLight {
    Vec4 positionRange{};       // xyz world position, w range
    Vec4 colorIntensity{};      // linear RGB, w intensity
    Vec4 directionOuterCone{};  // normalized world direction, w outer cosine
    std::array<std::uint32_t, 4> parameters{}; // type, castsShadow, packed inner cosine, reserved
};

struct alignas(16) RenderDirectionalLight {
    Vec4 directionIntensity{0.32152065F, -0.91863042F, 0.22965761F, 4.0F};
    Vec4 color{1.0F, 0.95F, 0.88F, 0.0F};
    std::array<std::uint32_t, 4> parameters{1U, 0U, 0U, 0U}; // castsShadow, reserved
};

struct alignas(16) RenderEnvironment {
    Vec4 skyColorIntensity{1.0F, 1.0F, 1.0F, 1.0F};
    Vec4 groundColorIntensity{0.72F, 0.66F, 0.58F, 0.8F};
    Vec4 parameters{1.0F, 0.0F, 0.0F, 0.0F}; // exposure compensation, rotation radians, probe count, max LOD
};
struct alignas(16) RenderReflectionProbe {
    Vec4 positionRadius{}; // xyz world position, w positive GPU-representable blend radius
    Vec4 tintIntensity{1.0F, 1.0F, 1.0F, 1.0F};
};


struct alignas(8) LightTileHeader {
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
};

struct TiledLightLists {
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    std::uint32_t overflowCount = 0;
    std::vector<LightTileHeader> headers;
    std::vector<std::uint32_t> indices;
};

struct ShadowAtlasAssignment {
    std::array<std::int32_t, kMaximumLocalLights> localLightSlots{};
    std::uint32_t directionalCascadeCount = 0;
    std::uint32_t localShadowCount = 0;
    std::uint32_t overflowCount = 0;
};

[[nodiscard]] std::uint32_t packUnitFloat(float value) noexcept;
[[nodiscard]] float unpackUnitFloat(std::uint32_t value) noexcept;

void validateLocalLights(std::span<const RenderLocalLight> lights);
[[nodiscard]] TiledLightLists buildTiledLightLists(
    std::span<const RenderLocalLight> lights, const Mat4& viewProjection,
    std::uint32_t width, std::uint32_t height);
[[nodiscard]] ShadowAtlasAssignment assignShadowAtlasSlots(
    const RenderDirectionalLight& directional,
    std::span<const RenderLocalLight> lights) noexcept;

void validateReflectionProbes(
    std::span<const RenderReflectionProbe> probes);
static_assert(sizeof(RenderLocalLight) == 64);
static_assert(offsetof(RenderLocalLight, positionRange) == 0);
static_assert(offsetof(RenderLocalLight, colorIntensity) == 16);
static_assert(offsetof(RenderLocalLight, directionOuterCone) == 32);
static_assert(offsetof(RenderLocalLight, parameters) == 48);
static_assert(sizeof(RenderDirectionalLight) == 48);
static_assert(sizeof(RenderEnvironment) == 48);
void validateDirectionalLight(const RenderDirectionalLight& light);
void validateEnvironment(const RenderEnvironment& environment);
static_assert(sizeof(RenderReflectionProbe) == 32);
static_assert(sizeof(LightTileHeader) == 8);

} // namespace ve
