#version 450
#extension GL_GOOGLE_include_directive : require
#if VE_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif

#include "common/scene_uniforms.glsl"
#include "common/pbr.glsl"
#include "common/lighting.glsl"

#if VE_BINDLESS
layout(set = 0, binding = 3) uniform sampler2D materialTextures[];
#else
layout(set = 0, binding = 3) uniform sampler2D materialTextures[3];
#endif


layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vWorldNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) flat in vec4 vAlbedoRoughness;
layout(location = 4) flat in vec4 vEmissiveMetallic;
layout(location = 5) flat in vec4 vMaterialFlags;
layout(location = 6) in vec4 vWorldTangent;
layout(location = 7) flat in uvec4 vTextureIndices;

layout(location = 0) out vec4 outColor;


vec4 sampleMaterialTexture(uint role) {
#if VE_BINDLESS
    return texture(materialTextures[nonuniformEXT(vTextureIndices[role])], vUv);
#else
    if (role == 0U) return texture(materialTextures[0], vUv);
    if (role == 1U) return texture(materialTextures[1], vUv);
    return texture(materialTextures[2], vUv);
#endif
}
bool hasMaterialTexture(uint bit) {
    return (vTextureIndices.w & bit) != 0U;
}
float sampleShadowView(uint viewIndex, vec3 worldPosition, vec3 n, vec3 l) {
    vec4 clip =
        lighting.shadowViewProjection[viewIndex] * vec4(worldPosition, 1.0);
    if (clip.w <= 0.0) return 1.0;
    vec3 projected = clip.xyz / clip.w;
    vec2 tileUv = projected.xy * 0.5 + 0.5;
    if (any(lessThan(tileUv, vec2(0.0))) ||
        any(greaterThan(tileUv, vec2(1.0))) ||
        projected.z <= 0.0 || projected.z >= 1.0) {
        return 1.0;
    }
    vec4 atlasRect = lighting.shadowUvScaleBias[viewIndex];
    vec2 atlasUv = tileUv * atlasRect.xy + atlasRect.zw;
    float slopeBias =
        mix(0.00008, 0.0008, 1.0 - clamp(dot(n, l), 0.0, 1.0));
    return texture(shadowAtlas,
                   vec3(atlasUv, min(projected.z + slopeBias, 1.0)));
}

float directionalShadowVisibility(vec3 worldPosition, vec3 n, vec3 l) {
    if (lighting.directionalParameters.x == 0U) return 1.0;
    float viewDepth =
        (lighting.viewProjection * vec4(worldPosition, 1.0)).w;
    if (viewDepth <= 0.0 || viewDepth > lighting.cascadeSplits.w)
        return 1.0;
    uint cascade = viewDepth <= lighting.cascadeSplits.x ? 0U
        : viewDepth <= lighting.cascadeSplits.y ? 1U
        : viewDepth <= lighting.cascadeSplits.z ? 2U
                                                : 3U;
    float visibility =
        sampleShadowView(cascade, worldPosition, n, l);
    if (cascade < 3U) {
        float previousSplit = cascade == 0U ? 0.0
            : cascade == 1U ? lighting.cascadeSplits.x
                            : lighting.cascadeSplits.y;
        float split = cascade == 0U ? lighting.cascadeSplits.x
            : cascade == 1U ? lighting.cascadeSplits.y
                            : lighting.cascadeSplits.z;
        float blendStart = mix(previousSplit, split, 0.9);
        float blend = smoothstep(blendStart, split, viewDepth);
        if (blend > 0.0) {
            visibility = mix(
                visibility,
                sampleShadowView(cascade + 1U, worldPosition, n, l),
                blend);
        }
    }
    return visibility;
}

float localShadowVisibility(LocalLight light, vec3 worldPosition,
                            vec3 n, vec3 l) {
    if (light.parameters.w == 0U) return 1.0;
    uint viewIndex = light.parameters.w - 1U;
    if (viewIndex >= lighting.counts.w) return 1.0;
    return sampleShadowView(viewIndex, worldPosition, n, l);
}



const uint MATERIAL_STANDARD = 0U;
const uint MATERIAL_MASKED = 1U;
const uint MATERIAL_CLEAR_COAT = 2U;
const uint MATERIAL_FOLIAGE = 3U;
const uint MATERIAL_SKIN = 4U;
const uint MATERIAL_HAIR = 5U;
const uint MATERIAL_CLOTH = 6U;
const uint MATERIAL_EMISSIVE = 7U;
const uint MATERIAL_LANDSCAPE = 8U;
const uint MATERIAL_WATER = 9U;
const uint MATERIAL_FEATURE_ALPHA_MASK = 1U;
const uint MATERIAL_FEATURE_GROUND_GRID = 4U;

uint materialClass() {
    return uint(round(clamp(vMaterialFlags.y, 0.0,
                            float(MATERIAL_WATER))));
}
uint packedMaterialFeatures() {
    return uint(round(max(vMaterialFlags.x, 0.0)));
}
bool hasMaterialFeature(uint feature) {
    return (packedMaterialFeatures() & feature) != 0U;
}
float materialAlphaCutoff() {
    return float(packedMaterialFeatures() >> 8U) / 32767.5;
}
float landscapeVariation(vec2 coordinate) {
    vec2 cell = floor(coordinate);
    vec2 local = fract(coordinate);
    local = local * local * (3.0 - 2.0 * local);
    float a = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);
    float b = fract(sin(dot(cell + vec2(1.0, 0.0),
                            vec2(127.1, 311.7))) * 43758.5453);
    float c = fract(sin(dot(cell + vec2(0.0, 1.0),
                            vec2(127.1, 311.7))) * 43758.5453);
    float d = fract(sin(dot(cell + vec2(1.0),
                            vec2(127.1, 311.7))) * 43758.5453);
    return mix(mix(a, b, local.x), mix(c, d, local.x), local.y);
}
vec3 landscapeSurface(vec3 normal, vec2 coordinate, float height) {
    float variation = landscapeVariation(coordinate * 0.37);
    float slope = smoothstep(0.18, 0.62, 1.0 - normal.y);
    vec3 meadow = mix(vec3(0.075, 0.19, 0.055),
                      vec3(0.19, 0.31, 0.08), variation);
    vec3 rock = mix(vec3(0.19, 0.17, 0.145),
                    vec3(0.36, 0.32, 0.27), variation);
    float snow = smoothstep(125.0, 210.0, height) *
                 smoothstep(0.65, 0.92, normal.y);
    return mix(mix(meadow, rock, slope), vec3(0.72, 0.77, 0.80), snow);
}
vec2 environmentUv(vec3 direction) {
    float rotation = lighting.environmentParameters.y /
                     (2.0 * PI);
    return vec2(
        fract(atan(direction.z, direction.x) / (2.0 * PI) +
              0.5 + rotation),
        0.5 - asin(clamp(direction.y, -1.0, 1.0)) / PI);
}

vec4 environmentProbeBlend(vec3 worldPosition) {
    vec3 weightedTint = vec3(0.0);
    float totalWeight = 1.0;
    uint probeCount = uint(clamp(
        round(lighting.environmentParameters.z), 0.0, 4.0));
    for (uint probeIndex = 0U; probeIndex < probeCount;
         ++probeIndex) {
        ReflectionProbe probe =
            lighting.reflectionProbes[probeIndex];
        float normalizedDistance =
            length(worldPosition - probe.positionRadius.xyz) /
            probe.positionRadius.w;
        float weight =
            (1.0 - smoothstep(0.55, 1.0, normalizedDistance)) *
            probe.tintIntensity.w;
        weightedTint += probe.tintIntensity.rgb * weight;
        totalWeight += weight;
    }
    float normalization = 1.0 / totalWeight;
    return vec4(weightedTint * normalization, normalization);
}

vec3 environmentRadiance(vec3 direction, vec3 radiance,
                         vec4 probeBlend) {
    float skyWeight =
        clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 globalTint = mix(
        lighting.environmentGround.rgb *
            lighting.environmentGround.a,
        lighting.environmentSky.rgb *
            lighting.environmentSky.a,
        skyWeight);
    return radiance *
        (globalTint * probeBlend.a + probeBlend.rgb);
}


vec3 evaluateDirectLight(vec3 n, vec3 v, vec3 l, vec3 radiance,
                         vec3 albedo, float roughness, float metallic,
                         vec3 f0) {
    uint model = materialClass();
    float rawNdotL = dot(n, l);
    float ndotl = max(rawNdotL, 0.0);
    vec3 h = normalize(v + l);
    float ndotv = max(dot(n, v), 0.0);
    vec3 result = vec3(0.0);
    if (ndotl > 0.0) {
        float d = distributionGGX(n, h, roughness);
        float g = geometrySmith(ndotv, ndotl, roughness);
        vec3 f = fresnelSchlick(max(dot(h, v), 0.0), f0);
        vec3 specular =
            (d * g * f) / max(4.0 * ndotv * ndotl, 0.0001);
        vec3 kd = (1.0 - f) * (1.0 - metallic);
        result = (kd * albedo / PI + specular) * radiance * ndotl;
    }
    float strength = clamp(vMaterialFlags.z, 0.0, 1.0);
    if (model == MATERIAL_CLEAR_COAT && ndotl > 0.0) {
        float coatRoughness = mix(0.18, 0.04, strength);
        vec3 coatF = fresnelSchlick(max(dot(h, v), 0.0), vec3(0.04));
        float coatD = distributionGGX(n, h, coatRoughness);
        float coatG = geometrySmith(ndotv, ndotl, coatRoughness);
        vec3 coat = coatD * coatG * coatF /
                    max(4.0 * ndotv * ndotl, 0.0001);
        result = result * (1.0 - 0.25 * strength) +
                 coat * radiance * ndotl * strength;
    } else if (model == MATERIAL_CLOTH && ndotl > 0.0) {
        float sheen = pow5(1.0 - max(dot(h, v), 0.0));
        result += albedo * radiance * ndotl * sheen *
                  (0.35 * strength);
    } else if (model == MATERIAL_HAIR && ndotl > 0.0) {
        vec3 tangent = normalize(vWorldTangent.xyz);
        float tangentDotH = dot(tangent, h);
        float sinTheta = sqrt(max(
            1.0 - tangentDotH * tangentDotH, 0.0));
        float longitudinal = pow(sinTheta, mix(24.0, 96.0, strength));
        result += radiance * ndotl * longitudinal *
                  mix(vec3(0.04), albedo, 0.35) * strength;
    } else if (model == MATERIAL_FOLIAGE ||
               model == MATERIAL_SKIN) {
        float wrap = (model == MATERIAL_FOLIAGE ? 0.5 : 0.25) *
                     strength;
        float wrapped = max((rawNdotL + wrap) / (1.0 + wrap), 0.0);
        result += albedo * radiance * max(wrapped - ndotl, 0.0) *
                  (1.0 - metallic) / PI;
    }
    return result;
}

vec3 evaluateLocalLight(LocalLight light, vec3 worldPosition, vec3 n, vec3 v,
                        vec3 albedo, float roughness, float metallic, vec3 f0) {
    vec3 toLight = light.positionRange.xyz - worldPosition;
    float distanceSquared = dot(toLight, toLight);
    float range = light.positionRange.w;
    float rangeSquared = range * range;
    if (distanceSquared >= rangeSquared ||
        distanceSquared <= 0.000001)
        return vec3(0.0);
    uint model = materialClass();
    float wrap = model == MATERIAL_FOLIAGE
        ? 0.5 * clamp(vMaterialFlags.z, 0.0, 1.0)
        : model == MATERIAL_SKIN
            ? 0.25 * clamp(vMaterialFlags.z, 0.0, 1.0)
            : 0.0;
    float unnormalizedNdotL = dot(n, toLight);
    if (wrap == 0.0 && unnormalizedNdotL <= 0.0)
        return vec3(0.0);
    float inverseDistance = inversesqrt(distanceSquared);
    if (wrap > 0.0 &&
        unnormalizedNdotL * inverseDistance <= -wrap)
        return vec3(0.0);
    vec3 l = toLight * inverseDistance;
    float normalizedDistanceSquared =
        distanceSquared / rangeSquared;
    float rangeWindow = clamp(
        1.0 - normalizedDistanceSquared *
              normalizedDistanceSquared,
        0.0, 1.0);
    float inverseFalloffDistance = min(
        inverseDistance * inverseDistance, 100.0);
    float attenuation =
        rangeWindow * rangeWindow * inverseFalloffDistance;
    if (light.parameters.x == 1U) {
        float coneCosine = dot(-l, light.directionOuterCone.xyz);
        float innerCosine = float(light.parameters.z) / 65535.0;
        attenuation *= smoothstep(light.directionOuterCone.w, innerCosine,
                                  coneCosine);
    }
    if (attenuation <= 0.0) return vec3(0.0);
    vec3 radiance = light.colorIntensity.rgb *
                    light.colorIntensity.a * attenuation;
    float visibility =
        localShadowVisibility(light, worldPosition, n, l);
    return evaluateDirectLight(n, v, l, radiance, albedo, roughness,
                               metallic, f0) * visibility;
}

void main() {
    vec3 n = normalize(vWorldNormal);
    if (hasMaterialTexture(2U)) {
        vec3 t = normalize(vWorldTangent.xyz - n * dot(n, vWorldTangent.xyz));
        vec3 b = cross(n, t) * vWorldTangent.w;
        vec3 tangentNormal = sampleMaterialTexture(1U).xyz * 2.0 - 1.0;
        tangentNormal.xy *= clamp(vMaterialFlags.w, 0.0, 4.0);
        tangentNormal = normalize(tangentNormal);
        n = normalize(mat3(t, b, n) * tangentNormal);
    }
    vec3 v = normalize(scene.cameraPositionTime.xyz - vWorldPosition);
    vec3 albedo = max(vAlbedoRoughness.rgb, vec3(0.0));
    float roughness = clamp(vAlbedoRoughness.a, 0.04, 1.0);
    float metallic = clamp(vEmissiveMetallic.a, 0.0, 1.0);
    float ambientOcclusion = 1.0;
    vec4 baseSample = hasMaterialTexture(1U)
        ? sampleMaterialTexture(0U)
        : vec4(1.0);
    if (hasMaterialFeature(MATERIAL_FEATURE_ALPHA_MASK) &&
        baseSample.a < materialAlphaCutoff()) {
        discard;
    }
    albedo *= baseSample.rgb;
    if (hasMaterialTexture(4U)) {
        vec3 orm = sampleMaterialTexture(2U).rgb;
        ambientOcclusion = clamp(orm.r, 0.0, 1.0);
        roughness = clamp(roughness * orm.g, 0.04, 1.0);
        metallic = clamp(metallic * orm.b, 0.0, 1.0);
    }
    uint model = materialClass();
    if (model == MATERIAL_LANDSCAPE) {
        albedo *= landscapeSurface(n, vUv, vWorldPosition.y);
        roughness = max(roughness, 0.72);
        metallic = 0.0;
    } else if (model == MATERIAL_WATER) {
        float time = scene.cameraPositionTime.w;
        vec2 phase = vUv * 9.0;
        float waveX = sin(phase.x + time * 0.73) * 0.11 +
                      sin(phase.y * 0.61 + time * 1.07) * 0.07;
        float waveZ = cos(phase.y - time * 0.67) * 0.11 +
                      cos(phase.x * 0.53 - time * 0.91) * 0.07;
        n = normalize(n + vec3(waveX, 0.0, waveZ));
        float variation = landscapeVariation(vUv * 0.8);
        albedo = mix(vec3(0.012, 0.075, 0.105),
                     vec3(0.025, 0.18, 0.21), variation);
        roughness = 0.12;
        metallic = 0.06;
    }

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 directionalL =
        -lighting.directionalDirectionIntensity.xyz;
    float directionalVisibility =
        directionalShadowVisibility(vWorldPosition, n, directionalL);
    vec3 directional = evaluateDirectLight(
        n, v, directionalL,
        lighting.directionalColor.rgb *
            lighting.directionalDirectionIntensity.w,
        albedo, roughness, metallic, f0) * directionalVisibility;
    uvec2 tile = uvec2(gl_FragCoord.xy) / LIGHT_TILE_SIZE;
    LightTileHeader header =
        lightTileHeaders[tile.y * lighting.counts.y + tile.x];
    vec3 local = vec3(0.0);
    for (uint index = 0U; index < header.count; ++index) {
        uint lightIndex = lightTileIndices[header.offset + index];
        local += evaluateLocalLight(localLights[lightIndex],
                                    vWorldPosition, n, v, albedo,
                                    roughness, metallic, f0);
    }

    float ndotv = max(dot(n, v), 0.0);
    vec3 ambientF = fresnelSchlickRoughness(ndotv, f0, roughness);
    vec3 ambientKd = (1.0 - ambientF) * (1.0 - metallic);
    float maximumEnvironmentLod =
        lighting.environmentParameters.w;
    vec4 probeBlend =
        environmentProbeBlend(vWorldPosition);
    vec3 diffuseEnvironment = environmentRadiance(
        n, lighting.environmentDiffuseRadiance.rgb, probeBlend);
    vec3 reflectionDirection = reflect(-v, n);
    vec3 specularRadiance = textureLod(
        environmentMap, environmentUv(reflectionDirection),
        roughness * maximumEnvironmentLod).rgb;
    vec3 specularEnvironment = environmentRadiance(
        reflectionDirection, specularRadiance, probeBlend);
    vec3 ambientDiffuse =
        ambientKd * albedo * diffuseEnvironment;
    vec3 ambientSpecular = ambientF * specularEnvironment;
    vec3 ambient = (ambientDiffuse + ambientSpecular) *
                   ambientOcclusion;
    float groundGrid =
        hasMaterialFeature(MATERIAL_FEATURE_GROUND_GRID) ? gridMask(vUv) : 0.0;
    vec3 gridTint = vec3(0.08, 0.09, 0.10) * groundGrid;
    vec3 color = directional + local + ambient +
                 vEmissiveMetallic.rgb + gridTint;
    outColor = vec4(color, 1.0);
}
